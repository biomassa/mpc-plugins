// Plugin-level tests for scalaSeq 2.0: step entry, knobs, playback to MIDI, the piano roll
// display, the touch gestures (roll, ruler, labels, velocity lane, FREE) and state.
#define SCALASEQ_TEST 1
#include "../src/ScalaSeq.cpp"

#include <cstdio>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void waitWorker() { std::this_thread::sleep_for(std::chrono::milliseconds(40)); }

static std::vector<std::array<uint8_t, 3>> takeLog(ScalaSeq& p)
{
    waitWorker();
    std::lock_guard<std::mutex> l(p.testMutex_);
    auto v = p.testLog_;
    p.testLog_.clear();
    return v;
}

// one block; transport playing at ppq (or stopped when ppq < 0)
static void block(ScalaSeq& p, std::vector<mpc::MidiEvent> ev, double ppq = -1, int frames = 64)
{
    static float l[512], r[512];
    float* outs[2] = {l, r};
    mpc::ProcessContext ctx;
    ctx.outputs = outs; ctx.numOutputs = 2; ctx.numFrames = frames; ctx.sampleRate = 48000;
    ctx.midi = ev.data(); ctx.numMidiEvents = (int)ev.size();
    if (ppq >= 0)
    {
        ctx.transport.valid = true; ctx.transport.playing = true; ctx.transport.ppqValid = true;
        ctx.transport.tempoValid = true; ctx.transport.bpm = 120; ctx.transport.ppqPos = ppq;
    }
    p.process(ctx);
}

// ---- touch helpers: pixels -> the XYPad value (the puck is 2 px: v = (px - 1) / (pad - 2))
static float pv(float px, float pad) { return std::max(0.0f, std::min(1.0f, (px - 1) / (pad - 2))); }
static void touchAt(ScalaSeq& p, int px, float x, float y) { p.set(px, x); p.set(px + 1, y); block(p, {}); }
static void lift(ScalaSeq& p, int px) { p.set(px, 0); block(p, {}); p.set(px + 1, 0); block(p, {}); }
// the roll: position in steps (view start 0, 16 steps on the page) and row (centre of 12-edo rows)
static float rx(float step) { return pv(step / 16 * kGW, kGW); }
static float ry(int row) { return pv((row + 0.5f) * kGH / 16, kGH); }

int main()
{
    ScalaSeq p;
    p.internalSetSampleRate(48000); p.internalSetBlockSize(64); p.internalResume();
    waitWorker();
    block(p, {});
    takeLog(p);
    CHECK(kNumParams < 2000, "parameter count %d", (int)kNumParams);

    // ---- step entry: pads 1, 3, 5 (Pad 1 Note = 60, so keys 60, 62, 64 = rows 0, 2, 4)
    p.set(kPadNote, 60);
    p.set(kStepEntry, 1);
    block(p, {});
    block(p, {mpc::MidiEvent::noteOn(0, 60, 100), mpc::MidiEvent::noteOff(0, 60)});
    block(p, {mpc::MidiEvent::noteOn(0, 62, 100), mpc::MidiEvent::noteOff(0, 62)});
    block(p, {mpc::MidiEvent::noteOn(0, 64, 100), mpc::MidiEvent::noteOff(0, 64)});
    Sequence& s = p.testModel().seqs[0];
    CHECK(s.count == 3, "3 notes entered: %d", s.count);
    if (s.count == 3)
    {
        CHECK(s.notes[0].st == 0 && s.notes[1].st == 1 && s.notes[2].st == 2, "starts 0 1 2");
        CHECK(s.notes[1].pitch == kCenter + 2 && s.notes[2].pitch == kCenter + 4, "pitches");
    }
    CHECK(std::fabs(p.testCursor() - 3) < 1e-5, "cursor moved to 3: %f", p.testCursor());
    auto log = takeLog(p);
    int previewOn = 0;
    for (auto& m : log) if ((m[0] & 0xF0) == 0x90) previewOn++;
    CHECK(previewOn == 3, "preview notes sounded: %d", previewOn);
    p.set(kStepEntry, 0);
    block(p, {});

    // ---- knobs: Note select 1-64, Degree spread over the scale, Period, Move, Length, Nudge
    auto knob = [&](int prm, float v) { p.set(prm, v); block(p, {}); };
    CHECK(p.testSel() == 2, "the last entered note is selected: %d", p.testSel());
    knob(kNoteSel, 2);
    CHECK(p.testSel() == 1, "Note 2 selected");
    std::string txt; p.formatValue(kNoteSel, 2, txt);
    CHECK(txt == "2/3", "Note text: %s", txt.c_str());
    knob(kNoteSel, 10);
    CHECK(p.testSel() == 2 && (int)std::lround(p.get(kNoteSel)) == 3, "Note select stops at the last note");
    knob(kNoteSel, 2);
    knob(kDeg, 40);                                   // 12-edo: 40 * 12 / 64 = degree 7
    CHECK(s.notes[1].pitch == kCenter + 7, "degree knob: %d", s.notes[1].pitch - kCenter);
    knob(kPeriod, 1);
    CHECK(s.notes[1].pitch == kCenter + 19, "period +1: %d", s.notes[1].pitch - kCenter);
    knob(kPeriod, 0);
    knob(kLen, 12);                                   // Snap 1/4: 12 = 3 steps (overlap allowed)
    CHECK(std::fabs(s.notes[1].len - 3) < 1e-5, "length 3: %f", s.notes[1].len);
    knob(kNoteSel, 3);
    knob(kMove, 20);                                  // 20 snap steps = step 5
    CHECK(std::fabs(s.notes[2].st - 5) < 1e-5, "moved to 5: %f", s.notes[2].st);
    knob(kNudge, 6);
    CHECK(std::fabs(s.notes[2].st - (5 + 6 / 96.0f)) < 1e-5, "nudge: %f", s.notes[2].st);
    knob(kNudge, 31);                                 // limited inside half a snap step (1/4: 11/96)
    CHECK(std::fabs(s.notes[2].st - (5 + 11 / 96.0f)) < 1e-5, "nudge limited: %f", s.notes[2].st);
    knob(kNudge, 0);
    knob(kNoteSel, 1);
    p.set(kLane0, 16); block(p, {});                  // chance lane op: P+ sets prob 100 and the default amount
    CHECK(s.notes[0].ch[0].op == 16 && s.notes[0].ch[0].prob == 0.0f && s.notes[0].ch[0].amt == 1, "lane op set, Prob stays 0");
    p.set(kLane0, 0); block(p, {});

    // ---- playback: 120 bpm, 1/16 steps; notes at steps 0 (deg 0), 1 (deg 7, 3 long), 5 (deg 4)
    takeLog(p);
    double ppq = 0;
    std::vector<int> ons;
    for (int b = 0; b < 750; ++b) { block(p, {}, ppq); ppq += 64 * (120.0 / 60.0 / 48000.0); }
    log = takeLog(p);
    for (auto& m : log) if ((m[0] & 0xF0) == 0x90) ons.push_back(m[1]);
    CHECK(ons.size() == 3 && ons[0] == 60 && ons[1] == 67 && ons[2] == 64, "played 60 67 64: %zu", ons.size());
    {
        // 2.2: every note-on comes right after a pitch bend on its channel (a synth that takes all
        // channels has one bend for all of them)
        int withBend = 0, noteOns = 0;
        for (size_t i = 0; i < log.size(); ++i)
            if ((log[i][0] & 0xF0) == 0x90 && log[i][2] > 0)
            {
                ++noteOns;
                if (i > 0 && (log[i - 1][0] & 0xF0) == 0xE0 && (log[i - 1][0] & 0x0F) == (log[i][0] & 0x0F)) ++withBend;
            }
        CHECK(noteOns == 3 && withBend == noteOns, "bend before every note-on: %d of %d", withBend, noteOns);
    }
    block(p, {});
    takeLog(p);

    // ---- delete note 1 with the Delete switch
    knob(kNoteSel, 1);
    p.set(kDelete, 1); block(p, {});
    CHECK(s.count == 2 && s.notes[0].pitch == kCenter + 7, "deleted: %d", s.count);

    // ---- display: note 1 (step 1, 3 steps, degree 7) is drawn exactly
    waitWorker(); block(p, {});
    {
        const int r = 7 - p.testViewLow();
        const float y = ((r + 0.5f) * kGH / 16 - kNoteH / 2) / (kGH - kNoteH);
        CHECK(std::fabs(p.get(kNoteY0) - y) < 1e-3, "note row: %f want %f", p.get(kNoteY0), y);
        // pieces: 3 steps = 165 px = 128 + 32 + 4 + 1, starting at 55 px
        auto left = [&](int k) { return p.get(kPieceX0 + k) * (kOFF + kGW) - kOFF; };
        CHECK(std::fabs(left(7) - 55) < 0.5f && std::fabs(left(5) - 183) < 0.5f && std::fabs(left(2) - 215) < 0.5f && std::fabs(left(0) - 219) < 0.5f,
              "pieces at 55 183 215 219: %f %f %f %f", left(7), left(5), left(2), left(0));
        CHECK(p.get(kPieceX0 + 6) == 0 && p.get(kPieceX0 + 1) == 0, "unused pieces parked off screen");
        CHECK(std::fabs(p.get(kNotchX0) * (kOFF + kGW) - kOFF - 55) < 0.5f, "notch at the start");
        // lines: 17 step lines, Snap 1/4 = 13.75 px apart: 48 snap lines; ruler numbers every step (55 px)
        CHECK(p.get(kStepLine0 + 16) < 1.0f && p.get(kStepLine0 + 17) == 1.0f, "17 step lines");
        CHECK(p.get(kSnapLine0 + 47) < 1.0f && p.get(kSnapLine0 + 48) == 1.0f, "48 snap lines");
        CHECK(p.get(kNumX0 + 0) > 0 && p.get(kNumX0 + 15) > 0 && p.get(kNumX0 + 16) == 0, "numbers 1-16");
    }

    // ---- touch: draw, tap, move, length, double tap, FREE, overlap
    {
        auto idle = [&](int ms) { for (int i = 0; i < ms * 48 / 64 + 2; ++i) block(p, {}); };
        const int base = s.count;
        const int vl = p.testViewLow();
        // draw: press at step 8.1, row 3; drag to 10.1; lift -> start 8, length 2
        touchAt(p, kRollTX, rx(8.1f), ry(3));
        touchAt(p, kRollTX, rx(9.0f), ry(3));
        touchAt(p, kRollTX, rx(10.1f), ry(3));
        lift(p, kRollTX);
        int drawn = -1;
        for (int i = 0; i < s.count; ++i) if (std::fabs(s.notes[i].st - 8) < 1e-5) drawn = i;
        CHECK(s.count == base + 1 && drawn >= 0, "drawn a note at 8");
        if (drawn >= 0) CHECK(std::fabs(s.notes[drawn].len - 2) < 1e-5 && s.notes[drawn].pitch == kCenter + vl + 3, "length 2, row 3: %f", s.notes[drawn].len);
        // draw leftwards: press at 7.9 (snap 8), row 5, drag to 6 -> 6..8
        touchAt(p, kRollTX, rx(7.9f), ry(5));
        touchAt(p, kRollTX, rx(6.0f), ry(5));
        lift(p, kRollTX);
        int left = -1;
        for (int i = 0; i < s.count; ++i) if (std::fabs(s.notes[i].st - 6) < 1e-5) left = i;
        CHECK(left >= 0 && std::fabs(s.notes[left].len - 2) < 1e-5, "drawn leftwards: 6..8");
        // tap empty at 12: Entry Len (1 step)
        idle(400);
        touchAt(p, kRollTX, rx(12.05f), ry(3));
        lift(p, kRollTX);
        int tapped = -1;
        for (int i = 0; i < s.count; ++i) if (std::fabs(s.notes[i].st - 12) < 1e-5) tapped = i;
        CHECK(tapped >= 0 && std::fabs(s.notes[tapped].len - 1) < 1e-5, "tap = Entry Len");
        // move the note at 8 (row 3): press its body at 8.3, drag 1 step right and 2 rows up
        idle(400);
        touchAt(p, kRollTX, rx(8.3f), ry(3));
        touchAt(p, kRollTX, rx(8.8f), ry(4));
        touchAt(p, kRollTX, rx(9.3f), ry(5));
        lift(p, kRollTX);
        int moved = -1;
        for (int i = 0; i < s.count; ++i) if (std::fabs(s.notes[i].st - 9) < 1e-5) moved = i;
        CHECK(moved >= 0 && s.notes[moved].pitch == kCenter + vl + 5, "moved to 9, row 5");
        // length: press the right third (9..11: at 10.8), drag to 12 -> length 3
        idle(400);
        touchAt(p, kRollTX, rx(10.8f), ry(5));
        touchAt(p, kRollTX, rx(11.4f), ry(5));
        touchAt(p, kRollTX, rx(12.0f), ry(5));
        lift(p, kRollTX);
        moved = -1;
        for (int i = 0; i < s.count; ++i) if (std::fabs(s.notes[i].st - 9) < 1e-5) moved = i;
        CHECK(moved >= 0 && std::fabs(s.notes[moved].len - 3) < 1e-5, "length by the right end: %f", moved >= 0 ? s.notes[moved].len : -1);
        // double tap the note at 12 (row 3): deleted; a slow second tap only selects
        const int before = s.count;
        idle(400);
        touchAt(p, kRollTX, rx(12.3f), ry(3)); lift(p, kRollTX);
        idle(500);
        touchAt(p, kRollTX, rx(12.3f), ry(3)); lift(p, kRollTX);
        CHECK(s.count == before, "slow second tap does not delete");
        idle(100);
        touchAt(p, kRollTX, rx(12.3f), ry(3)); lift(p, kRollTX);
        CHECK(s.count == before - 1, "double tap deletes: %d", s.count);
        // FREE held (second finger): drawing does not snap
        idle(400);
        touchAt(p, kFreeTX, 0.5f, 0.5f);
        CHECK(p.get(kFreeLit) > 0, "FREE lit");
        touchAt(p, kRollTX, rx(13.5f), ry(8));
        touchAt(p, kRollTX, rx(14.2f), ry(8));
        lift(p, kRollTX);
        lift(p, kFreeTX);
        CHECK(p.get(kFreeLit) == 1.0f, "FREE dark (parked off the right edge)");
        int fr = -1;
        for (int i = 0; i < s.count; ++i) if (s.notes[i].pitch == kCenter + vl + 8) fr = i;
        CHECK(fr >= 0 && std::fabs(s.notes[fr].st - 13.5f) < 0.02f && std::fabs(s.notes[fr].len - 0.7f) < 0.03f,
              "free note at 13.5, 0.7 long: %f %f", fr >= 0 ? s.notes[fr].st : -1, fr >= 0 ? s.notes[fr].len : -1);
    }

    // ---- ruler: drag sideways = scroll, up/down = zoom; labels: drag = rows; velocity lane
    {
        knob(kSteps, 64);
        const float rulH = kRulerPadH;
        touchAt(p, kRulTX, pv(400, kGW), 0.5f);
        touchAt(p, kRulTX, pv(400 - 2 * 55, kGW), 0.5f);   // 2 steps to the left: the view moves right
        lift(p, kRulTX);
        CHECK(p.testViewStart() == 2, "ruler scroll: view start %d", p.testViewStart());
        touchAt(p, kRulTX, pv(3, kGW), 0.5f);
        touchAt(p, kRulTX, pv(3, kGW), 0.5f - 48 / (rulH - 2));   // 48 px down = 4 more steps
        lift(p, kRulTX);
        CHECK(p.getInt(kZoom) == 20, "ruler zoom: %d", p.getInt(kZoom));
        p.set(kZoom, 16); p.set(kViewStart, 1); block(p, {}); block(p, {});
        const int vl = p.testViewLow();
        touchAt(p, kLabTX, 0.5f, 0.5f);
        touchAt(p, kLabTX, 0.5f, 0.5f - 2 * (kGH / 16) / (kGH - 2));   // 2 rows down = 2 rows higher
        lift(p, kLabTX);
        CHECK(p.testViewLow() == vl + 2, "label drag: %d -> %d", vl, p.testViewLow());
        knob(kViewLow, vl);
        // velocity: the bar of the note at step 1 (55 px)
        int n1 = -1;
        for (int i = 0; i < s.count; ++i) if (std::fabs(s.notes[i].st - 1) < 1e-5) n1 = i;
        touchAt(p, kVelTX, pv(57, kGW), pv(30, kVelH));
        touchAt(p, kVelTX, pv(57, kGW), pv(15, kVelH));
        lift(p, kVelTX);
        CHECK(n1 >= 0 && std::fabs(s.notes[n1].v - 15.0f / 60) < 0.03f && p.testSel() == n1, "velocity from the lane: %f", n1 >= 0 ? s.notes[n1].v : -1);
        knob(kSteps, 16);
    }

    // ---- rows sized by cents: ji_7 rows follow its intervals
    {
        block(p, {});
        int ji = -1;
        for (size_t i = 0; i < p.param(kScale).choices.size(); ++i) if (p.param(kScale).choices[i] == "ji_7") ji = (int)i;
        CHECK(ji >= 0, "ji_7 listed");
        knob(kViewLow, 0);
        p.set(kScale, (float)ji);
        waitWorker(); block(p, {}); waitWorker(); block(p, {});
        const float rp = kGH + kPARKT;
        auto rowFrac = [&](float v, float h) { return (v * (rp - h) + h / 2) / kGH; };
        const float r0 = rowFrac(p.get(kRowLine0), 1), r1 = rowFrac(p.get(kRowLine0 + 1), 1), r2 = rowFrac(p.get(kRowLine0 + 2), 1);
        const double c1 = 1200 * std::log2(10.0 / 9), c2 = 1200 * std::log2(128.0 / 105);
        CHECK(std::fabs((r1 - r0) / (r2 - r1) - c1 / (c2 - c1)) < 3e-2, "ji_7 row heights by cents: %f vs %f", (r1 - r0) / (r2 - r1), c1 / (c2 - c1));
        CHECK(std::fabs(rowFrac(p.get(kPerLine0), 2) - r0) < 3e-3, "period line at degree 0 (the bottom edge clamps by 1 px)");
        CHECK(std::fabs(rowFrac(p.get(kLab0 + 0), 17) - (r0 + r1) / 2) < 1e-3, "label 0 centred on its row");
        p.set(kScale, 0); waitWorker(); block(p, {}); waitWorker(); block(p, {});
    }

    // ---- 2.3 chance text, Clear Note, Clear All (arm, confirm within 3 s; or time out)
    {
        ScalaSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(64); u.internalResume();
        waitWorker(); block(u, {});
        Sequence& q = u.testModel().seqs[0];
        for (int i = 0; i < 2; ++i) { Note nt; nt.st = (float)i; nt.len = 1; nt.pitch = (int16_t)(kCenter + i); q.insert(nt); }
        q.notes[0].ch[0].op = 22; q.notes[0].ch[0].prob = 0.25f;   // REPEAT 25%
        q.notes[1].ch[1].op = 9;  q.notes[1].ch[1].prob = 0.47f;   // VEL-
        u.set(kNoteSel, 2); block(u, {}); u.set(kNoteSel, 1); block(u, {}); block(u, {});   // a real selection change redraws
        std::string t0, t1; u.formatValue(kChanceText0, 0, t0); u.formatValue(kChanceText0 + 1, 0, t1);
        CHECK(t0 == "1 REPEAT 25% +1" && t1 == "2 -", "chance text with amount: '%s' '%s'", t0.c_str(), t1.c_str());
        u.set(kClearNote, 1); block(u, {}); block(u, {});
        CHECK(q.notes[0].ch[0].op == 0 && q.notes[1].ch[1].op == 9 && u.get(kClearNote) == 0, "Clear Note clears only the selected note");
        q.notes[0].ch[0].op = 22; q.notes[0].ch[0].prob = 0.25f;
        // arm, then let 3 s pass: nothing cleared, switch off again
        u.set(kClearAll, 1); block(u, {});
        for (int i = 0; i < 3100 * 48 / 64; ++i) block(u, {});
        CHECK(q.notes[0].ch[0].op == 22 && q.notes[1].ch[1].op == 9 && u.get(kClearAll) == 0, "Clear All times out without clearing");
        // arm, confirm within 3 s: everything cleared
        u.set(kClearAll, 1); block(u, {});
        for (int i = 0; i < 500 * 48 / 64; ++i) block(u, {});
        u.set(kClearAll, 0); block(u, {});
        CHECK(q.notes[0].ch[0].op == 0 && q.notes[1].ch[1].op == 0, "Clear All after confirm clears every note");
    }

    // ---- 2.3.1: choices read as the MPC shows them; a move stops at the sequence end; the selected
    // note's chance ring is drawn above its highlight
    {
        ScalaSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(64); u.internalResume();
        waitWorker(); block(u, {});
        Sequence& q = u.testModel().seqs[0];
        const int vl = u.testViewLow();
        u.set(kSteps, 8); block(u, {});
        u.setNormalised(kEntryLen, 0.6171875f); block(u, {});   // measured on the MPC: shown as "1"
        CHECK(u.testChoice(kEntryLen) == 4, "Entry Len between choices reads as shown: %d", u.testChoice(kEntryLen));
        auto rx8 = [](float step) { return pv(step / 16 * kGW, kGW); };   // 16 steps on the page
        touchAt(u, kRollTX, rx8(2.0f), ry(4)); lift(u, kRollTX);
        CHECK(q.count == 1 && std::fabs(q.notes[0].len - 1.0f) < 1e-5, "tap gives Entry Len 1: %f", q.count ? q.notes[0].len : -1);
        // move it right past the end (8 steps): its end stops at 8
        for (int i = 0; i < 400 * 48 / 64; ++i) block(u, {});
        touchAt(u, kRollTX, rx8(2.3f), ry(4));
        for (float x = 2.3f; x <= 9.5f; x += 0.25f) touchAt(u, kRollTX, rx8(x), ry(4));
        lift(u, kRollTX);
        CHECK(std::fabs(q.notes[0].st + q.notes[0].len - 8.0f) < 1e-4 && std::fabs(q.notes[0].len - 1.0f) < 1e-5,
              "move stops at the sequence end: st %f len %f", q.notes[0].st, q.notes[0].len);
        // a chance op (at 0 %) on the selected note: its ring is drawn above the highlight
        u.set(kLane0, 22); block(u, {}); block(u, {});
        CHECK(q.notes[0].ch[0].op == 30, "Op knob position 22 = DELAY %% (op 30, after DELAY)");
        u.set(kLane0, 23); block(u, {}); block(u, {});
        CHECK(q.notes[0].ch[0].op == 22 && q.notes[0].ch[0].prob == 0.0f, "op set, Prob 0");
        CHECK(u.get(kSelChanceX) > 0 && u.get(kChanceX0) > 0, "selected note's ring shown above the highlight");
        (void)vl;
    }

    // ---- 2.2 snap: a move puts the start on a snap line (Nudge dropped); with hidden snap lines
    // (64 steps on the page, Snap 1/4 = 3.4 px) touch edits snap to whole steps
    {
        ScalaSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(64); u.internalResume();
        waitWorker(); block(u, {});
        Sequence& q = u.testModel().seqs[0];
        const int vl = u.testViewLow();
        u.set(kSteps, 16); block(u, {});
        Note nt; nt.st = 4 + 6 / 96.0f; nt.len = 1; nt.pitch = (int16_t)(kCenter + vl + 3); q.insert(nt);
        block(u, {});
        // drag its body 1 step right: start lands on 5.0 (offset dropped)
        touchAt(u, kRollTX, rx(4.4f), ry(3)); touchAt(u, kRollTX, rx(4.9f), ry(3)); touchAt(u, kRollTX, rx(5.4f), ry(3)); lift(u, kRollTX);
        CHECK(std::fabs(q.notes[0].st - 5.0f) < 1e-5, "move lands on the snap line: %f", q.notes[0].st);
        // 64 steps on the page: snap lines hidden, a tap at step 9.3 places at 9 (whole step)
        u.set(kZoom, 64); block(u, {}); block(u, {});
        for (int i = 0; i < 400 * 48 / 64; ++i) block(u, {});
        auto rx64 = [](float step) { return pv(step / 64 * kGW, kGW); };
        touchAt(u, kRollTX, rx64(9.3f), ry(6)); lift(u, kRollTX);
        int t9 = -1; for (int i = 0; i < q.count; ++i) if (q.notes[i].pitch == kCenter + vl + 6) t9 = i;
        CHECK(t9 >= 0 && std::fabs(q.notes[t9].st - 9.0f) < 1e-5, "hidden lines: tap snaps to the whole step: %f", t9 >= 0 ? q.notes[t9].st : -1);
    }

    // ---- notes of the same pitch do not overlap (they stop at the neighbour); other pitches may
    {
        ScalaSeq u;
        u.internalSetSampleRate(48000); u.internalSetBlockSize(64); u.internalResume();
        waitWorker();
        block(u, {});
        Sequence& q = u.testModel().seqs[0];
        const int vl = u.testViewLow();
        Note nt; nt.st = 4; nt.len = 1; nt.pitch = (int16_t)(kCenter + vl + 3); q.insert(nt);
        u.set(kSteps, 16); block(u, {});
        // draw in row 3 from 2 to 6: stops at the note at 4
        touchAt(u, kRollTX, rx(2.05f), ry(3)); touchAt(u, kRollTX, rx(4.0f), ry(3)); touchAt(u, kRollTX, rx(6.0f), ry(3)); lift(u, kRollTX);
        int a = -1; for (int i = 0; i < q.count; ++i) if (std::fabs(q.notes[i].st - 2) < 1e-5) a = i;
        CHECK(a >= 0 && std::fabs(q.notes[a].len - 2) < 1e-5, "drawing stops at the same-pitch note: %f", a >= 0 ? q.notes[a].len : -1);
        // the same drag one row higher (another pitch) may overlap
        touchAt(u, kRollTX, rx(2.05f), ry(4)); touchAt(u, kRollTX, rx(4.0f), ry(4)); touchAt(u, kRollTX, rx(6.0f), ry(4)); lift(u, kRollTX);
        int b = -1; for (int i = 0; i < q.count; ++i) if (std::fabs(q.notes[i].st - 2) < 1e-5 && q.notes[i].pitch == kCenter + vl + 4) b = i;
        CHECK(b >= 0 && std::fabs(q.notes[b].len - 4) < 1e-5, "another pitch overlaps: %f", b >= 0 ? q.notes[b].len : -1);
        // move the note at 2 (row 3) one step right: it would overlap the note at 4 -> it stays
        for (int i = 0; i < 400 * 48 / 64; ++i) block(u, {});
        touchAt(u, kRollTX, rx(2.3f), ry(3)); touchAt(u, kRollTX, rx(2.8f), ry(3)); touchAt(u, kRollTX, rx(3.3f), ry(3)); lift(u, kRollTX);
        a = -1; for (int i = 0; i < q.count; ++i) if (q.notes[i].pitch == kCenter + vl + 3 && q.notes[i].st < 3.5f) a = i;
        CHECK(a >= 0 && std::fabs(q.notes[a].st - 2) < 1e-5, "move into a same-pitch note is stopped: %f", a >= 0 ? q.notes[a].st : -1);
        // Length knob: stops at the note at 4 (2 steps = 8 snap steps of 1/4)
        u.set(kLen, 16); block(u, {});
        CHECK(a >= 0 && std::fabs(q.notes[a].len - 2) < 1e-5, "Length knob stops at the neighbour: %f", a >= 0 ? q.notes[a].len : -1);
    }

    // ---- timing: the MPC's song position is rounded to 1/960 quarter in each block. Notes between
    // steps (0.5, 2.25, 3.625) must start at their exact time in every loop, none lost or doubled.
    {
        ScalaSeq t;
        t.internalSetSampleRate(44100); t.internalSetBlockSize(128); t.internalResume();
        waitWorker();
        Sequence& q = t.testModel().seqs[0];
        const float starts[3] = {0.5f, 2.25f, 3.625f};
        for (float st0 : starts) { Note nt; nt.st = st0; nt.len = 0.5f; nt.pitch = kCenter; q.insert(nt); }
        t.set(kSteps, 4); t.set(kRangeEnd, 4);
        static float L[128], R[128]; float* outs[2] = {L, R};
        auto blk = [&](double ppq) {
            mpc::ProcessContext c; c.outputs = outs; c.numOutputs = 2; c.numFrames = 128; c.sampleRate = 44100;
            c.transport.valid = c.transport.playing = c.transport.ppqValid = c.transport.tempoValid = true;
            c.transport.bpm = 122.00007; c.transport.ppqPos = ppq; t.process(c); };
        { mpc::ProcessContext c; c.outputs = outs; c.numOutputs = 2; c.numFrames = 128; c.sampleRate = 44100; t.process(c); }
        waitWorker(); { std::lock_guard<std::mutex> l(t.testMutex_); t.testLog_.clear(); }
        const double qps = 122.00007 / 60.0 / 44100.0;
        std::vector<double> onsQ;   // note-on times in quarters (block start + nothing: per block)
        double exact = 0;
        for (int b = 0; b < 44100 * 8 / 128; ++b)
        {
            // as measured on the MPC One G2 (122 bpm, 128 frames): the reported position is ahead of
            // the exact one by 0, 0.0004, 0.0007 quarter in turn, so every 3rd block starts 0.0007 back
            static const double ahead[3] = {0.0, 0.0004, 0.0007};
            blk(exact + ahead[b % 3]);
            exact += 128 * qps;
            std::this_thread::sleep_for(std::chrono::microseconds(2500));
            std::lock_guard<std::mutex> l(t.testMutex_);
            for (auto& m : t.testLog_) if ((m[0] & 0xF0) == 0x90 && m[2]) onsQ.push_back(exact);
            t.testLog_.clear();
        }
        // 1/16 steps: 4 steps = 1 quarter; in 16 quarters: 16 loops x 3 notes
        const double stepQ = 0.25;
        int bad = 0;
        for (size_t k = 0; k < onsQ.size(); ++k)
        {
            const double want = (k / 3) * 4 * stepQ + starts[k % 3] * stepQ;
            if (std::fabs(onsQ[k] - want) > 0.008) ++bad;   // within about one block (128 samples = 0.006 q)
        }
        if (bad) for (size_t k = 0; k < 12 && k < onsQ.size(); ++k) std::printf("   on %.4f\n", onsQ[k]);
        CHECK(onsQ.size() >= 45 && onsQ.size() <= 49 && bad == 0, "MPC song position pattern: %zu notes, %d off time", onsQ.size(), bad);
    }

    // ---- scheduled MIDI: the sample clock gives even times although blocks come late by 0-10 ms
    {
        rt::SampleClock c;
        uint32_t rnd = 12345;
        const double sr = 44100;
        double worst = 0;
        for (int b = 0; b < 44100 * 60 / 128; ++b)   // one minute of 128-frame blocks
        {
            rnd = rnd * 1664525u + 1013904223u;
            const double late = (rnd >> 8) % 10000 / 1e6;        // 0 .. 10 ms
            const uint64_t smp = (uint64_t)b * 128;
            const double ideal = 5.0 + smp / sr;                 // seconds
            c.update((int64_t)((ideal + late) * 1e9), smp, sr);
            if (b > 100)
            {
                const double err = (c.timeOf(smp + 64) - (int64_t)((ideal + 64 / sr) * 1e9)) / 1e6;   // ms
                worst = std::max(worst, std::fabs(err));
            }
        }
        CHECK(worst < 1.0, "sample clock error with 0-10 ms late blocks: %.3f ms", worst);
    }

    // ---- state round trip (SSQ4); 1.x states are not loaded
    std::vector<uint8_t> st;
    block(p, {});
    p.saveExtraState(st);
    {
        ScalaSeq q;
        q.internalSetSampleRate(48000); q.internalSetBlockSize(64);
        q.loadExtraState(st.data(), st.size());
        q.internalResume();
        block(q, {});
        CHECK(q.testModel().seqs[0].count == s.count, "state restored: %d", q.testModel().seqs[0].count);
        const Note& n = q.testModel().seqs[0].notes[q.testSel()];
        CHECK((int)std::lround(q.get(kNoteSel)) == q.testSel() + 1, "Note knob after load");
        CHECK((int)std::lround(q.get(kLen)) == (int)std::lround(n.len / 0.25f), "Length knob after load");
    }
    {
        std::vector<uint8_t> old = st;
        old[3] = '3';
        ScalaSeq q;
        q.internalSetSampleRate(48000); q.internalSetBlockSize(64);
        q.loadExtraState(old.data(), old.size());
        q.internalResume();
        block(q, {});
        CHECK(q.testModel().seqs[0].count == 0, "a 1.x state is not loaded");
    }

    std::printf(g_fail ? "%d FAILED\n" : "plugin tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
