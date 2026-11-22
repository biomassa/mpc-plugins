// perc_engine.h - the engine of one percSeq row: the scalaSeq engine (a port of the gen~ engine of
// dingus.pitchGateSeq) without pitch and glide.
//
// SPDX-License-Identifier: MIT
//
// One call of tick() per audio sample (or per catch-up sub-tick). It reads the row's tables
// (perc_model.h) and reports note on / note off moments, the hit's velocity, and, when a hit
// starts, what its chance ops decided: the sound (row) it plays, sample reverse, a CC value and a
// flam. The plugin turns these into MIDI.
//
// CPU: per tick only comparisons and a few multiplies. Chance rolls and table reads happen once
// per hit; FADE uses std::pow once per ratchet hit.

#ifndef PERCSEQ_ENGINE_H
#define PERCSEQ_ENGINE_H

#include <cmath>
#include <cstdint>

#include "perc_model.h"

namespace perc {

// What the chance ops decided for a hit, at the moment it starts.
struct Fire
{
    int sound = 0;       // the row whose note plays
    bool rev = false;    // SMP REV
    int cc = -1;         // CC op value, -1 = none
    int flam = 0;        // FLAM distance in 1/96 step, 0 = none
    int hit = -1;        // hit index in the row
};

struct RowOut
{
    bool on = false, off = false;   // off before on when both are set (a hit cuts the one before)
    float vel = 0;                  // velocity of the note that goes on (0..1, before the row Velocity)
    bool start = false;             // a new hit started (fire is valid)
    Fire fire;
    int step = -1;                  // current step
};

// Shared state for the sound ops (owned by the plugin).
struct SoundCtx
{
    const Pool* pool = nullptr;
    int lastPlayed = -1;   // the sound the last hit played (any row)
    int lastHome = -1;     // the row of the last hit (any row)
    int (*nextRow)(void* user) = nullptr;   // the row whose hit comes next in time
    void* user = nullptr;
};

class Engine
{
public:
    // Parameters (set by the plugin)
    int dir = 0;            // 0 forward, 1 reverse, 2 random
    bool ptnBars = false;   // PTN counts bars (true) or loop repeats (false)
    int barCount = 0;       // bar counter from the transport
    int home = 0;           // this row

    void reset()
    {
        const int d = dir, h = home; const bool p = ptnBars; const uint32_t r = rngState_;
        *this = Engine();
        dir = d; home = h; ptnBars = p; rngState_ = r ^ (uint32_t)(0x9E3779B9u * (unsigned)(h + 1));
        if (rngState_ == 0) rngState_ = 1;
    }
    // Start again (transport start, jump, sequence switch): the next tick begins a step at once.
    void restart() { rst_ = 1; prevsub_ = 1.0; act_ = 0; qon_ = 0; }
    void seed(uint32_t s) { rngState_ = s ? s : 1; }

    // phase: position in the row's loop 0..1. running: transport plays. T: the row's tables.
    RowOut tick(double phase, bool running, const Tables& T, SoundCtx& sc)
    {
        RowOut out;
        const int n = std::max(1, std::min(kMaxSteps, T.steps));
        const int d = ((dir % 3) + 3) % 3;
        // REVERSE: the other way round until the loop restarts (random stays random)
        int d2 = (revf_ > 0 && d < 2) ? 1 - d : d;
        const float msw = std::max(50.0f, std::min(75.0f, T.swing)) / 100.0f;

        // position inside the current step, with swing (pairs of steps)
        const double pos0 = phase * n;
        double pos = pos0;
        const double pp = std::floor(pos0 / 2);
        const double pu = pos0 - pp * 2;
        if (pp * 2 + 1 < n)
            pos = pp * 2 + ((pu < 2 * msw) ? pu / (2 * msw) : 1 + (pu - 2 * msw) / (2 - 2 * msw));
        const double sub = pos - std::floor(pos);
        const double ds = sub - prevsub_;
        const bool newstep = ds < -0.5;
        double adv = newstep ? ds + 1 : ds;
        if (adv < 0) adv = 0;
        prevsub_ = sub;
        posSteps = pos;

        if (act_)
        {
            el_ += adv;
            if (el_ >= glen_ || !running) act_ = 0;
        }

        const int lo = std::max(0, std::min(n - 1, T.rs));
        const int hi = std::max(lo, std::min(n - 1, T.re));
        const int span = hi - lo + 1;
        int cnt = ptnBars ? barCount : std::max(reps_, 0);

        bool fire = false;
        const Slot* fs = nullptr;
        float flen = 0, fcut = 64;

        if (newstep && running)
        {
            int nxt;
            bool wrapnow = false;
            if (rst_)
            {
                // forward playback starts at the step the position points to
                const int at = (int)std::floor(pos);
                nxt = (d == 0 && at >= lo && at <= hi) ? at : ((d == 1) ? hi : lo);
                rst_ = 0; reps_ = -1; jt_ = -1; rptn_ = 0; rpting_ = 0; revf_ = 0; qon_ = 0; d2 = d;
            }
            else if (rptn_ > 0) { nxt = cur_; rptn_--; rpting_ = 1; }
            else if (jt_ >= 0) { nxt = std::max(lo, std::min(hi, jt_)); jt_ = -1; rpting_ = 0; }
            else if (d2 == 0) { rpting_ = 0; nxt = cur_ + 1; if (nxt > hi || nxt < lo) wrapnow = true; }
            else if (d2 == 1) { rpting_ = 0; nxt = cur_ - 1; if (nxt < lo || nxt > hi) wrapnow = true; }
            else { rpting_ = 0; nxt = lo + std::min((int)std::floor(rnd() * span), hi - lo); }
            const int first = (d == 1) ? hi : lo;
            if (wrapnow)
            {
                nxt = first;
                if (revf_ > 0)
                {
                    nxt = std::max(lo, std::min(hi, (d == 1) ? hi - 1 : lo + 1));
                    reps_ += (nxt == first) ? 0 : 1;
                }
                revf_ = 0;
                d2 = d;
            }
            if (rpting_ == 0 && revf_ == 0 && nxt == first) reps_++;
            cnt = ptnBars ? barCount : std::max(reps_, 0);
            // SKIP / UNSKIP of the hits starting in the step (a repeated step is not skipped)
            bool ok = rpting_ != 0;
            for (int i = 0; i < n * (1 - rpting_); ++i)
            {
                bool sk = false;
                for (int j = 0; j < kSlots; ++j)
                {
                    const Slot& sl = T.fwd[nxt][j];
                    if (sl.len <= 0) break;
                    int s = 0;
                    for (int k = 0; k < kLanes; ++k)
                    {
                        const EngCell& c = T.cells[sl.hi][k];
                        if (c.op == O_SKIP || c.op == O_UNSKIP)
                            if (ptnOk(c, cnt) && rnd() < std::min(c.prob, 1.0f)) s = (c.op == O_SKIP) ? 1 : 0;
                    }
                    if (s) sk = true;
                }
                if (!sk) { ok = true; break; }
                if (d2 == 0) nxt = (nxt + 1 > hi) ? lo : nxt + 1;
                else if (d2 == 1) nxt = (nxt - 1 < lo) ? hi : nxt - 1;
                else nxt = lo + std::min((int)std::floor(rnd() * span), hi - lo);
            }
            cur_ = nxt;
            slot_ = ok ? 0 : kSlots;
            rev_ = (d2 == 1);
        }

        // hits of the current step, in order of their offset into the step
        if (running && slot_ < kSlots && cur_ >= 0)
        {
            const Slot& sl = rev_ ? T.rev[cur_][slot_] : T.fwd[cur_][slot_];
            if (sl.len <= 0) slot_ = kSlots;
            else if (sub >= sl.off)
            {
                slot_++;
                fire = true;
                fs = &sl;
                fcut = (d2 == 0) ? (float)(hi + 1 - cur_ - sl.off) : 64.0f;
                flen = std::min(sl.len, fcut);
            }
        }

        if (fire && flen > 0)
        {
            float cv = fs->vel, clen = flen, dly = 0;
            int crat = std::max(fs->rat, 1);
            bool muted = false;
            Fire f;
            f.sound = home; f.hit = fs->hi;
            const float room = fs->room;
            const Pool* pl = sc.pool;
            for (int k = 0; k < kLanes; ++k)
            {
                const EngCell& c = T.cells[fs->hi][k];
                if (c.op <= 0) continue;
                if (!(ptnOk(c, cnt) && rnd() < std::min(c.prob, 1.0f))) continue;
                const float amt = c.amt;
                switch (c.op)
                {
                    case O_MUTE: muted = true; break;
                    case O_UNMUTE: muted = false; break;
                    case O_JMPB: jt_ = lo + wrapi(cur_ - (int)amt - lo, span); break;
                    case O_JMPR: jt_ = lo + std::min((int)std::floor(rnd() * span), hi - lo); break;
                    case O_JMPF: jt_ = lo + wrapi(cur_ + (int)amt - lo, span); break;
                    case O_JMPTO: jt_ = (int)amt - 1; break;
                    case O_VELM: cv = std::max(0.0f, cv - amt / 100); break;
                    case O_VELP: cv = std::min(1.0f, cv + amt / 100); break;
                    case O_VELMAX: cv = 1; break;
                    case O_GATEM: clen = std::max(clen - amt, 0.01f); break;
                    case O_GATEP: clen = std::min(clen + amt, std::max(room, clen)); break;
                    case O_TIE: clen = std::max(room, clen); break;
                    case O_RATCH: crat = (int)amt; break;
                    case O_SNDP: if (pl) f.sound = pl->step(f.sound, (int)amt); break;
                    case O_SNDM: if (pl) f.sound = pl->step(f.sound, -(int)amt); break;
                    case O_SNDRP: case O_SNDRM:
                        if (pl && pl->n > 0)
                        {
                            const int m = amt > 0 ? (int)amt : std::max(1, pl->n - 1);
                            const int k2 = 1 + std::min((int)std::floor(rnd() * m), m - 1);
                            f.sound = pl->step(f.sound, c.op == O_SNDRP ? k2 : -k2);
                        }
                        break;
                    case O_SNDR:
                        if (pl && pl->n > 0)
                        {
                            if (amt <= 0) f.sound = pl->list[std::min((int)std::floor(rnd() * pl->n), pl->n - 1)];
                            else
                            {
                                const int a = (int)amt;
                                const int rr = std::min((int)std::floor(rnd() * (2 * a + 1)), 2 * a) - a;
                                f.sound = pl->step(f.sound, rr);
                            }
                        }
                        break;
                    case O_DELAY: dly = amt; break;
                    case O_REPEAT: if (rpting_ == 0) rptn_ = std::max(rptn_, (int)amt); break;
                    case O_REVERSE: revf_ = (d < 2) ? 1 : 0; break;
                    case O_SNDHOLD: if (sc.lastPlayed >= 0) f.sound = sc.lastPlayed; break;
                    case O_SNDPREV: if (sc.lastHome >= 0) f.sound = sc.lastHome; break;
                    case O_SNDNEXT: if (sc.nextRow) { const int r = sc.nextRow(sc.user); if (r >= 0) f.sound = r; } break;
                    case O_VELR: cv = std::max(0.0f, std::min(1.0f, cv + (rnd() * 2 - 1) * amt / 100)); break;
                    case O_DELAYP: dly = amt / 100; break;
                    case O_FLAM: f.flam = (int)amt; break;
                    case O_SMPREV: f.rev = true; break;
                    case O_CC: f.cc = (int)amt; break;
                    default: break;
                }
            }
            // DELAY: the hit starts later and keeps its length
            clen = std::min(clen, fcut - dly);
            if (clen <= 0) muted = true;
            if (!muted)
            {
                qon_ = 1; qnew_ = 1; qd_ = dly; qv_ = cv; qlen_ = clen; qrat_ = crat;
                qduty_ = fs->duty; qfade_ = fs->fade; qfire_ = f;
            }
        }

        bool start = false;
        if (qon_)
        {
            if (!running) qon_ = 0;
            else
            {
                if (!qnew_) qd_ -= (float)adv;
                qnew_ = 0;
                if (qd_ <= 0) { qon_ = 0; start = true; }
            }
        }

        if (start)
        {
            act_ = 1; el_ = 0; glen_ = qlen_; grat_ = std::max(qrat_, 1); gvel_ = qv_;
            gduty_ = qduty_; gfade_ = qfade_; hitprev_ = -1;
            out.start = true;
            out.fire = qfire_;
        }

        bool high = false;
        bool newHit = false;
        float hvel = 0;
        if (act_)
        {
            // FADE: velocity per ratchet hit, exponential (computed once per hit)
            const int hit = std::min((int)std::floor(el_ / glen_ * grat_), grat_ - 1);
            if (hit != hitprev_)
            {
                hitprev_ = hit;
                newHit = true;
                hvelHit_ = gvel_;
                if (grat_ > 1 && gfade_ != 0)
                {
                    const float hn = (float)hit / (grat_ - 1);
                    const float fm = std::pow(0.01f, std::abs((float)gfade_) / 50.0f);
                    hvelHit_ = gvel_ * (gfade_ < 0 ? std::pow(fm, 1 - hn) : std::pow(fm, hn));
                }
            }
            hvel = hvelHit_;
            if (grat_ <= 1) high = true;
            else
            {
                const double rp = el_ / glen_ * grat_;
                high = (rp - std::floor(rp) < gduty_);
            }
        }
        // a new hit or a new ratchet hit while the note still sounds: off, then on
        if (high && (!lastHigh_ || start || (newHit && lastHigh_))) { out.off = lastHigh_; out.on = true; out.vel = hvel; }
        else if (!high && lastHigh_) out.off = true;
        lastHigh_ = high;
        out.step = cur_;
        return out;
    }

    double posSteps = 0;     // the last position in steps (with swing), for SND NEXT
    int curStep() const { return cur_; }

private:
    static int wrapi(int x, int m) { return ((x % m) + m) % m; }
    static bool ptnOk(const EngCell& c, int cnt) { return c.pb <= 1 || wrapi(cnt, c.pb) == c.pa - 1; }
    float rnd()   // 0..1
    {
        uint32_t x = rngState_;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        rngState_ = x;
        return (x >> 8) * (1.0f / 16777216.0f);
    }

    double prevsub_ = 1.0;   // 1.0: the first tick starts a step
    int rst_ = 1, cur_ = -1, slot_ = kSlots, reps_ = -1, jt_ = -1, rptn_ = 0, rpting_ = 0, revf_ = 0;
    bool rev_ = false;
    int act_ = 0, grat_ = 1, gfade_ = 0, hitprev_ = -1;
    double el_ = 0, glen_ = 1;
    float gvel_ = 0, gduty_ = 0.5f, hvelHit_ = 0;
    bool lastHigh_ = false;
    int qon_ = 0, qnew_ = 0, qrat_ = 1, qfade_ = 0;
    float qd_ = 0, qv_ = 0, qlen_ = 1, qduty_ = 0.5f;
    Fire qfire_;
    uint32_t rngState_ = 0x9E3779B9u;
};

} // namespace perc

#endif
