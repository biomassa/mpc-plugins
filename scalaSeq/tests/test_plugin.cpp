// Plugin-level tests for scalaSeq: step entry, note edits, playback to MIDI, state.
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

int main()
{
    ScalaSeq p;
    p.internalSetSampleRate(48000); p.internalSetBlockSize(64); p.internalResume();
    waitWorker();
    block(p, {});
    takeLog(p);

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

    // ---- the NOTE knobs follow the selection (the last entered note)
    waitWorker();
    CHECK((int)std::lround(p.get(kNote)) == 3 && (int)std::lround(p.get(kDegree)) == 4, "note 3 selected: %f %f", p.get(kNote), p.get(kDegree));

    // ---- edits: select note 2, set degree 7, length 2 (clamped by the next note to 1)
    p.set(kStepEntry, 0);
    p.set(kNote, 2); block(p, {});
    p.set(kDegree, 7); block(p, {});
    CHECK(s.notes[1].pitch == kCenter + 7, "degree edit: %d", s.notes[1].pitch - kCenter);
    p.set(kLength, 2); block(p, {});
    CHECK(std::fabs(s.notes[1].len - 1) < 1e-5, "length clamped by the next note: %f", s.notes[1].len);
    // move note 3 from 2 to 5, then make note 2 longer
    p.set(kNote, 3); block(p, {});
    p.set(kStart, 5); block(p, {});
    CHECK(std::fabs(s.notes[2].st - 5) < 1e-5, "moved to 5: %f", s.notes[2].st);
    p.set(kNote, 2); block(p, {});
    p.set(kLength, 3); block(p, {});
    CHECK(std::fabs(s.notes[1].len - 3) < 1e-5, "longer now: %f", s.notes[1].len);
    // chance lane 1 on note 1: op P+ sets prob 100 and the default amount
    p.set(kNote, 1); block(p, {});
    p.set(kLane0, 16); block(p, {});
    CHECK(s.notes[0].ch[0].op == 16 && s.notes[0].ch[0].prob == 1.0f && s.notes[0].ch[0].amt == 1, "lane op set");
    p.set(kLane0, 0); block(p, {});

    // ---- playback: 120 bpm, 1/16 steps (0.25 q), 16 steps; notes at steps 0 (deg 0), 1 (deg 7), 5 (deg 4)
    takeLog(p);
    double ppq = 0;
    std::vector<int> ons;
    for (int b = 0; b < 750; ++b)   // 1 s at 48 kHz = 2 quarter notes = 8 steps
    {
        block(p, {}, ppq);
        ppq += 64 * (120.0 / 60.0 / 48000.0);
    }
    log = takeLog(p);
    for (auto& m : log) if ((m[0] & 0xF0) == 0x90) ons.push_back(m[1]);
    CHECK(ons.size() == 3 && ons[0] == 60 && ons[1] == 67 && ons[2] == 64, "played 60 67 64: %zu", ons.size());
    block(p, {});   // stop
    log = takeLog(p);

    // ---- delete note 1
    p.set(kNote, 1); block(p, {});
    p.set(kDelete, 1); block(p, {});
    CHECK(s.count == 2 && s.notes[0].pitch == kCenter + 7, "deleted: %d", s.count);

    // ---- the grid shows the notes: note at step 1, degree 7 -> row (7 - viewLow) from the bottom
    waitWorker();
    block(p, {});
    waitWorker();
    {
        const int viewLow = (int)std::lround(p.get(kViewLow));
        const int rowFromBottom = 7 - viewLow;
        const float y = kMarkPark + (rowFromBottom + 0.5f) / 16 * (1 - kMarkPark);
        // step 1 = column 4: body and start markers at the note's row; column 3 empty (parked)
        CHECK(std::fabs(p.get(kMarkA0 + 4) - y) < 1e-4 && std::fabs(p.get(kMarkB0 + 4) - y) < 1e-4,
              "markers at the note start: %f %f (want %f)", p.get(kMarkA0 + 4), p.get(kMarkB0 + 4), y);
        CHECK(std::fabs(p.get(kMarkA0 + 7) - y) < 1e-4 && p.get(kMarkB0 + 7) == 0.0f, "body marker in the note, no start marker");
        CHECK(p.get(kMarkA0 + 3) == 0.0f, "empty column parked");
    }

    // ---- touch: tap an empty cell (row 3, step 8) = new note; drag = length; tap it again = delete
    {
        auto tap = [&](float x, float y) { p.set(kTouchX, x); p.set(kTouchY, y); block(p, {}); };
        auto idle = [&](int ms) { for (int i = 0; i < ms * 48 / 64 + 2; ++i) block(p, {}); };
        const int before = s.count;
        const int viewLow = (int)std::lround(p.get(kViewLow));
        tap((32 + 0.5f) / 64, (3 + 0.5f) / 16);   // column 32 = step 8, row 3
        idle(30);                                   // settle -> touch begins
        CHECK(s.count == before + 1, "tap placed a note: %d", s.count);
        int idx = -1;
        for (int i = 0; i < s.count; ++i) if (std::fabs(s.notes[i].st - 8) < 1e-5) idx = i;
        CHECK(idx >= 0 && s.notes[idx].pitch == kCenter + viewLow + 3, "note at step 8, row 3");
        // drag to column 39 (step 9.75): length 2 steps (end snapped up to the 1/4 grid = 10)
        tap((39 + 0.5f) / 64, (3 + 0.5f) / 16);
        idle(200);                                  // release
        CHECK(idx >= 0 && std::fabs(s.notes[idx].len - 2) < 1e-5, "drag set the length: %f", idx >= 0 ? s.notes[idx].len : -1);
        CHECK(p.get(kTouchX) == 1.0f, "puck parked after release");
        // tap the selected note again: deleted
        tap((33 + 0.5f) / 64, (3 + 0.5f) / 16);
        idle(200);
        CHECK(s.count == before, "second tap deleted it: %d", s.count);
        // a pad outside the 16 pads is ignored
        const int c0 = s.count;
        p.set(kStepEntry, 1); block(p, {});
        block(p, {mpc::MidiEvent::noteOn(0, 100, 100), mpc::MidiEvent::noteOff(0, 100)});
        CHECK(s.count == c0 && (int)std::lround(p.get(kLastKey)) == 100, "pad outside ignored, last key shown");
        p.set(kStepEntry, 0); block(p, {});
    }

    // ---- rows sized by cents: 12-edo rows are equal; ji_7 rows follow its intervals
    {
        block(p, {});
        const float h1 = p.get(kRowLine0 + 1) - p.get(kRowLine0), h2 = p.get(kRowLine0 + 2) - p.get(kRowLine0 + 1);
        CHECK(std::fabs(h1 - h2) < 1e-4, "12-edo: equal rows (%f %f)", h1, h2);
        int ji = -1;
        for (size_t i = 0; i < p.param(kScale).choices.size(); ++i) if (p.param(kScale).choices[i] == "ji_7") ji = (int)i;
        CHECK(ji >= 0, "ji_7 listed");
        p.set(kViewLow, 0); block(p, {});
        p.set(kScale, (float)ji);
        waitWorker(); block(p, {}); waitWorker(); block(p, {});
        // ji_7 degrees from 0: 10/9 (182.4 c), 128/105 (342.9 c) -> row heights 182.4 and 160.5 cents
        const float r0 = p.get(kRowLine0), r1 = p.get(kRowLine0 + 1), r2 = p.get(kRowLine0 + 2);
        const double c1 = 1200 * std::log2(10.0 / 9), c2 = 1200 * std::log2(128.0 / 105);
        CHECK(std::fabs((r1 - r0) / (r2 - r1) - c1 / (c2 - c1)) < 1e-2, "ji_7 row heights by cents: %f vs %f", (r1 - r0) / (r2 - r1), c1 / (c2 - c1));
        CHECK(p.get(kPerLine0) == r0, "period line at degree 0");
        CHECK(std::fabs(p.get(kLab0 + 0) - (r0 + r1) / 2) < 1e-4, "label 0 centred on its row");
        // touch maps through the cents rows: a tap just above the first row line lands in row 1
        p.set(kScale, 0); waitWorker(); block(p, {}); waitWorker(); block(p, {});
    }

    // ---- state round trip
    std::vector<uint8_t> st;
    block(p, {});
    p.saveExtraState(st);
    ScalaSeq q;
    q.internalSetSampleRate(48000); q.internalSetBlockSize(64);
    q.loadExtraState(st.data(), st.size());
    q.internalResume();
    CHECK(q.testModel().seqs[0].count == 2 && q.testModel().seqs[0].notes[1].len == s.notes[1].len, "state restored");

    std::printf(g_fail ? "%d FAILED\n" : "plugin tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
