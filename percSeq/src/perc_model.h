// perc_model.h - percSeq data: hits, rows, sequences, chance cells, and the engine tables.
//
// SPDX-License-Identifier: MIT
//
// percSeq is the percussion version of scalaSeq. A sequence has 16 rows. Each row is one sound
// (one MIDI note, a drum pad or a chop slice) with its own playback settings (Rate, Steps, Range,
// Direction, Swing) and its own hits. Hits of one row do not overlap; hits of different rows are
// independent. Nothing here allocates after construction, so the audio thread can edit and rebuild.

#ifndef PERCSEQ_MODEL_H
#define PERCSEQ_MODEL_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../../common/kept_hits.h"

namespace perc {

constexpr int kRows = 16;
constexpr int kMaxSteps = 64;
constexpr int kSlots = 8;           // hits that may start in one step of a row
constexpr int kMaxHits = 64;        // hits in one row
constexpr int kLanes = 4;           // chance lanes
constexpr int kSeqs = 32;
constexpr float kEps = 1e-6f;
constexpr float kMinLen = 0.05f;

// ---- rates (note value of one step), in quarter notes ---------------------------------------------
constexpr int kNumRates = 15;
inline const char* const* rateNames()
{
    static const char* n[kNumRates] = {"1/64", "1/32T", "1/32", "1/16T", "1/16Q", "1/16", "1/8T", "1/8Q",
                                       "1/8D", "1/8", "1/4T", "1/4", "1/4D", "1/2", "1 bar"};
    return n;
}
inline double rateQ(int i)
{
    static const double q[kNumRates] = {1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6, 1.0 / 5, 1.0 / 4, 1.0 / 3, 2.0 / 5,
                                        3.0 / 4, 1.0 / 2, 2.0 / 3, 1.0, 3.0 / 2, 2.0, 4.0};
    return q[std::max(0, std::min(kNumRates - 1, i))];
}
constexpr int kDefRate = 5;   // 1/16

// ---- 1.3: ratio rates (Rate Mode = Ratio: the step is the musical Rate divided by Ratio + Fine) ----
constexpr int kNumRatios = 15, kDefRatio = 6;   // 1
inline const char* const* ratioNames()
{
    static const char* n[kNumRatios] = {"0.25", "0.33", "0.5", "0.67", "0.75", "0.8", "1", "1.25", "1.33", "1.5",
                                        "1.67", "2", "2.5", "3", "4"};
    return n;
}
inline double ratioOf(int i, int fine)   // fine in 1/100, -10..10
{
    static const double r[kNumRatios] = {0.25, 1.0 / 3, 0.5, 2.0 / 3, 0.75, 0.8, 1.0, 1.25, 4.0 / 3, 1.5,
                                         5.0 / 3, 2.0, 2.5, 3.0, 4.0};
    return std::max(0.05, r[std::max(0, std::min(kNumRatios - 1, i))] + fine / 100.0);
}
// step length in quarter notes of a rate setting (mode 0 musical, 1 ratio)
inline double stepQOf(int rate, int mode, int ratio, int fine)
{
    return mode == 1 ? rateQ(rate) / ratioOf(ratio, fine) : rateQ(rate);
}

// ---- 1.3: CC numbers for Lock CC and lane CC (0 = off). Skipped (user): 0-5, 7, 10, 11, 32, 64-68,
// 98-101, 120-127 (bank select, standard controllers the track MIDI filter knows, NRPN/RPN, channel mode).
inline bool ccAllowed(int c)
{
    return c > 0 && c < 120 && !(c <= 5 || c == 7 || c == 10 || c == 11 || c == 32 || (c >= 64 && c <= 68) || (c >= 98 && c <= 101));
}
inline int numCCChoices() { int n = 1; for (int c = 0; c < 128; ++c) if (ccAllowed(c)) ++n; return n; }   // + off
inline int ccOfIndex(int i)   // 0 = off
{
    if (i <= 0) return 0;
    for (int c = 0; c < 128; ++c) if (ccAllowed(c) && --i == 0) return c;
    return 0;
}
inline int indexOfCC(int cc)
{
    if (!ccAllowed(cc)) return 0;
    int i = 0;
    for (int c = 0; c <= cc; ++c) if (ccAllowed(c)) ++i;
    return i;
}

// ---- chance ops ----------------------------------------------------------------------------------
// The scalaSeq ops without the glide ops; the pitch ops change the sound (the row) of a hit.
enum Unit : uint8_t { U_NONE, U_ST, U_TO, U_PTS, U_GATE, U_X, U_ROWS, U_REP, U_RND, U_PCT, U_FLAM, U_CC };
struct OpInfo { const char* name; int lo, hi, def; Unit unit; };
enum Op : int
{
    O_NONE = 0, O_MUTE, O_UNMUTE, O_SKIP, O_UNSKIP, O_JMPB, O_JMPR, O_JMPF, O_JMPTO,
    O_VELM, O_VELP, O_VELMAX, O_GATEM, O_GATEP, O_TIE, O_RATCH,
    O_SNDP, O_SNDM, O_SNDRP, O_SNDRM, O_SNDR, O_DELAY, O_REPEAT, O_REVERSE,
    O_SNDHOLD, O_SNDPREV, O_SNDNEXT, O_VELR, O_DELAYP, O_FLAM, O_SMPREV, O_CC,
    kNumOps
};
inline const OpInfo& opInfo(int op)
{
    static const OpInfo ops[kNumOps] = {
        {"-", 0, 0, 0, U_NONE},
        {"MUTE", 0, 0, 0, U_NONE}, {"UNMUTE", 0, 0, 0, U_NONE}, {"SKIP", 0, 0, 0, U_NONE}, {"UNSKIP", 0, 0, 0, U_NONE},
        {"JMP<", 1, 16, 1, U_ST}, {"JMP?", 0, 0, 0, U_NONE}, {"JMP>", 1, 16, 1, U_ST}, {"JMP TO", 1, 64, 1, U_TO},
        {"VEL-", 1, 100, 30, U_PTS}, {"VEL+", 1, 100, 30, U_PTS}, {"VEL MAX", 0, 0, 0, U_NONE},
        {"GATE-", 0, 11, 3, U_GATE}, {"GATE+", 0, 11, 3, U_GATE}, {"TIE", 0, 0, 0, U_NONE},
        {"RATCH", 2, 8, 2, U_X},
        {"SND+", 1, 15, 1, U_ROWS}, {"SND-", 1, 15, 1, U_ROWS},
        {"SND?+", 1, 15, 0, U_ROWS}, {"SND?-", 1, 15, 0, U_ROWS}, {"SND?", 1, 15, 0, U_ROWS},
        {"DELAY", 0, 7, 3, U_GATE}, {"REPEAT", 1, 8, 1, U_REP}, {"REVERSE", 0, 0, 0, U_NONE},
        {"SND HOLD", 0, 0, 0, U_NONE}, {"SND PREV", 0, 0, 0, U_NONE}, {"SND NEXT", 0, 0, 0, U_NONE},
        {"VEL?", 1, 100, 20, U_RND},
        {"DELAY %", 1, 100, 25, U_PCT},
        {"FLAM", 1, 48, 8, U_FLAM}, {"SMP REV", 0, 0, 0, U_NONE}, {"CC", 0, 127, 64, U_CC}};
    return ops[(op >= 0 && op < kNumOps) ? op : 0];
}
// Op knob order: the sequence stores the op id; the knob shows DELAY % after DELAY and the
// percussion ops at the end.
constexpr int kOpOrder[kNumOps] = {O_NONE, O_MUTE, O_UNMUTE, O_SKIP, O_UNSKIP, O_JMPB, O_JMPR, O_JMPF, O_JMPTO,
                                   O_VELM, O_VELP, O_VELMAX, O_GATEM, O_GATEP, O_TIE, O_RATCH,
                                   O_SNDP, O_SNDM, O_SNDRP, O_SNDRM, O_SNDR, O_DELAY, O_DELAYP, O_REPEAT, O_REVERSE,
                                   O_SNDHOLD, O_SNDPREV, O_SNDNEXT, O_VELR, O_FLAM, O_SMPREV, O_CC};
inline int opOfChoice(int c) { return kOpOrder[(c >= 0 && c < kNumOps) ? c : 0]; }
inline int choiceOfOp(int op)
{
    for (int c = 0; c < kNumOps; ++c) if (kOpOrder[c] == op) return c;
    return 0;
}
// gate amounts in steps (GATE-, GATE+, DELAY)
inline float gateSteps(int i)
{
    static const float g[12] = {1.f / 16, 1.f / 8, 1.f / 6, 1.f / 4, 1.f / 3, 1.f / 2, 3.f / 4, 1, 1.5f, 2, 3, 4};
    return g[std::max(0, std::min(11, i))];
}
inline const char* gateName(int i)
{
    static const char* n[12] = {"1/16", "1/8", "1/6", "1/4", "1/3", "1/2", "3/4", "1", "1.5", "2", "3", "4"};
    return n[std::max(0, std::min(11, i))];
}
// PTN choices: index 0 = always; then A:B for B = 2..8, A = 1..B
constexpr int kNumPtns = 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8;
inline void ptnOf(int idx, int& a, int& b)
{
    a = 0; b = 0;
    if (idx <= 0) return;
    int k = 1;
    for (int bb = 2; bb <= 8; ++bb)
        for (int aa = 1; aa <= bb; ++aa, ++k)
            if (k == idx) { a = aa; b = bb; return; }
}

struct Cell
{
    uint8_t op = 0;      // 0 = none
    uint8_t ptn = 0;     // PTN choice index (0 = always)
    uint8_t amt = 0;     // amount in the op's unit (0 = default; for random sound ops 0 = the whole pool)
    uint8_t pad = 0;
    float prob = 0.0f;   // 0..1
};

struct Hit
{
    float st = 0, len = 1;   // steps of its row
    float v = 1;             // velocity 0..1
    float d = 0.5f;          // duty 0.05..0.95
    int8_t r = 1;            // ratchets 1..8
    int8_t fd = 0;           // fade -50..50
    int16_t nd = 0;          // 1.1: Nudge in 1/96 step; the hit's Move position is st - nd / 96
    Cell ch[kLanes];
    int8_t lock[4] = {-1, -1, -1, -1};   // 1.3: p-lock values 0..127, -1 = off (sent with Lock CC 1-4 of the sound's row)
};
// the hit layout of project formats PSQ1 and PSQ2 (no locks)
struct HitV2
{
    float st, len, v, d;
    int8_t r, fd;
    int16_t nd;
    Cell ch[kLanes];
};

// 1.3: a CC lane: one value per step, its own length, rate and direction; sends CC `cc` (0 = off)
constexpr int kCcLanes = 2;
struct CcLane
{
    uint8_t cc = 0, steps = 16, rate = kDefRate, mode = 0, ratio = kDefRatio, dir = 0, rest = 64;
    int8_t fine = 0;
    uint8_t val[kMaxSteps];
    CcLane() { std::memset(val, 64, sizeof val); }
};

struct Row
{
    int steps = 16;
    int rStart = 0, rEnd = 15;   // play range, whole steps
    float swing = 50;            // 50..75 %
    int rate = kDefRate;
    int dir = 0;                 // 0 forward, 1 reverse, 2 random
    int note = 36;               // the MIDI note the row plays
    float vel = 100;             // 0..100 %: scales the velocity of its hits
    int revCC = 102, ccNum = 14, ccRest = 64;
    bool on = true;              // 1.1: an off row stops and sends nothing
    int mode = 0, ratio = kDefRatio, fine = 0;   // 1.3: Rate Mode (0 musical, 1 ratio), Ratio, Fine (1/100)
    uint8_t lockCC[4] = {0, 0, 0, 0};            // 1.3: CC numbers of the locks (0 = off)
    CcLane lane[kCcLanes];                       // 1.3
    int count = 0;
    int kept = 0;                // 2026-11-22: kept hits after the active ones (common/kept_hits.h)
    Hit hits[kMaxHits];

    int hitAt(float pos) const
    {
        for (int i = 0; i < count; ++i)
            if (pos >= hits[i].st - kEps && pos < hits[i].st + hits[i].len - kEps) return i;
        return -1;
    }
    // room after st up to the next hit or the end (self = the hit's own index, or -1)
    float maxLen(float st, int self) const
    {
        float m = steps - st;
        for (int i = 0; i < count; ++i)
            if (i != self && hits[i].st >= st - kEps) m = std::min(m, hits[i].st - st);
        return std::max(0.0f, m);
    }
    float minStart(float st, int self) const   // end of the hit before st
    {
        float m = 0;
        for (int i = 0; i < count; ++i)
            if (i != self && hits[i].st < st - kEps) m = std::max(m, hits[i].st + hits[i].len);
        return m;
    }
    bool fits(float st, float len, int self) const
    {
        if (st < -kEps || st + len > steps + kEps) return false;
        for (int i = 0; i < count; ++i)
        {
            if (i == self) continue;
            if (st < hits[i].st + hits[i].len - kEps && hits[i].st < st + len - kEps) return false;
        }
        return true;
    }
    void remove(int i)
    {
        if (i < 0 || i >= count) return;
        for (int k = i; k < count + kept - 1; ++k) hits[k] = hits[k + 1];   // the kept hits move down too
        --count;
    }
    bool full() const { return count + kept >= kMaxHits; }   // kept hits count toward the 64
    // insert, keeping the order; returns the new index or -1
    int insert(const Hit& h)
    {
        if (full()) return -1;
        for (int k = count + kept; k > count; --k) hits[k] = hits[k - 1];   // make room: the kept hits move up
        int i = count;
        while (i > 0 && hits[i - 1].st > h.st) { hits[i] = hits[i - 1]; --i; }
        hits[i] = h;
        ++count;
        return i;
    }
    // 2026-11-22: hits outside Steps are kept (common/kept_hits.h), not deleted
    static bool sameButLen(const Hit& a, const Hit& b)
    {
        if (a.v != b.v || a.d != b.d || a.r != b.r || a.fd != b.fd || a.nd != b.nd) return false;
        for (int l = 0; l < kLanes; ++l)
            if (a.ch[l].op != b.ch[l].op || a.ch[l].ptn != b.ch[l].ptn || a.ch[l].amt != b.ch[l].amt
                || a.ch[l].prob != b.ch[l].prob) return false;
        for (int k = 0; k < 4; ++k) if (a.lock[k] != b.lock[k]) return false;
        return true;
    }
    // all hits at full length (sorted) into out[kMaxHits]; returns how many
    int logical(Hit* out) const { return keephits::logical(hits, count, kept, steps, kMinLen, kEps, sameButLen, out); }
    // hits from a logical list for the current Steps
    void fromList(const Hit* list, int n) { keephits::split(list, n, steps, kMinLen, kMaxHits, hits, count, kept); fitRange(); }
    // a new Steps: the hits outside it are kept; scratch: kMaxHits hits
    void setSteps(int n, Hit* scratch) { const int L = logical(scratch); steps = n; fromList(scratch, L); }
    int keptOutside() const { return keephits::outside(hits, count, kept, steps, kMinLen); }
    void fitRange()
    {
        rEnd = std::min(rEnd, steps - 1);
        rStart = std::min(rStart, rEnd);
    }
};

struct Sequence
{
    Row rows[kRows];
    // 2026-11-25 (user): Max Steps: no row (and no CC lane) plays more than this many of its own steps (its
    // Steps is kept); Restart: all rows start again together every maxSteps x rateQ(restartRate), counted
    // from the song start. New sequences: 7, off, 1/16 (projects saved before: 64, off, 1/16).
    int maxSteps = 7;
    bool restart = false;
    int restartRate = kDefRate;
    Sequence() { for (int r = 0; r < kRows; ++r) { rows[r].note = 36 + r; rows[r].revCC = 102 + r; rows[r].ccNum = 14 + r; } }
};

// ---- engine tables (rebuilt from a Row; read by the row's engine) ---------------------------------
struct Slot
{
    float off = 0, len = 0;   // len 0 = no more hits in this step
    int rat = 1;
    float vel = 0, duty = 0.5f;
    int fade = 0;
    int hi = 0;               // hit index
    float room = 0;           // steps to the next hit or the end
};

struct EngCell { int op = 0; float prob = 0; int pa = 0, pb = 0; float amt = 0; };

struct Tables
{
    int steps = 16, rs = 0, re = 15;
    float swing = 50;
    Slot fwd[kMaxSteps][kSlots];
    Slot rev[kMaxSteps][kSlots];
    EngCell cells[kMaxHits][kLanes];
};

// amount of a cell in the engine's units (gate ops: steps; random sound ops: 0 = the whole pool);
// steps, rs, re: the row as it plays (2026-11-25: Max Steps)
inline float opValueIn(const Cell& c, int steps, int rs, int re)
{
    const OpInfo& o = opInfo(c.op);
    if (o.unit == U_NONE) return 0;
    int a = c.amt;
    int hi = o.hi;
    if (o.unit == U_TO) hi = std::min(o.hi, steps);
    if (o.unit == U_ST) hi = std::max(1, std::min(o.hi, re - rs));
    if (o.unit == U_ROWS && (c.op == O_SNDRP || c.op == O_SNDRM || c.op == O_SNDR) && a == 0) return 0;
    if (!a && o.unit != U_GATE && o.unit != U_CC) a = o.def;
    a = std::max(o.lo, std::min(hi, a));
    return o.unit == U_GATE ? gateSteps(a) : (float)a;
}
inline float opValue(const Cell& c, const Row& r) { return opValueIn(c, r.steps, r.rStart, r.rEnd); }
// 2026-11-25: the row as it plays under Max Steps: steps, Range Start, Range End
inline int playSteps(const Row& s, int cap) { return std::max(1, std::min(s.steps, std::max(1, cap))); }
inline int playEnd(const Row& s, int cap) { return std::min(s.rEnd, playSteps(s, cap) - 1); }
inline int playStart(const Row& s, int cap) { return std::min(s.rStart, playEnd(s, cap)); }

// cap: Max Steps of the row's sequence (2026-11-25): the row plays min(Steps, cap) steps; a hit that
// crosses the end plays cut at the end, a hit after it does not play
inline void buildTables(const Row& s, Tables& T, int cap = kMaxSteps)
{
    const int n = playSteps(s, cap), re = playEnd(s, cap), rs = playStart(s, cap);
    T.steps = n; T.rs = rs; T.re = re; T.swing = s.swing;
    for (int k = 0; k < kMaxSteps; ++k)
        for (int j = 0; j < kSlots; ++j) { T.fwd[k][j] = Slot(); T.rev[k][j] = Slot(); }
    auto put = [&](Slot& sl, float off, float len, int i) {
        const Hit& h = s.hits[i];
        sl.off = off; sl.len = len; sl.rat = h.r; sl.vel = h.v;
        sl.duty = h.d; sl.fade = h.fd; sl.hi = i;
        sl.room = std::min(s.maxLen(h.st, i), std::max(0.0f, (float)n - h.st));
    };
    int fc[kMaxSteps] = {0}, rc[kMaxSteps] = {0};
    for (int i = 0; i < s.count; ++i)
    {
        const Hit& h = s.hits[i];
        const int k = (int)std::floor(h.st + kEps);
        if (k < 0 || k >= n || fc[k] >= kSlots) continue;
        put(T.fwd[k][fc[k]++], std::max(0.0f, h.st - k), std::min(h.len, (float)n - h.st), i);
    }
    // reverse: a hit [a, b) plays mirrored, starting when the backward playhead reaches b;
    // hits starting in the play range are clipped to its end; latest end first in each step
    struct R { float b; int i; };
    R rv[kMaxHits];
    int nr = 0;
    for (int i = 0; i < s.count; ++i)
    {
        const Hit& o = s.hits[i];
        if (o.st < rs - kEps || o.st >= re + 1 - kEps) continue;
        rv[nr++] = {std::min(o.st + o.len, (float)(re + 1)), i};
    }
    std::sort(rv, rv + nr, [](const R& x, const R& y) { return x.b > y.b; });
    for (int q = 0; q < nr; ++q)
    {
        const float b = rv[q].b, a = s.hits[rv[q].i].st;
        const int k = (int)std::ceil(b - kEps) - 1;
        if (k < 0 || k >= n || rc[k] >= kSlots) continue;
        put(T.rev[k][rc[k]++], k + 1 - b, b - a, rv[q].i);
    }
    for (int i = 0; i < s.count; ++i)
        for (int l = 0; l < kLanes; ++l)
        {
            const Cell& c = s.hits[i].ch[l];
            EngCell& e = T.cells[i][l];
            e.op = c.op; e.prob = c.prob; ptnOf(c.ptn, e.pa, e.pb);
            e.amt = opValueIn(c, n, rs, re);
        }
}

// ---- the sound pool (POOL page) ------------------------------------------------------------------
struct Pool
{
    int list[kRows];
    int n = 0;
    void set(const bool* on) { n = 0; for (int r = 0; r < kRows; ++r) if (on[r]) list[n++] = r; }
    // k pool rows up (k > 0) or down from row `from`, wrapping in the pool. A row outside the pool
    // counts from where it would sit: +1 is the first pool row above it, -1 the first below.
    int step(int from, int k) const
    {
        if (n <= 0 || k == 0) return from;
        int i = -1;
        for (int q = 0; q < n; ++q) if (list[q] == from) i = q;
        if (i < 0)
        {
            int below = 0;
            for (int q = 0; q < n; ++q) if (list[q] < from) ++below;
            i = k > 0 ? below - 1 : below;
        }
        const int j = ((i + k) % n + n) % n;
        return list[j];
    }
};

} // namespace perc

#endif
