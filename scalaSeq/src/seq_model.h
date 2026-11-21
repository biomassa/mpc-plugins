// seq_model.h - scalaSeq data: notes, chance cells, sequences, tuning, and the engine tables.
//
// SPDX-License-Identifier: MIT
//
// Port of the data model of dingus.pgRoll.js (Max v8ui) to fixed-size C++ structures.
// Nothing here allocates after construction, so the audio thread can edit and rebuild.
//
// Pitch: a note's pitch is a degree index 0..511. kCenter is scale degree 0 of period 0, which
// sounds at Root (+ Root Tune). Index kCenter + k is the k-th degree above it.

#ifndef SCALASEQ_MODEL_H
#define SCALASEQ_MODEL_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../../common/scala.h"

namespace seq {

constexpr int kMaxSteps = 64;
constexpr int kSlots = 16;          // notes that may start in one step
constexpr int kMaxNotes = 256;      // notes in one sequence
constexpr int kLanes = 4;           // chance lanes
constexpr int kSeqs = 32;
constexpr int kPitchN = 512;        // degree indices
constexpr int kCenter = 256;        // degree 0 of period 0 (Root)
constexpr int kMaxMask = 64;        // scale mask entries kept
constexpr float kEps = 1e-6f;
constexpr float kMinLen = 0.05f;

// ---- chance ops (index = op number, same as dingus.pgRoll.js) ------------------------------------
enum Unit : uint8_t { U_NONE, U_ST, U_TO, U_PTS, U_GATE, U_X, U_DEG, U_REP, U_RND, U_PCT };
struct OpInfo { const char* name; int lo, hi, def; Unit unit; };
constexpr int kNumOps = 31;   // 2.4: GLIDE, NO GLIDE, DELAY % appended (not in the Max original)
inline const OpInfo& opInfo(int op)
{
    static const OpInfo ops[kNumOps] = {
        {"-", 0, 0, 0, U_NONE},
        {"MUTE", 0, 0, 0, U_NONE}, {"UNMUTE", 0, 0, 0, U_NONE}, {"SKIP", 0, 0, 0, U_NONE}, {"UNSKIP", 0, 0, 0, U_NONE},
        {"JMP<", 1, 16, 1, U_ST}, {"JMP?", 0, 0, 0, U_NONE}, {"JMP>", 1, 16, 1, U_ST}, {"JMP TO", 1, 64, 1, U_TO},
        {"VEL-", 1, 100, 30, U_PTS}, {"VEL+", 1, 100, 30, U_PTS}, {"VEL MAX", 0, 0, 0, U_NONE},
        {"GATE-", 0, 11, 3, U_GATE}, {"GATE+", 0, 11, 3, U_GATE}, {"TIE", 0, 0, 0, U_NONE},
        {"RATCH", 2, 8, 2, U_X},
        {"P+", 1, 64, 1, U_DEG}, {"P-", 1, 64, 1, U_DEG},
        {"P?+", 1, 64, 0, U_DEG}, {"P?-", 1, 64, 0, U_DEG}, {"P?", 1, 64, 0, U_DEG},
        {"DELAY", 0, 7, 3, U_GATE}, {"REPEAT", 1, 8, 1, U_REP}, {"REVERSE", 0, 0, 0, U_NONE},
        {"HOLD", 0, 0, 0, U_NONE}, {"P PREV", 0, 0, 0, U_NONE}, {"P NEXT", 0, 0, 0, U_NONE},
        {"VEL?", 1, 100, 20, U_RND},
        {"GLIDE", 10, 100, 50, U_PCT}, {"NO GLIDE", 0, 0, 0, U_NONE},
        {"DELAY %", 1, 100, 25, U_PCT}};
    return ops[(op >= 0 && op < kNumOps) ? op : 0];
}
// Op knob order (2.4.0). The sequence stores the op id; the knob shows the ops in this order,
// so DELAY % sits after DELAY and saved sequences keep their ops.
constexpr int kOpOrder[kNumOps] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                   21, 30, 22, 23, 24, 25, 26, 27, 28, 29};
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
    uint8_t amt = 0;     // amount in the op's unit (0 = default / full period for degree ops)
    uint8_t pad = 0;
    float prob = 0.0f;   // 0..1
};

struct Note
{
    float st = 0, len = 1;   // steps
    int16_t pitch = kCenter; // degree index
    int8_t r = 1;            // ratchets 1..8
    int8_t fd = 0;           // fade -50..50
    float v = 1;             // velocity 0..1
    float gl = 0;            // glide 0..1 steps
    float d = 0.5f;          // duty 0.05..0.95
    Cell ch[kLanes];
};

struct Sequence
{
    int steps = 16;
    int rStart = 0, rEnd = 15;   // play range, whole steps
    float swing = 50;            // 50..75 %
    int count = 0;
    Note notes[kMaxNotes];

    void sort() { std::sort(notes, notes + count, [](const Note& a, const Note& b) { return a.st < b.st; }); }
    int noteAt(float pos) const
    {
        for (int i = 0; i < count; ++i)
            if (pos >= notes[i].st - kEps && pos < notes[i].st + notes[i].len - kEps) return i;
        return -1;
    }
    // room after st up to the next note / the end (self = the note's own index, or -1)
    float maxLen(float st, int self) const
    {
        float m = steps - st;
        for (int i = 0; i < count; ++i)
            if (i != self && notes[i].st >= st - kEps) m = std::min(m, notes[i].st - st);
        return m;
    }
    // 2.0.1: notes of the same pitch must not overlap (other pitches may)
    float maxLenPitch(float st, int pitch, int self) const   // room up to the next same-pitch note
    {
        float m = steps - st;
        for (int i = 0; i < count; ++i)
            if (i != self && notes[i].pitch == pitch && notes[i].st >= st - kEps) m = std::min(m, notes[i].st - st);
        return std::max(0.0f, m);
    }
    float minStartPitch(float st, int pitch, int self) const   // end of the same-pitch note before st
    {
        float m = 0;
        for (int i = 0; i < count; ++i)
            if (i != self && notes[i].pitch == pitch && notes[i].st < st - kEps) m = std::max(m, notes[i].st + notes[i].len);
        return m;
    }
    bool fitsPitch(float st, float len, int pitch, int self) const
    {
        if (st < -kEps || st + len > steps + kEps) return false;
        for (int i = 0; i < count; ++i)
        {
            if (i == self || notes[i].pitch != pitch) continue;
            if (st < notes[i].st + notes[i].len - kEps && notes[i].st < st + len - kEps) return false;
        }
        return true;
    }
    bool fits(float st, float len, int self) const
    {
        if (st < -kEps || st + len > steps + kEps) return false;
        for (int i = 0; i < count; ++i)
        {
            if (i == self) continue;
            if (st < notes[i].st + notes[i].len - kEps && notes[i].st < st + len - kEps) return false;
        }
        return true;
    }
    void remove(int i)
    {
        if (i < 0 || i >= count) return;
        for (int k = i; k < count - 1; ++k) notes[k] = notes[k + 1];
        --count;
    }
    // insert, keeping the order; returns the new index or -1
    int insert(const Note& n)
    {
        if (count >= kMaxNotes) return -1;
        int i = count;
        while (i > 0 && notes[i - 1].st > n.st) { notes[i] = notes[i - 1]; --i; }
        notes[i] = n;
        ++count;
        return i;
    }
    void fitToSteps()
    {
        int w = 0;
        for (int i = 0; i < count; ++i)
        {
            Note n = notes[i];
            if (n.st >= steps - kEps) continue;
            if (n.st + n.len > steps) n.len = steps - n.st;
            if (n.len < kMinLen) continue;
            notes[w++] = n;
        }
        count = w;
        rEnd = std::min(rEnd, steps - 1);
        rStart = std::min(rStart, rEnd);
    }
    int prevNote(int i) const { return count > 1 ? (i - 1 + count) % count : i; }
    int nextNote(int i) const { return count > 1 ? (i + 1) % count : i; }
};

// ---- tuning --------------------------------------------------------------------------------------
// The scale is owned elsewhere (loaded by the worker thread); the audio thread only reads it.
struct Tuning
{
    const scala::Scale* sc = nullptr;
    double rootPitch = 60.0;        // MIDI pitch of degree 0 of period 0
    bool mask[kMaxMask];

    Tuning() { for (bool& m : mask) m = true; }
    bool valid() const { return sc && sc->valid(); }
    int size() const { return valid() ? sc->size() : 12; }
    int degreeOf(int v) const { const int n = size(); return ((v - kCenter) % n + n) % n; }
    int periodOf(int v) const { const int n = size(); return (int)std::floor((double)(v - kCenter) / n); }
    bool inScale(int v) const { const int d = degreeOf(v); return d >= kMaxMask || mask[d]; }
    double cents(int d) const { return valid() ? sc->cents[(size_t)d] : 100.0 * d; }
    double period() const { return valid() ? sc->cents[(size_t)sc->size()] : 1200.0; }
    double midiOf(int v) const { return rootPitch + (periodOf(v) * period() + cents(degreeOf(v))) / 100.0; }
    int snapPitch(int v) const
    {
        if (inScale(v)) return v;
        for (int k = 1; k < size(); ++k)
        {
            if (inScale(v - k)) return v - k;
            if (inScale(v + k)) return v + k;
        }
        return v;
    }
    int periodDegrees() const
    {
        int c = 0;
        for (int i = 0; i < size() && i < kMaxMask; ++i) c += mask[i] ? 1 : 0;
        return std::max(1, c);
    }
};

// ---- engine tables (rebuilt from a Sequence + Tuning; read by the engine) -------------------------
struct Slot
{
    float off = 0, len = 0;   // len 0 = no more notes in this step
    float midi = 0;
    int rat = 1;
    float vel = 0, duty = 0.5f, glide = 0;
    int pidx = 0;             // degree index
    int fade = 0;
    int ni = 0;               // note index
    float room = 0;           // steps to the next note or the end
};

struct EngCell { int op = 0; float prob = 0; int pa = 0, pb = 0; float amt = 0; };

struct Tables
{
    int steps = 16, rs = 0, re = 15;
    float swing = 50;
    Slot fwd[kMaxSteps][kSlots];
    Slot rev[kMaxSteps][kSlots];
    EngCell cells[kMaxNotes][kLanes];
    float pitchMidi[kPitchN];
    int rankLE[kPitchN], rankLT[kPitchN];   // scale degrees at or below / below an index
    int scaleList[kPitchN];
    int scaleCount = 0;
};

// amount of a cell in the engine's units (gate ops: steps; degree ops: 0 = a full period)
inline float opValue(const Cell& c, const Sequence& s, const Tuning& t, int noteIdx)
{
    const OpInfo& o = opInfo(c.op);
    if (c.op == 25 || c.op == 26)
    {
        const int nb = c.op == 25 ? s.prevNote(noteIdx) : s.nextNote(noteIdx);
        return (float)s.notes[nb].pitch;
    }
    if (o.unit == U_NONE) return 0;
    int a = c.amt;
    int hi = o.hi;
    if (o.unit == U_DEG) { hi = std::min(o.hi, t.periodDegrees()); if (!a) a = t.periodDegrees(); }
    if (o.unit == U_TO) hi = std::min(o.hi, s.steps);
    if (o.unit == U_ST) hi = std::max(1, std::min(o.hi, s.rEnd - s.rStart));
    if (!a && o.unit != U_GATE) a = o.def;
    a = std::max(o.lo, std::min(hi, a));
    return o.unit == U_GATE ? gateSteps(a) : (float)a;
}

inline void buildTables(const Sequence& s, const Tuning& t, Tables& T)
{
    T.steps = s.steps; T.rs = s.rStart; T.re = s.rEnd; T.swing = s.swing;
    for (int k = 0; k < kMaxSteps; ++k)
        for (int j = 0; j < kSlots; ++j) { T.fwd[k][j] = Slot(); T.rev[k][j] = Slot(); }
    auto put = [&](Slot& sl, float off, float len, int i) {
        const Note& nt = s.notes[i];
        sl.off = off; sl.len = len; sl.midi = (float)t.midiOf(nt.pitch); sl.rat = nt.r; sl.vel = nt.v;
        sl.duty = nt.d; sl.glide = nt.gl; sl.pidx = nt.pitch; sl.fade = nt.fd; sl.ni = i;
        sl.room = s.maxLen(nt.st, i);
    };
    int fc[kMaxSteps] = {0}, rc[kMaxSteps] = {0};
    for (int i = 0; i < s.count; ++i)
    {
        const Note& nt = s.notes[i];
        const int k = (int)std::floor(nt.st + kEps);
        if (k < 0 || k >= s.steps || fc[k] >= kSlots) continue;
        put(T.fwd[k][fc[k]++], std::max(0.0f, nt.st - k), nt.len, i);
    }
    // reverse: a note [a, b) plays mirrored, starting when the backward playhead reaches b;
    // notes starting in the play range are clipped to its end; latest end first in each step
    struct R { float b; int i; };
    R rv[kMaxNotes];
    int nr = 0;
    for (int i = 0; i < s.count; ++i)
    {
        const Note& o = s.notes[i];
        if (o.st < s.rStart - kEps || o.st >= s.rEnd + 1 - kEps) continue;
        rv[nr++] = {std::min(o.st + o.len, (float)(s.rEnd + 1)), i};
    }
    std::sort(rv, rv + nr, [](const R& x, const R& y) { return x.b > y.b; });
    for (int q = 0; q < nr; ++q)
    {
        const float b = rv[q].b, a = s.notes[rv[q].i].st;
        const int k = (int)std::ceil(b - kEps) - 1;
        if (k < 0 || k >= s.steps || rc[k] >= kSlots) continue;
        put(T.rev[k][rc[k]++], k + 1 - b, b - a, rv[q].i);
    }
    for (int i = 0; i < s.count; ++i)
        for (int l = 0; l < kLanes; ++l)
        {
            const Cell& c = s.notes[i].ch[l];
            EngCell& e = T.cells[i][l];
            e.op = c.op; e.prob = c.prob; ptnOf(c.ptn, e.pa, e.pb);
            e.amt = opValue(c, s, t, i);
        }
    int cnt = 0;
    for (int i = 0; i < kPitchN; ++i)
    {
        T.pitchMidi[i] = (float)t.midiOf(i);
        T.rankLT[i] = cnt;
        if (t.inScale(i)) T.scaleList[cnt++] = i;
        T.rankLE[i] = cnt;
    }
    T.scaleCount = cnt;
}

} // namespace seq

#endif
