// Tests for percSeq: step entry, playback per row (own rates and lengths), the look-ahead queue,
// FLAM, SMP REV, CC, sound ops with the pool, sequence switching, Reset, touch and state.
#define PERCSEQ_TEST 1
#include "../src/PercSeq.cpp"

#include <cstdio>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void waitWorker() { std::this_thread::sleep_for(std::chrono::milliseconds(40)); }

constexpr int kFr = 64;
constexpr double kQps = 120.0 / 60.0 / 48000.0;   // quarter notes per sample at 120 bpm, 48 kHz

// one block; transport playing at ppq (or stopped when ppq < 0)
static void block(PercSeq& p, std::vector<mpc::MidiEvent> ev, double ppq = -1)
{
    static float l[512], r[512];
    float* outs[2] = {l, r};
    mpc::ProcessContext ctx;
    ctx.outputs = outs; ctx.numOutputs = 2; ctx.numFrames = kFr; ctx.sampleRate = 48000;
    ctx.midi = ev.data(); ctx.numMidiEvents = (int)ev.size();
    if (ppq >= 0)
    {
        ctx.transport.valid = true; ctx.transport.playing = true; ctx.transport.ppqValid = true;
        ctx.transport.tempoValid = true; ctx.transport.bpm = 120; ctx.transport.ppqPos = ppq;
    }
    p.process(ctx);
}
// play from ppq a to b (exclusive), block by block
static double play(PercSeq& p, double a, double b)
{
    double q = a;
    while (q < b - 1e-9) { block(p, {}, q); q += kFr * kQps; }
    return q;
}
static void stop(PercSeq& p) { block(p, {}); block(p, {}); }

static std::vector<PercSeq::TestEmit> ons(PercSeq& p, int type = 0)
{
    std::vector<PercSeq::TestEmit> v;
    for (auto& e : p.testEmits_) if (e.type == type) v.push_back(e);
    return v;
}
static std::vector<std::array<uint8_t, 3>> takeLog(PercSeq& p)
{
    waitWorker();
    std::lock_guard<std::mutex> l(p.testMutex_);
    auto v = p.testLog_;
    p.testLog_.clear();
    return v;
}
static bool near(double a, double b, double tol = kQps * 1.01) { return std::fabs(a - b) <= tol; }   // one sample

// clear every row of the given sequence and set row r to a plain pattern
static void clearSeq(PercSeq& p, int s = 0)
{
    Sequence& q = p.testModel().seqs[s];
    for (int r = 0; r < kRows; ++r) { Row fresh; fresh.note = 36 + r; fresh.revCC = 102 + r; fresh.ccNum = 14 + r; q.rows[r] = fresh; }
}
static void addHit(PercSeq& p, int r, float st, float len = 0.5f, int s = 0)
{
    Hit h; h.st = st; h.len = len;
    p.testModel().seqs[s].rows[r].insert(h);
}
static void rebuild(PercSeq& p) { p.internalResume(); }   // reset(): rebuilds the tables

int main()
{
    PercSeq p;
    p.internalSetSampleRate(48000); p.internalSetBlockSize(kFr); p.internalResume();
    waitWorker();
    block(p, {});
    CHECK(kNumParams < 2200, "parameter count %d", (int)kNumParams);
    // 1.1 defaults: Snap 1, Entry Len 1, Steps/Page 8
    CHECK(p.testChoice(kSnap) == 0 && p.testChoice(kEntryLen) == 4 && p.testChoice(kZoom) == 0, "defaults snap %d entry %d zoom %d",
          p.testChoice(kSnap), p.testChoice(kEntryLen), p.testChoice(kZoom));
    // the tests below were written for Snap 1/4 and 16 steps on the page
    p.set(kSnap, 3); p.set(kZoom, 1); block(p, {});
    std::printf("parameters: %d\n", (int)kNumParams);

    // ---- step entry: Pad 1 Note 92; pads 1, 3, 3 -> rows 0, 2, 2 at the cursor
    p.set(kPadNote, 92);
    p.set(kStepEntry, 1);
    block(p, {});
    block(p, {mpc::MidiEvent::noteOn(0, 92, 100), mpc::MidiEvent::noteOff(0, 92)});
    block(p, {mpc::MidiEvent::noteOn(0, 94, 100), mpc::MidiEvent::noteOff(0, 94)});
    block(p, {mpc::MidiEvent::noteOn(0, 94, 100), mpc::MidiEvent::noteOff(0, 94)});
    {
        Sequence& s = p.testModel().seqs[0];
        CHECK(s.rows[0].count == 1 && s.rows[0].hits[0].st == 0, "row 1: hit at 0 (%d)", s.rows[0].count);
        CHECK(s.rows[2].count == 2 && s.rows[2].hits[0].st == 1 && s.rows[2].hits[1].st == 2, "row 3: hits at 1, 2 (%d)", s.rows[2].count);
        CHECK(p.testSelRow() == 2 && p.testSel() == 1, "the last entered hit is selected (%d %d)", p.testSelRow(), p.testSel());
        CHECK(near(p.testCursor(), 3), "cursor 3: %f", p.testCursor());
    }
    p.set(kStepEntry, 0);
    block(p, {});
    // Pad 1 Note skips 45-91
    p.set(kPadNote, 50); block(p, {});
    CHECK(p.getInt(kPadNote) == 44, "Pad 1 Note 92 -> 50 jumps down to 44: %d", p.getInt(kPadNote));

    // ---- playback: row 1 (note 36) 1/16, hits at 0 and 1; row 2 (note 37) 1/8T, hits at 0, 1, 2
    clearSeq(p);
    addHit(p, 0, 0); addHit(p, 0, 1);
    p.testModel().seqs[0].rows[1].rate = 6;   // 1/8T = 1/3 quarter
    p.testModel().seqs[0].rows[1].steps = 3; p.testModel().seqs[0].rows[1].rEnd = 2;
    addHit(p, 1, 0, 0.5f); addHit(p, 1, 1, 0.5f); addHit(p, 1, 2, 0.5f);
    p.testModel().seqs[0].rows[0].steps = 4; p.testModel().seqs[0].rows[0].rEnd = 3;
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 2.0);
    stop(p);
    {
        auto v = ons(p);
        std::vector<double> t36, t37;
        for (auto& e : v) { if (e.a == 36) t36.push_back(e.t); if (e.a == 37) t37.push_back(e.t); }
        // row 1: loop of 4 sixteenths = 1 quarter: hits at 0, 0.25, 1, 1.25
        CHECK(t36.size() == 4, "row 1 hits in 2 quarters: %zu", t36.size());
        if (t36.size() == 4) CHECK(near(t36[0], 0) && near(t36[1], 0.25) && near(t36[2], 1) && near(t36[3], 1.25),
                                   "row 1 times %f %f %f %f", t36[0], t36[1], t36[2], t36[3]);
        // row 2: every 1/3 quarter: 0, 1/3, 2/3, 1, 4/3, 5/3
        CHECK(t37.size() == 6, "row 2 hits: %zu", t37.size());
        for (size_t i = 0; i < t37.size(); ++i) CHECK(near(t37[i], i / 3.0, 1e-4), "row 2 hit %zu at %f", i, t37[i]);
        // nothing goes out before its time, and at most one sample late
        for (auto& e : p.testEmits_) CHECK(e.at >= e.t - 1e-9 && e.at - e.t < kQps + 1e-9, "emitted at %f for %f", e.at, e.t);
    }

    // ---- velocity: row Velocity 50 % halves the hit velocity
    clearSeq(p);
    addHit(p, 0, 0);
    p.testModel().seqs[0].rows[0].vel = 50;
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 0.2); stop(p);
    {
        auto v = ons(p);
        CHECK(!v.empty() && v[0].b == 64, "velocity 127 * 50 %% = 64: %d", v.empty() ? -1 : v[0].b);
    }

    // ---- row Velocity 0 %: the row sends nothing
    clearSeq(p);
    addHit(p, 0, 0);
    p.testModel().seqs[0].rows[0].vel = 0;
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 0.2); stop(p);
    CHECK(ons(p).empty(), "a 0 %% row sends no note: %zu", ons(p).size());

    // ---- two rows on the same note at once: note-off before the second note-on, one off at the end
    clearSeq(p);
    addHit(p, 0, 0, 1.0f); addHit(p, 1, 0, 0.5f);
    p.testModel().seqs[0].rows[1].note = 36;
    rebuild(p);
    takeLog(p);
    play(p, 0, 0.4); stop(p);
    {
        auto log = takeLog(p);
        std::vector<int> seq;   // 1 = on, 0 = off, for note 36
        for (auto& m : log) if (m[1] == 36 && ((m[0] & 0xF0) == 0x90 || (m[0] & 0xF0) == 0x80)) seq.push_back((m[0] & 0xF0) == 0x90 ? 1 : 0);
        CHECK(seq.size() >= 4 && seq[0] == 1 && seq[1] == 0 && seq[2] == 1 && seq[3] == 0, "on, off, on, off (%zu)", seq.size());
    }

    // ---- FLAM 8/96 step before a hit at step 2 (1/16): grace at 0.5 - 8/96 * 0.25, half velocity
    clearSeq(p);
    addHit(p, 0, 2);
    {
        Hit& h = p.testModel().seqs[0].rows[0].hits[0];
        h.ch[0].op = O_FLAM; h.ch[0].prob = 1; h.ch[0].amt = 8;
    }
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 0.9); stop(p);
    {
        auto v = ons(p);
        CHECK(v.size() == 2, "grace + hit: %zu", v.size());
        if (v.size() == 2)
        {
            CHECK(near(v[0].t, 0.5 - 8.0 / 96 * 0.25) && near(v[1].t, 0.5), "flam times %f %f", v[0].t, v[1].t);
            CHECK(v[0].b == 64 && v[1].b == 127, "flam velocities %d %d", v[0].b, v[1].b);
            CHECK(v[0].a == 36 && v[1].a == 36, "same sound");
        }
    }

    // ---- SMP REV: CC 102 127 before the REV hit, CC 102 0 before the next normal hit of the row
    clearSeq(p);
    addHit(p, 0, 0); addHit(p, 0, 1); addHit(p, 0, 2);
    {
        Hit& h = p.testModel().seqs[0].rows[0].hits[1];
        h.ch[0].op = O_SMPREV; h.ch[0].prob = 1;
    }
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 0.7); stop(p);
    {
        auto cc = ons(p, 2);
        CHECK(cc.size() == 2, "two CCs: %zu", cc.size());
        if (cc.size() == 2)
        {
            CHECK(cc[0].a == 102 && cc[0].b == 127 && near(cc[0].t, 0.25), "reverse on before hit 2: %d %d %f", cc[0].a, cc[0].b, cc[0].t);
            CHECK(cc[1].a == 102 && cc[1].b == 0 && near(cc[1].t, 0.5), "reverse off before hit 3: %d %d %f", cc[1].a, cc[1].b, cc[1].t);
        }
        // order: the CC comes before the note at the same time
        int iCC = -1, iOn = -1;
        for (size_t i = 0; i < p.testEmits_.size(); ++i)
        {
            const auto& e = p.testEmits_[i];
            if (e.type == 2 && iCC < 0) iCC = (int)i;
            if (e.type == 0 && near(e.t, 0.25) && iOn < 0) iOn = (int)i;
        }
        CHECK(iCC >= 0 && iOn > iCC, "CC before the note (%d %d)", iCC, iOn);
    }

    // ---- stop with a reversed pad: Rev CC 0 goes out
    clearSeq(p);
    addHit(p, 0, 0);
    { Hit& h = p.testModel().seqs[0].rows[0].hits[0]; h.ch[0].op = O_SMPREV; h.ch[0].prob = 1; }
    rebuild(p);
    takeLog(p);
    play(p, 0, 0.2); stop(p);
    {
        auto log = takeLog(p);
        bool on = false, off = false;
        for (auto& m : log) if ((m[0] & 0xF0) == 0xB0 && m[1] == 102) { if (m[2] == 127) on = true; else if (on && m[2] == 0) off = true; }
        CHECK(on && off, "reverse on, then off at stop (%d %d)", on, off);
    }

    // ---- CC op: CC 14 = 30 before hit 1, CC 14 = rest 64 before hit 2
    clearSeq(p);
    addHit(p, 0, 0); addHit(p, 0, 1);
    {
        Hit& h = p.testModel().seqs[0].rows[0].hits[0];
        h.ch[2].op = O_CC; h.ch[2].prob = 1; h.ch[2].amt = 30;
    }
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 0.45); stop(p);
    {
        auto cc = ons(p, 2);
        CHECK(cc.size() == 2 && cc[0].a == 14 && cc[0].b == 30 && cc[1].a == 14 && cc[1].b == 64, "CC value then rest (%zu)", cc.size());
    }

    // ---- SND+ 1 with the pool {1, 3, 16}: row 3 -> row 16, row 16 wraps to row 1
    clearSeq(p);
    addHit(p, 2, 0); addHit(p, 15, 1);
    for (int r = 0; r < kRows; ++r) p.set(kPool0 + r, (r == 0 || r == 2 || r == 15) ? 1.0f : 0.0f);
    for (int r : {2, 15})
    {
        Hit& h = p.testModel().seqs[0].rows[r].hits[0];
        h.ch[0].op = O_SNDP; h.ch[0].prob = 1; h.ch[0].amt = 1;
    }
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 0.45); stop(p);
    {
        auto v = ons(p);
        CHECK(v.size() == 2 && v[0].a == 36 + 15 && v[1].a == 36 + 0, "SND+ notes %d %d", v.size() > 0 ? v[0].a : -1, v.size() > 1 ? v[1].a : -1);
    }
    for (int r = 0; r < kRows; ++r) p.set(kPool0 + r, 1.0f);
    block(p, {});

    // ---- SND PREV: the row of the last hit (row 5 at step 0, row 1 at step 1 plays row 5's note)
    clearSeq(p);
    addHit(p, 4, 0); addHit(p, 0, 1);
    { Hit& h = p.testModel().seqs[0].rows[0].hits[0]; h.ch[0].op = O_SNDPREV; h.ch[0].prob = 1; }
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 0.45); stop(p);
    {
        auto v = ons(p);
        CHECK(v.size() == 2 && v[1].a == 40, "SND PREV plays row 5: %d", v.size() > 1 ? v[1].a : -1);
    }

    // ---- sequence notes: playing, note 61 at ppq 0.5 starts sequence 2 there (row 1 hit at step 1)
    clearSeq(p, 0); clearSeq(p, 1);
    addHit(p, 0, 0, 0.5f, 0);
    addHit(p, 3, 1, 0.5f, 1);
    p.testModel().seqs[1].rows[3].rStart = 1;
    rebuild(p);
    p.testEmits_.clear();
    double q = play(p, 0, 0.5);
    block(p, {mpc::MidiEvent::noteOn(0, 61, 100, 0)}, q);
    q += kFr * kQps;
    play(p, q, 0.9);
    stop(p);
    CHECK(p.testEngSeq() == 1, "sequence 2 plays: %d", p.testEngSeq());
    {
        auto v = ons(p);
        bool found = false;
        for (auto& e : v) if (e.a == 39 && std::fabs(e.t - q + kFr * kQps) < 1e-6) found = true;
        CHECK(found, "sequence 2 row 4 starts at its Range Start at the note (%zu notes)", v.size());
    }
    // stopped: note 60 selects sequence 1
    block(p, {mpc::MidiEvent::noteOn(0, 60, 100, 0)});
    block(p, {});
    CHECK(p.testEdSeq() == 0 && p.testEngSeq() == 0, "note 60 selects sequence 1");

    // ---- Sequence knob while playing: switch at the next bar line (ppq 4)
    clearSeq(p, 0); clearSeq(p, 1);
    addHit(p, 0, 0, 0.5f, 0);
    addHit(p, 1, 0, 0.5f, 1);
    rebuild(p);
    p.testEmits_.clear();
    q = play(p, 0, 1.0);
    p.set(kSequence, 2);
    q = play(p, q, 4.5);
    stop(p);
    {
        double first37 = -1;
        for (auto& e : ons(p)) if (e.a == 37) { first37 = e.t; break; }
        CHECK(near(first37, 4.0), "sequence 2 starts at the bar line: %f", first37);
    }
    p.set(kSequence, 1);
    block(p, {});

    // ---- Reset 1 bar: a row of 3 sixteenths restarts at every bar line
    clearSeq(p);
    p.testModel().seqs[0].rows[0].steps = 3; p.testModel().seqs[0].rows[0].rEnd = 2;
    addHit(p, 0, 0, 0.5f);
    p.set(kReset, 1);
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 4.3);
    stop(p);
    {
        bool atBar = false;
        for (auto& e : ons(p)) if (near(e.t, 4.0)) atBar = true;
        // without Reset the hits would fall at 3.75 and 4.5; with Reset there is one at 4.0
        CHECK(atBar, "Reset: a hit at the bar line");
        int n = 0;
        for (auto& e : ons(p)) if (e.t > 3.7 && e.t < 4.0 - 1e-6) ++n;
        CHECK(n == 1, "the hit at 3.75 still plays before the bar: %d", n);
    }
    p.set(kReset, 0);
    block(p, {});

    // ---- a jump: no hit is played twice, the hit at the new position plays
    clearSeq(p);
    addHit(p, 0, 0, 0.5f); addHit(p, 0, 8, 0.5f);   // step 8 = ppq 2
    rebuild(p);
    p.testEmits_.clear();
    play(p, 0, 0.3);
    play(p, 2.0, 2.3);   // locate to ppq 2
    stop(p);
    {
        int at2 = 0;
        for (auto& e : ons(p)) if (near(e.t, 2.0)) ++at2;
        CHECK(at2 == 1, "after the jump the hit at 2.0 plays once: %d", at2);
    }

    // ---- touch: tap the roll at row 5 (from the bottom), step 3 -> a hit of Entry Len in row 5
    clearSeq(p);
    rebuild(p);
    block(p, {});
    {
        auto pv = [](float px, float pad) { return std::max(0.0f, std::min(1.0f, (px - 1) / (pad - 2))); };
        const float x = pv(3.0f / 16 * kGW + 2, kGW), y = pv((4 + 0.5f) * kRowH, kGH);
        p.set(kRollTX, x); p.set(kRollTX + 1, y); block(p, {});
        p.set(kRollTX, 0); block(p, {}); p.set(kRollTX + 1, 0); block(p, {});
        const Row& w = p.testModel().seqs[0].rows[4];
        CHECK(w.count == 1 && near(w.hits[0].st, 3) && near(w.hits[0].len, 1), "tap: hit in row 5 at 3, length 1 (%d)", w.count);
        CHECK(p.testSelRow() == 4, "row 5 selected: %d", p.testSelRow());
        // 1.1: a row label touch switches row 9 off, a second touch on again
        p.set(kLabTX, 0.5f); p.set(kLabTX + 1, pv((8 + 0.5f) * kRowH, kGH)); block(p, {});
        p.set(kLabTX, 0); block(p, {}); p.set(kLabTX + 1, 0); block(p, {});
        CHECK(!p.testModel().seqs[0].rows[8].on && !p.getBool(kOn0 + 8), "label switches row 9 off");
        p.set(kLabTX, 0.5f); p.set(kLabTX + 1, pv((8 + 0.5f) * kRowH, kGH)); block(p, {});
        p.set(kLabTX, 0); block(p, {}); p.set(kLabTX + 1, 0); block(p, {});
        CHECK(p.testModel().seqs[0].rows[8].on && p.getBool(kOn0 + 8), "label switches row 9 on");
    }

    // ---- Row knob on HIT: moves the selected hit to another row
    clearSeq(p);
    addHit(p, 0, 1);
    rebuild(p);
    p.set(kSelRow, 1); block(p, {});
    p.set(kHitRow, 7); block(p, {});
    CHECK(p.testModel().seqs[0].rows[0].count == 0 && p.testModel().seqs[0].rows[6].count == 1, "hit moved to row 7");
    CHECK(p.testSelRow() == 6, "row 7 selected");

    // ---- ROW page edits the selected row
    p.set(kRNote, 50); p.set(kRRate, 9); block(p, {});
    CHECK(p.testModel().seqs[0].rows[6].note == 50 && p.testModel().seqs[0].rows[6].rate == 9, "row 7 note 50, rate 1/8");

    // ---- display: the playhead of row 1 is drawn while playing
    clearSeq(p);
    addHit(p, 0, 0);
    rebuild(p);
    play(p, 0, 0.3);
    CHECK(p.testDispStep(0) == 1, "row 1 at step 2 at ppq 0.3: %d", p.testDispStep(0));
    CHECK(p.testComputed(kCellX0 + 1) > 0.0f, "row 1 step square shown (16-step size)");
    CHECK(p.testComputed(kCellX0 + 0) == 0.0f && p.testComputed(kCellX0 + 2) == 0.0f, "the other sizes are parked");
    {   // 2026-11-22: the LANE page shows the VEL lane's playhead (the selected row's step)
        const int oldRow = p.testSelRow();
        p.set(kSelRow, 1); p.set(kLaneSel, 0);
        play(p, 0.3, 0.35);
        CHECK(p.testComputed(kLnPlayX0 + 1) > 0.0f, "VEL lane playhead shown");
        CHECK(std::fabs(p.testComputed(kLnPlayX0 + 1) - p.testComputed(kCellX0 + 1)) < 1e-6f, "at the row's step");
        p.set(kSelRow, oldRow + 1); block(p, {}, 0.35);
    }
    stop(p);

    // ---- 1.1: row off: silent, no step square; on again: starts at its Range Start at the next bar
    clearSeq(p);
    addHit(p, 0, 0); addHit(p, 1, 0);
    p.testModel().seqs[0].rows[1].steps = 3; p.testModel().seqs[0].rows[1].rEnd = 2;   // 3-step row: off the bar grid
    p.testModel().seqs[0].rows[1].rStart = 1;
    p.testModel().seqs[0].rows[1].insert([] { Hit h; h.st = 1; h.len = 0.5f; return h; }());
    rebuild(p);
    p.set(kOn0 + 1, 0); block(p, {});
    CHECK(!p.testModel().seqs[0].rows[1].on, "row 2 off");
    p.testEmits_.clear();
    q = play(p, 0, 1.3);
    {
        int n37 = 0;
        for (auto& e : ons(p)) if (e.a == 37) ++n37;
        CHECK(n37 == 0, "an off row sends nothing: %d", n37);
        CHECK(p.testComputed(kCellX0 + 1 * kZoomN + 1) == 0.0f, "an off row shows no step square");
    }
    p.set(kOn0 + 1, 1);
    q = play(p, q, 4.4);
    stop(p);
    {
        double first = -1;
        for (auto& e : ons(p)) if (e.a == 37) { first = e.t; break; }
        CHECK(near(first, 4.0), "row 2 starts at the next bar at its Range Start (hit at step 1): %f", first);
    }
    // a sound op that lands on an off row: silent
    clearSeq(p);
    addHit(p, 0, 0);
    { Hit& h = p.testModel().seqs[0].rows[0].hits[0]; h.ch[0].op = O_SNDP; h.ch[0].prob = 1; h.ch[0].amt = 1; }
    rebuild(p);
    p.set(kOn0 + 1, 0); block(p, {});
    p.testEmits_.clear();
    play(p, 0, 0.2); stop(p);
    CHECK(ons(p).empty(), "SND+ onto an off row: silent (%zu)", ons(p).size());
    p.set(kOn0 + 1, 1); block(p, {});
    // per sequence: sequence 2 keeps its own on/off
    p.set(kOn0 + 3, 0); block(p, {});
    p.set(kSequence, 2); block(p, {});
    CHECK(p.getBool(kOn0 + 3), "sequence 2 row 4 is on");
    p.set(kSequence, 1); block(p, {});
    CHECK(!p.getBool(kOn0 + 3), "sequence 1 row 4 is still off");
    p.set(kOn0 + 3, 1); block(p, {});
    // Steps/Page: 8, 16, 32, 64
    p.set(kZoom, 3); block(p, {});
    p.set(kZoom, 1); block(p, {});

    // ---- 1.1: Nudge -96..96 from the Move position, whatever Snap is
    clearSeq(p);
    addHit(p, 0, 4, 0.5f);
    rebuild(p);
    p.set(kSelRow, 1); block(p, {});
    p.set(kHitSel, 1); block(p, {});
    p.set(kNudge, 60); block(p, {});
    CHECK(near(p.testModel().seqs[0].rows[0].hits[0].st, 4 + 60 / 96.0, 1e-5), "nudge +60: %f", p.testModel().seqs[0].rows[0].hits[0].st);
    p.set(kNudge, -96); block(p, {});
    CHECK(near(p.testModel().seqs[0].rows[0].hits[0].st, 3, 1e-5), "nudge -96: %f", p.testModel().seqs[0].rows[0].hits[0].st);
    p.set(kMove, 8); block(p, {});   // Snap 1/4: Move 8 = step 2; the Nudge stays
    CHECK(near(p.testModel().seqs[0].rows[0].hits[0].st, 1, 1e-5) && p.testModel().seqs[0].rows[0].hits[0].nd == -96, "move keeps the nudge: %f", p.testModel().seqs[0].rows[0].hits[0].st);
    CHECK(p.getInt(kNudge) == -96 && p.getInt(kMove) == 8, "knobs show nudge %d move %d", p.getInt(kNudge), p.getInt(kMove));

    // ---- 1.1: pages: 8 steps, < > buttons, a ruler swipe, the limit at the longest row, FOLLOW, lists close on a pick
    p.set(kZoom, 0); block(p, {});
    clearSeq(p);
    p.testModel().seqs[0].rows[2].steps = 24; p.testModel().seqs[0].rows[2].rEnd = 23;   // longest row: 3 pages
    addHit(p, 0, 0);
    rebuild(p);
    p.set(kPageNext, 1); block(p, {});
    CHECK(p.testViewStart() == 8 && !p.getBool(kPageNext) && p.getInt(kViewStart) == 2, "> : page 2 (view %d)", p.testViewStart());
    p.set(kPageNext, 1); block(p, {}); p.set(kPageNext, 1); block(p, {});
    CHECK(p.testViewStart() == 16, "stops at the last page of the longest row: %d", p.testViewStart());
    p.set(kPagePrev, 1); block(p, {});
    CHECK(p.testViewStart() == 8, "< : page 2: %d", p.testViewStart());
    {
        auto pv = [](float px, float pad) { return std::max(0.0f, std::min(1.0f, (px - 1) / (pad - 2))); };
        p.set(kRulTX, pv(400, kGW)); p.set(kRulTX + 1, 0.5f); block(p, {});
        for (int k = 1; k <= 10; ++k) { p.set(kRulTX, pv(400 - k * 20.0f, kGW)); block(p, {}); }   // 200 px to the left
        p.set(kRulTX, 0); block(p, {}); p.set(kRulTX + 1, 0); block(p, {});
        CHECK(p.testViewStart() == 16, "a swipe to the left: one page on (%d)", p.testViewStart());
    }
    p.set(kFollow, 1); p.set(kSelRow, 1); block(p, {});
    play(p, 0, 0.3);
    CHECK(p.testViewStart() == 0, "FOLLOW: back to the page of row 1's playhead: %d", p.testViewStart());
    stop(p);
    p.set(kFollow, 0); block(p, {});
    p.set(kListOpen, 2); block(p, {});
    p.set(kLane0, 3); block(p, {});
    CHECK(p.getInt(kListOpen) == 0, "a pick closes the list: %d", p.getInt(kListOpen));
    p.set(kZoom, 1); block(p, {});

    // ---- 1.1: a knob value between two choices is written back on the choice percSeq took
    clearSeq(p);
    addHit(p, 0, 0);
    rebuild(p);
    p.set(kSelRow, 1); block(p, {});
    p.setNormalised(kLane0, 0.0546875f); block(p, {});   // 7/128: between MUTE (1/31) and UNMUTE (2/31)
    CHECK(p.testModel().seqs[0].rows[0].hits[0].ch[0].op == O_MUTE, "Op from the knob value: %d", p.testModel().seqs[0].rows[0].hits[0].ch[0].op);
    CHECK(std::fabs(p.getNormalised(kLane0) - 1.0f / 31) < 1e-6f, "Op knob written back on MUTE: %f", p.getNormalised(kLane0));
    p.setNormalised(kLane0, 0.0f); block(p, {});

    // ---- state: save into a new instance
    clearSeq(p);
    addHit(p, 3, 1.5f, 0.25f);
    p.testModel().seqs[0].rows[3].note = 70;
    p.testModel().seqs[0].rows[3].rate = 2;
    p.testModel().seqs[5].rows[7].on = false;
    p.testSaveAll(); block(p, {});
    {
        std::vector<uint8_t> out;
        p.saveExtraState(out);
        PercSeq q2;
        q2.internalSetSampleRate(48000); q2.internalSetBlockSize(kFr); q2.internalResume();
        q2.loadExtraState(out.data(), out.size());
        q2.internalResume();
        block(q2, {});
        const Row& w = q2.testModel().seqs[0].rows[3];
        CHECK(w.count == 1 && near(w.hits[0].st, 1.5f) && near(w.hits[0].len, 0.25f) && w.note == 70 && w.rate == 2,
              "state: row 4 restored (%d)", w.count);
        CHECK(!q2.testModel().seqs[5].rows[7].on && q2.testModel().seqs[5].rows[6].on, "state: row on/off restored");
    }

    // ---- 1.2: < > step buttons (Hit, Row, Move, Length): stop at the ends, Move and Row skip taken places
    {
        auto press = [&](int b) { p.set(b, 1); block(p, {}); block(p, {}); };
        p.set(kSequence, 1); block(p, {});
        clearSeq(p); p.set(kSnap, 0); block(p, {});
        for (int i = 0; i < 5; ++i) addHit(p, 0, (float)(i * 2), 1.0f);   // steps 0 2 4 6 8
        rebuild(p); block(p, {});
        p.set(kSelRow, 1); p.set(kHitSel, 1); block(p, {}); block(p, {});
        for (int i = 0; i < 7; ++i) press(kHitNext);
        CHECK(p.testSel() == 4 && (int)std::lround(p.get(kHitSel)) == 5, "Hit > stops at the last hit: sel %d value %g", p.testSel(), p.get(kHitSel));
        CHECK(p.get(kHitNext) == 0, "the > button is set back to off");
        for (int i = 0; i < 7; ++i) press(kHitPrev);
        CHECK(p.testSel() == 0, "Hit < stops at the first hit: %d", p.testSel());
        press(kMoveNext);   // hit 1 (step 0) -> step 1 is free
        CHECK(near(p.testModel().seqs[0].rows[0].hits[0].st, 1.0f, 1e-4), "Move > to step 1: %f", p.testModel().seqs[0].rows[0].hits[0].st);
        press(kMoveNext);   // step 2 is taken -> skips to step 3
        CHECK(near(p.testModel().seqs[0].rows[0].hits[1].st, 3.0f, 1e-4) && p.testSel() == 1, "Move > skips the taken step 2 to step 3 (sel %d)", p.testSel());
        press(kLenNext);    // length 1 -> the next hit is at step 4: at most 1
        CHECK(near(p.testModel().seqs[0].rows[0].hits[1].len, 1.0f, 1e-4), "Len > stops at the next hit: %f", p.testModel().seqs[0].rows[0].hits[1].len);
        for (int i = 0; i < 16; ++i) addHit(p, 1, (float)i, 1.0f);   // row 2 full
        rebuild(p); block(p, {});
        press(kRowNext);    // row 2 is full -> row 3
        CHECK(p.testSelRow() == 2 && p.testModel().seqs[0].rows[2].count == 1, "Row > skips the full row 2 to row 3: %d", p.testSelRow());
        press(kRowPrev);    // back over the full row 2 to row 1
        CHECK(p.testSelRow() == 0, "Row < skips the full row 2 to row 1: %d", p.testSelRow());
    }

    // ---- 1.3: ratio rate: row 1 at 1/16 x Ratio 2 (+0.00) plays every 1/32 (0.125 quarter)
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(kFr); u.internalResume();
        waitWorker(); block(u, {});
        clearSeq(u);
        Row& w = u.testModel().seqs[0].rows[0];
        w.steps = 4; w.rEnd = 3; w.mode = 1; w.ratio = 11;   // 2
        for (int i = 0; i < 4; ++i) addHit(u, 0, (float)i, 0.5f);
        rebuild(u);
        u.testEmits_.clear();
        play(u, 0, 1.0);
        stop(u);
        auto v = ons(u);
        CHECK(v.size() == 8, "ratio 2: 8 hits in one quarter: %zu", v.size());
        for (size_t i = 0; i < v.size(); ++i) CHECK(near(v[i].t, i * 0.125, 1e-4), "ratio 2: hit %zu at %f", i, v[i].t);
        // Ratio 1.5 + Fine 0.10 = 1.6: a step is 0.25 / 1.6 quarter
        clearSeq(u);
        Row& w2 = u.testModel().seqs[0].rows[0];
        w2.steps = 4; w2.rEnd = 3; w2.mode = 1; w2.ratio = 9; w2.fine = 10;
        for (int i = 0; i < 4; ++i) addHit(u, 0, (float)i, 0.5f);
        rebuild(u);
        u.testEmits_.clear();
        play(u, 0, 1.0);
        stop(u);
        v = ons(u);
        CHECK(v.size() >= 2 && near(v[1].t, 0.25 / 1.6, 1e-4), "ratio 1.6: 2nd hit at %f", v.size() >= 2 ? v[1].t : -1.0);
    }
    // ---- 1.3: locks: a hit with Lock 1 = 90 on Lock CC 1 = 74 sends CC 74 90 just before its note
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(kFr); u.internalResume();
        waitWorker(); block(u, {});
        clearSeq(u);
        Row& w = u.testModel().seqs[0].rows[0];
        w.steps = 4; w.rEnd = 3; w.lockCC[0] = 74; w.lockCC[1] = 75;
        addHit(u, 0, 0, 0.5f); addHit(u, 0, 2, 0.5f);
        w.hits[0].lock[0] = 90;          // lock 2 of hit 1 is off: nothing on CC 75
        w.hits[1].lock[1] = 10;          // hit 2: only lock 2
        rebuild(u);
        u.testEmits_.clear();
        play(u, 0, 0.75);
        stop(u);
        int n74 = 0, n75 = 0; bool before = true;
        for (size_t i = 0; i < u.testEmits_.size(); ++i)
        {
            const auto& e = u.testEmits_[i];
            if (e.type == 2 && e.a == 74) { ++n74; CHECK(e.b == 90 && near(e.t, 0), "lock 1: CC 74 90 at 0 (%d at %f)", e.b, e.t);
                before = before && i + 1 < u.testEmits_.size() && u.testEmits_[i + 1].type != 1; }
            if (e.type == 2 && e.a == 75) { ++n75; CHECK(e.b == 10 && near(e.t, 0.5), "lock 2: CC 75 10 at 0.5 (%d at %f)", e.b, e.t); }
        }
        CHECK(n74 == 1 && n75 == 1, "one lock each: %d %d", n74, n75);
        CHECK(before, "the lock comes before the note");
        // no Lock CC: nothing is sent
        w.lockCC[0] = 0; rebuild(u); u.testEmits_.clear(); play(u, 0, 0.2); stop(u);
        for (auto& e : u.testEmits_) CHECK(!(e.type == 2 && e.b == 90), "Lock CC off sends nothing");
    }
    // ---- 1.3: CC lane: 4 steps of 1/16 on CC 20, values 10 10 50 50; sent only on change; Rest at stop
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(kFr); u.internalResume();
        waitWorker(); block(u, {});
        clearSeq(u);
        CcLane& L = u.testModel().seqs[0].rows[0].lane[0];
        L.cc = 20; L.steps = 4; L.rest = 33;
        L.val[0] = 10; L.val[1] = 10; L.val[2] = 50; L.val[3] = 50;
        rebuild(u);
        u.testEmits_.clear();
        play(u, 0, 0.99);   // one loop of the lane (4 x 1/16 = 1 quarter)
        std::vector<std::pair<double, int>> cc;
        for (auto& e : u.testEmits_) if (e.type == 2 && e.a == 20) cc.push_back({e.t, e.b});
        CHECK(cc.size() == 2, "lane: 10 at 0, 50 at 0.5 (the same value is not sent again): %zu", cc.size());
        if (cc.size() >= 2) CHECK(cc[0].second == 10 && near(cc[0].first, 0, 1e-6) && cc[1].second == 50 && near(cc[1].first, 0.5, 1e-6),
                                  "lane values/times: %d@%f %d@%f", cc[0].second, cc[0].first, cc[1].second, cc[1].first);
        u.testLog_.clear();
        stop(u);
        auto log = takeLog(u);
        bool rest = false;
        for (auto& m : log) if ((m[0] & 0xF0) == 0xB0 && m[1] == 20 && m[2] == 33) rest = true;
        CHECK(rest, "lane: Rest 33 at stop");
        // a lock on the same CC at the same time wins (comes after the lane value)
        clearSeq(u);
        Row& w = u.testModel().seqs[0].rows[0];
        CcLane& L2 = w.lane[0];
        L2.cc = 20; L2.steps = 4; for (auto& x : L2.val) x = 64;
        w.lockCC[0] = 20; w.steps = 4; w.rEnd = 3;
        addHit(u, 0, 0, 0.5f); w.hits[0].lock[0] = 100;
        rebuild(u);
        u.testEmits_.clear();
        play(u, 0, 0.2);
        stop(u);
        int last = -1;
        for (auto& e : u.testEmits_) if (e.type == 2 && e.a == 20 && near(e.t, 0, 1e-6)) last = e.b;
        CHECK(last == 100, "lock wins over the lane at its hit: last CC 20 at 0 = %d", last);
        // Rnd stays inside the lane's steps; Rev plays 4 3 2 1
        L2.dir = 1; w.count = 0; w.lockCC[0] = 0; L2.val[0] = 1; L2.val[1] = 2; L2.val[2] = 3; L2.val[3] = 4;
        rebuild(u); u.testEmits_.clear(); play(u, 0, 0.99); stop(u);
        std::vector<int> vals;
        for (auto& e : u.testEmits_) if (e.type == 2 && e.a == 20) vals.push_back(e.b);
        CHECK(vals.size() == 4 && vals[0] == 4 && vals[3] == 1, "lane Rev: %zu values, first %d", vals.size(), vals.empty() ? -1 : vals[0]);
    }
    // ---- 1.3: state PSQ3: rate mode, locks, lanes
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(kFr); u.internalResume();
        waitWorker(); block(u, {});
        clearSeq(u);
        Row& w = u.testModel().seqs[2].rows[5];
        w.mode = 1; w.ratio = 8; w.fine = -3; w.lockCC[2] = 74;
        w.lane[1].cc = 21; w.lane[1].steps = 7; w.lane[1].dir = 2; w.lane[1].fine = 4; w.lane[1].val[6] = 99;
        addHit(u, 5, 1, 0.5f, 2); w.hits[0].lock[3] = 55;
        u.testSaveAll(); block(u, {});
        std::vector<uint8_t> out;
        u.saveExtraState(out);
        CHECK(out.size() > 4 && out[3] == '3', "state format PSQ3");
        PercSeq q3;
        q3.internalSetSampleRate(48000); q3.internalSetBlockSize(kFr); q3.internalResume();
        q3.loadExtraState(out.data(), out.size());
        q3.internalResume(); block(q3, {});
        const Row& r = q3.testModel().seqs[2].rows[5];
        CHECK(r.mode == 1 && r.ratio == 8 && r.fine == -3 && r.lockCC[2] == 74, "state: rate mode and lock CC");
        CHECK(r.lane[1].cc == 21 && r.lane[1].steps == 7 && r.lane[1].dir == 2 && r.lane[1].fine == 4 && r.lane[1].val[6] == 99,
              "state: lane");
        CHECK(r.count == 1 && r.hits[0].lock[3] == 55 && r.hits[0].lock[0] == -1, "state: hit locks");
    }
    // ---- 1.3: the RATE, LOCK and LANE parameters edit the selected row, hit and lane
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(kFr); u.internalResume();
        waitWorker(); block(u, {});
        clearSeq(u); addHit(u, 2, 1, 0.5f); rebuild(u); block(u, {});
        auto knob = [&](int i, float v) { u.set(i, v); block(u, {}); block(u, {}); };
        knob(kSelRow, 3); knob(kHitSel, 1);
        knob(kRMode, 1); knob(kRRatio, 9); knob(kRFine, 5);
        const Row& w = u.testModel().seqs[0].rows[2];
        CHECK(w.mode == 1 && w.ratio == 9 && w.fine == 5, "RATE edits the row: %d %d %d", w.mode, w.ratio, w.fine);
        knob(kLock0 + 1, 77); knob(kLockCC0 + 1, (float)indexOfCC(74));
        CHECK(w.hits[0].lock[1] == 77 && w.lockCC[1] == 74, "LOCK edits: %d %d", w.hits[0].lock[1], w.lockCC[1]);
        knob(kLaneSel, 2); knob(kLnCC, (float)indexOfCC(30)); knob(kLnSteps, 12); knob(kLnDir, 1); knob(kLnRest, 9);
        CHECK(w.lane[1].cc == 30 && w.lane[1].steps == 12 && w.lane[1].dir == 1 && w.lane[1].rest == 9 && w.lane[0].cc == 0,
              "LANE edits lane 2 of the row");
        knob(kLaneSel, 0); knob(kLnSteps, 3);
        CHECK(w.lane[0].steps == 16 && w.lane[1].steps == 12, "LANE settings do nothing while VEL is shown");
        u.set(kSelRowNext, 1); block(u, {}); block(u, {});
        CHECK(u.testSelRow() == 3 && u.get(kSelRowNext) == 0, "Row > selects the next row: %d", u.testSelRow());
    }
    // ---- 1.3: CC numbers skip the reserved ones
    CHECK(!ccAllowed(7) && !ccAllowed(64) && !ccAllowed(99) && !ccAllowed(121) && !ccAllowed(0) && ccAllowed(74) && ccAllowed(6),
          "CC skip list");
    CHECK(numCCChoices() == 102 && ccOfIndex(indexOfCC(74)) == 74 && ccOfIndex(0) == 0, "CC choices: %d", numCCChoices());

    waitWorker();
    // ---- COPY / PASTE / CLEAR of the edited sequence (PASTE and CLEAR: two touches), clipboard shared by instances
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(64); u.internalResume();
        waitWorker(); block(u, {});
        clearSeq(u); addHit(u, 1, 2, 0.5f); u.testModel().seqs[0].rows[1].lane[0].cc = 40; u.testModel().seqs[0].rows[1].mode = 1; rebuild(u);
        block(u, {});
        auto tap = [&](PercSeq& x, int i) { x.set(i, 1 - x.get(i)); block(x, {}); block(x, {}); };
        u.set(kSequence, 1); block(u, {}); block(u, {});
        tap(u, kSeqCopy);
        CHECK(u.get(kSeqCopy) == 0, "COPY is set back to off");
        u.set(kSequence, 3); block(u, {}); block(u, {});
        tap(u, kSeqPaste);
        CHECK(u.testModel().seqs[2].rows[1].count == 0, "PASTE: one touch only arms");
        tap(u, kSeqPaste);
        CHECK(u.testModel().seqs[2].rows[1].count == 1 && u.testModel().seqs[2].rows[1].lane[0].cc == 40 && u.testModel().seqs[2].rows[1].mode == 1,
              "PASTE: the sequence with all its data");
        PercSeq w;
        w.internalSetSampleRate(48000); w.internalSetBlockSize(64); w.internalResume();
        waitWorker(); block(w, {});
        w.set(kSequence, 5); block(w, {}); block(w, {});
        tap(w, kSeqPaste); tap(w, kSeqPaste);
        CHECK(w.testModel().seqs[4].rows[1].count == 1 && w.testModel().seqs[4].rows[1].lane[0].cc == 40, "PASTE in another instance");
        tap(u, kSeqClear); tap(u, kSeqClear);
        CHECK(u.testModel().seqs[2].rows[1].count == 0 && u.testModel().seqs[2].rows[1].lane[0].cc == 0 && u.testModel().seqs[2].rows[1].mode == 0
              && u.testModel().seqs[0].rows[1].count == 1, "CLEAR: only the edited sequence, back to a new one");
    }

    // ---- SEND: one CC at 64 for MIDI Learn; nothing while the CC is off
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(64); u.internalResume();
        waitWorker(); block(u, {});
        u.set(kSequence, 1); block(u, {});
        u.testModel().seqs[0].rows[0].lockCC[0] = 74; u.set(kSelRow, 1);
        block(u, {}); takeLog(u);
        u.set(kSend0, 1); block(u, {}); block(u, {});
        for (int b = 0; b < 600; ++b) block(u, {});   // 0.8 s of audio: the zigzag runs
        CHECK(u.get(kSend0) > 0.5f, "SEND stays on while it sends");
        u.set(kSend0, 0); block(u, {}); block(u, {});   // touch again: stop with 64
        auto log = takeLog(u);
        std::vector<int> v;
        for (auto& m : log) if ((m[0] & 0xF0) == 0xB0 && m[1] == 74) v.push_back(m[2]);
        CHECK(v.size() >= 4 && v[0] == 0 && v[1] == 127 && v[2] == 0 && v.back() == 64, "SEND: zigzag 0 127 0 ... then 64 (%zu values)", v.size());
        u.set(kSend0, 1); block(u, {});
        for (int b = 0; b < 11300; ++b) block(u, {});   // 15 s of audio
        CHECK(u.get(kSend0) == 0, "SEND stops by itself after 15 s");
        takeLog(u);
        u.set(kSend0 + 1, 1); block(u, {}); block(u, {});   // Lock CC 2 is off
        u.set(kSend0 + 4, 1); block(u, {}); block(u, {});   // VEL is shown: no lane CC
        int cc = 0;
        for (auto& m : takeLog(u)) if ((m[0] & 0xF0) == 0xB0) ++cc;
        CHECK(cc == 0, "SEND sends nothing while the CC is off (%d)", cc);
    }

    // ---- 2026-11-22: a Range End at the last step follows Steps; one inside stays
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(kFr); u.internalResume();
        waitWorker(); block(u, {});
        Row& w = u.testModel().seqs[0].rows[0];
        u.set(kRSteps, 16); block(u, {});
        CHECK(w.rEnd == 15, "Range End at 16: %d", w.rEnd + 1);
        u.set(kRSteps, 1); block(u, {});
        CHECK(w.rEnd == 0, "Steps 1 clips Range End: %d", w.rEnd + 1);
        u.set(kRSteps, 16); block(u, {});
        CHECK(w.rEnd == 15, "Range End follows Steps back to 16: %d", w.rEnd + 1);
        u.set(kREnd, 8); block(u, {});
        u.set(kRSteps, 32); block(u, {});
        CHECK(w.rEnd == 7, "Range End inside the steps stays at 8: %d", w.rEnd + 1);
        u.set(kRSteps, 4); block(u, {});
        CHECK(w.rEnd == 3, "Steps below Range End clips it: %d", w.rEnd + 1);
    }
    // ---- 2026-11-22: hits outside Steps are kept (not deleted); a cut hit gets its full length back
    {
        PercSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(kFr); u.internalResume();
        waitWorker(); block(u, {});
        u.set(kSelRow, 1); block(u, {});
        Row& w = u.testModel().seqs[0].rows[0];
        auto setup = [&]() { w = Row(); w.note = 36; addHit(u, 0, 0, 1); addHit(u, 0, 3, 4); addHit(u, 0, 10, 1); };
        auto steps = [&](int n) { u.set(kRSteps, (float)n); block(u, {}); };
        setup(); steps(16);
        steps(4);
        CHECK(w.count == 2 && near(w.hits[1].len, 1) && w.keptOutside() == 1, "Steps 4: 2 shown (one cut), 1 kept: %d %d", w.count, w.keptOutside());
        steps(1);
        CHECK(w.count == 1 && w.keptOutside() == 2, "Steps 1: 1 shown, 2 kept: %d %d", w.count, w.keptOutside());
        steps(16);
        CHECK(w.count == 3 && near(w.hits[1].st, 3) && near(w.hits[1].len, 4) && near(w.hits[2].st, 10) && w.kept == 0,
              "Steps back to 16: all 3 hits, the cut one at full length (%d, len %f)", w.count, w.count > 1 ? w.hits[1].len : -1.0f);
        // an edit of the cut hit counts: VEL edit, then Steps up -> it stays cut
        steps(4); w.hits[1].v = 0.5f; steps(16);
        CHECK(w.count == 3 && near(w.hits[1].len, 1), "VEL edit of a cut hit: its length stays cut: %f", w.hits[1].len);
        setup(); steps(16); steps(4); w.hits[1].len = 0.5f; steps(16);
        CHECK(near(w.hits[1].len, 0.5f), "length edit of a cut hit stays: %f", w.hits[1].len);
        // delete the cut hit, save, load: it stays gone; kept hits survive save/load while Steps is short
        setup(); steps(16); steps(4); w.remove(1);
        u.testSaveAll(); block(u, {});
        std::vector<uint8_t> out;
        u.saveExtraState(out);
        PercSeq q;
        q.internalSetSampleRate(48000); q.internalSetBlockSize(kFr); q.internalResume();
        q.loadExtraState(out.data(), out.size()); q.internalResume(); block(q, {});
        const Row& l = q.testModel().seqs[0].rows[0];
        CHECK(l.steps == 4 && l.count == 1 && l.keptOutside() == 1, "load with Steps 4: 1 shown, 1 kept: %d %d", l.count, l.keptOutside());
        q.set(kSelRow, 1); block(q, {}); q.set(kRSteps, 16); block(q, {});
        CHECK(l.count == 2 && near(l.hits[1].st, 10), "after the load, Steps 16 brings the kept hit back; the deleted one stays gone: %d", l.count);
        // a full row (64 with the kept hits) takes no new hit
        Row f;
        for (int i = 0; i < kMaxHits; ++i) { Hit h; h.st = i * 0.5f; h.len = 0.25f; f.insert(h); }
        f.steps = 32; f.setSteps(8, u.testScratch());
        CHECK(f.full() && f.insert(Hit()) == -1 && f.count + f.kept == kMaxHits, "kept hits count toward the 64");
        // PASTE takes the kept hits along; CLEAR removes them
        setup(); steps(16); steps(4);
        auto tap = [&](PercSeq& x, int i) { x.set(i, 1 - x.get(i)); block(x, {}); block(x, {}); };
        tap(u, kSeqCopy);
        u.set(kSequence, 2); block(u, {}); block(u, {});
        tap(u, kSeqPaste); tap(u, kSeqPaste);
        Row& pr = u.testModel().seqs[1].rows[0];
        CHECK(pr.count == 2 && pr.keptOutside() == 1, "PASTE: kept hits too: %d %d", pr.count, pr.keptOutside());
        tap(u, kSeqClear); tap(u, kSeqClear);
        CHECK(pr.count == 0 && pr.kept == 0, "CLEAR: kept hits too");
        // CC lane values survive Lane Steps 16 -> 1 -> 16
        u.set(kSequence, 1); block(u, {}); block(u, {});
        w.lane[0].val[9] = 99;
        u.set(kLaneSel, 1); block(u, {}); u.set(kLnSteps, 1); block(u, {}); u.set(kLnSteps, 16); block(u, {});
        CHECK(w.lane[0].val[9] == 99 && w.lane[0].steps == 16, "CC lane values survive Lane Steps 16 -> 1 -> 16");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all plugin tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
