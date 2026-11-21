// seq_engine.h - the scalaSeq engine: a port of the gen~ engine of dingus.pitchGateSeq (uPG).
//
// SPDX-License-Identifier: MIT
//
// One call of tick() per audio sample, as in gen~. It reads the note tables (seq_model.h) and
// returns the gate level, the hit velocity, the pitch (tuned MIDI float, with glides) and the
// current step. scalaSeq turns these into MIDI (note on at a gate rise, note off at a fall,
// pitch bend for glides).
//
// Not ported (later, with the modulation design): the length / direction / rate / range
// modulation inputs (fixed at 0), the chance scale (fixed at 1), the trigger and envelope
// outputs. The PTN bar count comes from the MPC transport.
//
// CPU: per sample only comparisons and a few multiplies. Chance rolls and table reads happen
// once per note; FADE uses std::pow once per ratchet hit, not per sample.

#ifndef SCALASEQ_ENGINE_H
#define SCALASEQ_ENGINE_H

#include <cmath>
#include <cstdint>

#include "seq_model.h"

namespace seq {

struct EngineOut
{
    float gate = 0;      // > 0 while a hit sounds (level = velocity)
    float vel = 0;       // velocity of the current hit
    float pitch = 0;     // tuned MIDI float
    int step = -1;       // current step
    bool tieStart = false;   // a note started without a gap (TIE): legato
};

class Engine
{
public:
    // Parameters (set by the plugin; read every sample)
    int dir = 0;            // 0 forward, 1 reverse, 2 random
    bool ptnBars = false;   // PTN counts bars (true) or loop repeats (false)
    int barCount = 0;       // bar counter from the transport

    void reset()
    {
        const int d = dir; const bool p = ptnBars; const uint32_t r = rngState_;
        *this = Engine();
        dir = d; ptnBars = p; rngState_ = r;
    }
    // Start again (transport start or position jump): the next tick begins a step at once.
    void restart() { rst_ = 1; prevsub_ = 1.0; }

    // phase: position in the loop 0..1 (one cycle = the whole sequence). running: transport plays.
    // T: tables of the playing sequence. sr: sample rate.
    EngineOut tick(double phase, bool running, const Tables& T, double sr)
    {
        EngineOut out;
        const bool wrapped = (phase - pph_) < -0.5;
        pph_ = phase;
        wrappedNow = wrapped;

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
                // MPC clock: forward playback starts at the step the song position points to
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
            // SKIP / UNSKIP of the notes starting in the step (a repeated step is not skipped)
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
                        const EngCell& c = T.cells[sl.ni][k];
                        if (c.op == 3 || c.op == 4)
                            if (ptnOk(c, cnt) && rnd() < std::min(c.prob, 1.0f)) s = (c.op == 3) ? 1 : 0;
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

        // notes of the current step, in order of their offset into the step
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
            float cv = fs->vel, clen = flen, dly = 0, cgl = fs->glide, glFrac = -1;   // glFrac >= 0: GLIDE op (share of the hit)
            int crat = std::max(fs->rat, 1), pidx = fs->pidx;
            bool muted = false, tie = false, hold = false;
            const float room = fs->room;
            for (int k = 0; k < kLanes; ++k)
            {
                const EngCell& c = T.cells[fs->ni][k];
                if (c.op <= 0) continue;
                if (!(ptnOk(c, cnt) && rnd() < std::min(c.prob, 1.0f))) continue;
                const float amt = c.amt;
                int sdir = 0, scnt = 0;
                switch (c.op)
                {
                    case 1: muted = true; break;
                    case 2: muted = false; break;
                    case 5: jt_ = lo + wrapi(cur_ - (int)amt - lo, span); break;
                    case 6: jt_ = lo + std::min((int)std::floor(rnd() * span), hi - lo); break;
                    case 7: jt_ = lo + wrapi(cur_ + (int)amt - lo, span); break;
                    case 8: jt_ = (int)amt - 1; break;
                    case 9: cv = std::max(0.0f, cv - amt / 100); break;
                    case 10: cv = std::min(1.0f, cv + amt / 100); break;
                    case 11: cv = 1; break;
                    case 12: clen = std::max(clen - amt, 0.01f); break;
                    case 13: clen = std::min(clen + amt, std::max(room, clen)); break;
                    case 14: clen = std::max(room, clen); tie = true; break;
                    case 15: crat = (int)amt; break;
                    case 16: sdir = 1; scnt = (int)amt; break;
                    case 17: sdir = -1; scnt = (int)amt; break;
                    case 18: sdir = 1; scnt = 1 + std::min((int)std::floor(rnd() * amt), (int)amt - 1); break;
                    case 19: sdir = -1; scnt = 1 + std::min((int)std::floor(rnd() * amt), (int)amt - 1); break;
                    case 20: { const int rr = std::min((int)std::floor(rnd() * (2 * amt + 1)), (int)(2 * amt)) - (int)amt;
                               sdir = rr < 0 ? -1 : 1; scnt = std::abs(rr); break; }
                    case 21: dly = amt; break;
                    case 22: if (rpting_ == 0) rptn_ = std::max(rptn_, (int)amt); break;
                    case 23: revf_ = (d < 2) ? 1 : 0; break;
                    case 24: hold = true; break;
                    case 25: case 26: pidx = (int)amt; hold = false; break;
                    case 27: cv = std::max(0.0f, std::min(1.0f, cv + (rnd() * 2 - 1) * amt / 100)); break;
                    case 28: glFrac = amt / 100; break;          // GLIDE: glide over this share of the hit
                    case 29: glFrac = -1; cgl = 0; break;        // NO GLIDE
                    case 30: dly = amt / 100; break;             // DELAY %: a share of a step
                    default: break;
                }
                if (sdir != 0 && scnt > 0 && pidx >= 0 && pidx < kPitchN)
                {
                    hold = false;
                    if (T.scaleCount > 0)
                    {
                        if (sdir > 0)
                        {
                            const int rr = std::min(T.rankLE[pidx] + scnt - 1, T.scaleCount - 1);
                            pidx = std::max(pidx, T.scaleList[rr]);
                        }
                        else
                        {
                            const int rr = std::max(T.rankLT[pidx] - scnt, 0);
                            pidx = std::min(pidx, T.scaleList[rr]);
                        }
                    }
                }
            }
            // DELAY: the note starts later and keeps its length
            clen = std::min(clen, fcut - dly);
            if (clen <= 0) muted = true;
            if (!muted)
            {
                qon_ = 1; qnew_ = 1; qd_ = dly; qv_ = cv; qlen_ = clen; qrat_ = crat; qpi_ = pidx;
                qduty_ = fs->duty; qfade_ = fs->fade; qglide_ = glFrac >= 0 ? glFrac * clen : cgl; qmidi_ = fs->midi;
                qtie_ = tie; qhold_ = hold;
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
            if (lastout_ > 0 && !tienext_) gapc_ = (int)std::floor(sr * 0.003);
            out.tieStart = tienext_ && lastout_ > 0;
            tienext_ = qtie_;
            act_ = 1; el_ = 0; glen_ = qlen_; grat_ = std::max(qrat_, 1); gvel_ = qv_;
            gduty_ = qduty_; gfade_ = qfade_; hitprev_ = -1;
            if (!qhold_)
            {
                const float gl = qglide_;
                if (havep_ && gl > 0) { pfrom_ = ppitch_; gdur_ = gl; gpos_ = 0; }
                else gdur_ = 0;
                pto_ = (qpi_ >= 0 && qpi_ < kPitchN) ? T.pitchMidi[qpi_] : qmidi_;
                havep_ = 1;
            }
        }

        if (gdur_ > 0)
        {
            gpos_ += adv;
            ppitch_ = pfrom_ + (pto_ - pfrom_) * (float)std::min(gpos_ / gdur_, 1.0);
            if (gpos_ >= gdur_) gdur_ = 0;
        }
        else ppitch_ = pto_;

        float g = 0, hvel = 0;
        if (act_)
        {
            // FADE: velocity per ratchet hit, exponential (computed once per hit)
            const int hit = std::min((int)std::floor(el_ / glen_ * grat_), grat_ - 1);
            if (hit != hitprev_)
            {
                hitprev_ = hit;
                hvelHit_ = gvel_;
                if (grat_ > 1 && gfade_ != 0)
                {
                    const float hn = (float)hit / (grat_ - 1);
                    const float fm = std::pow(0.01f, std::abs((float)gfade_) / 50.0f);
                    hvelHit_ = gvel_ * (gfade_ < 0 ? std::pow(fm, 1 - hn) : std::pow(fm, hn));
                }
            }
            hvel = hvelHit_;
            if (grat_ <= 1) g = hvel;
            else
            {
                const double rp = el_ / glen_ * grat_;
                if (rp - std::floor(rp) < gduty_) g = hvel;
            }
        }
        if (gapc_ > 0) { gapc_--; g = 0; }
        lastout_ = g;

        out.gate = g; out.vel = hvel; out.pitch = ppitch_; out.step = cur_;
        return out;
    }

    bool wrappedNow = false;   // the phase wrapped at the last tick (sequence switch point)

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

    double pph_ = 0, prevsub_ = 1.0;   // 1.0: the first tick starts a step
    int rst_ = 1, cur_ = -1, slot_ = kSlots, reps_ = -1, jt_ = -1, rptn_ = 0, rpting_ = 0, revf_ = 0;
    bool rev_ = false;
    int act_ = 0, grat_ = 1, gfade_ = 0, hitprev_ = -1, gapc_ = 0, havep_ = 0;
    double el_ = 0, glen_ = 1, gpos_ = 0, gdur_ = 0;
    float gvel_ = 0, gduty_ = 0.5f, hvelHit_ = 0, lastout_ = 0;
    float ppitch_ = 60, pfrom_ = 60, pto_ = 60;
    bool tienext_ = false;
    int qon_ = 0, qnew_ = 0, qrat_ = 1, qpi_ = 0, qfade_ = 0;
    float qd_ = 0, qv_ = 0, qlen_ = 1, qduty_ = 0.5f, qglide_ = 0, qmidi_ = 60;
    bool qtie_ = false, qhold_ = false;
    uint32_t rngState_ = 0x9E3779B9u;
};

} // namespace seq

#endif
