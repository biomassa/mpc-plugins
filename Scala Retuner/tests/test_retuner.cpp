// Unit tests for Scala Retuner (runs on the Mac/Linux build machine, no ALSA needed).
// Build + run: see tests/run.sh
#define SCALA_RETUNER_TEST 1
#include "../src/ScalaRetuner.cpp"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const double kRange = 24.0;

// Pitch (MIDI, fractional) the target will play for a table entry, under the given curve.
static double played(const scala::RetuneTable& t, int key, bool opx)
{
    const int b = scala::bendFor(t, key, 0.0f);
    double semis = b >= 0 ? b / (opx ? 8064.0 : 8192.0) * kRange : b / 8192.0 * kRange;
    if (opx && b >= 0 && b / 8064.0 > 1.0) semis = kRange;
    return t.note[key] + semis;
}

static void waitWorker() { std::this_thread::sleep_for(std::chrono::milliseconds(30)); }

static int indexOf(ScalaRetuner& p, int param, const char* name)
{
    const auto& ch = p.param(param).choices;
    for (size_t i = 0; i < ch.size(); ++i) if (ch[i] == name) return (int)i;
    return -1;
}

static std::vector<std::array<uint8_t, 3>> takeLog(ScalaRetuner& p)
{
    waitWorker();
    std::lock_guard<std::mutex> l(p.testMutex_);
    auto v = p.testLog_;
    p.testLog_.clear();
    return v;
}

static void sendMidi(ScalaRetuner& p, std::vector<mpc::MidiEvent> ev)
{
    float l[64] = {}, r[64] = {};
    float* outs[2] = {l, r};
    mpc::ProcessContext ctx;
    ctx.outputs = outs; ctx.numOutputs = 2; ctx.numFrames = 64;
    ctx.midi = ev.data(); ctx.numMidiEvents = (int)ev.size();
    p.process(ctx);
}

int main()
{
    // --- 1. Pure math --------------------------------------------------------------------
    {
        std::string text; scala::Scale s;
        CHECK(mpc::readTextFile(mpc::contentPath("Scales/ji_7.scl"), text) && scala::parseScl(text, s), "ji_7 parse");
        CHECK(s.size() == 7, "ji_7 size %d", s.size());
        CHECK(std::fabs(s.cents[1] - 1200 * std::log2(10.0 / 9.0)) < 1e-9, "ji_7 10/9");
        CHECK(std::fabs(s.cents[7] - 1200) < 1e-9, "ji_7 period");

        scala::Mapping m = scala::linearMapping(60, 0);
        scala::RetuneTable t;
        for (int curve = 0; curve < 2; ++curve)
        {
            scala::buildTable(s, m, 24, 24, (scala::BendCurve)curve, 2, t);
            double worst = 0;
            for (int k = 0; k < 128; ++k)
            {
                const int off = k - 60, q = scala::detail::floorDiv(off, 7), r = off - q * 7;
                const double want = 60 + (q * 1200 + s.cents[(size_t)r]) / 100.0;
                if (want < -0.5 || want > 127.49) { CHECK(t.note[k] == -1, "ji_7 key %d should be off", k); continue; }
                worst = std::max(worst, std::fabs(played(t, k, curve == 1) - want) * 100);
            }
            CHECK(worst < 0.35, "ji_7 curve %d worst error %.3f cents", curve, worst);
            std::printf("ji_7 linear, curve %s: worst error %.3f cents\n", curve ? "OPx-4" : "Symmetric", worst);
        }

        CHECK(mpc::readTextFile(mpc::contentPath("Scales/22edo.scl"), text) && scala::parseScl(text, s), "22edo parse");
        scala::buildTable(s, scala::linearMapping(60, 0), 24, 24, scala::BendCurve::Symmetric, 2, t);
        double worst = 0;
        for (int k = 0; k < 128; ++k)
        {
            const double want = 60 + (k - 60) * 12.0 / 22.0;
            if (t.note[k] < 0) continue;
            worst = std::max(worst, std::fabs(played(t, k, false) - want) * 100);
        }
        CHECK(worst < 0.35, "22edo worst %.3f", worst);
        std::printf("22edo linear: worst error %.3f cents, key 61 -> note %d bend %d\n", worst, t.note[61], scala::bendFor(t, 61, 0));

        // Small bend ranges give finer steps.
        scala::buildTable(s, scala::linearMapping(60, 0), 2, 2, scala::BendCurve::Symmetric, 2, t);
        CHECK(t.note[61] == 61 && std::abs(scala::bendFor(t, 61, 0) - std::lround(-45.4545 / 200 * 8192)) <= 1, "22edo range 2: %d %d", t.note[61], scala::bendFor(t, 61, 0));

        // In Bend in scale steps: 22edo, 2 steps, range 24
        scala::buildTable(s, scala::linearMapping(60, 0), 24, 24, scala::BendCurve::Symmetric, 2, t);
        auto wheelPitch = [&](int key, float w) { int b = scala::bendFor(t, key, w); return t.note[key] + b / 8192.0 * 24; };
        CHECK(std::fabs(wheelPitch(60, 1.0f) - (60 + 2 * 12.0 / 22)) < 0.004, "wheel up = key+2 steps: %f", wheelPitch(60, 1.0f));
        CHECK(std::fabs(wheelPitch(60, -1.0f) - (60 - 2 * 12.0 / 22)) < 0.004, "wheel down = key-2 steps");
        CHECK(std::fabs(wheelPitch(61, 0.5f) - (60 + 2 * 12.0 / 22)) < 0.004, "half wheel = 1 step");
        scala::Scale j7; std::string jt;
        mpc::readTextFile(mpc::contentPath("Scales/ji_7.scl"), jt); scala::parseScl(jt, j7);
        scala::buildTable(j7, scala::linearMapping(60, 0), 24, 24, scala::BendCurve::Symmetric, 1, t);
        CHECK(std::fabs(wheelPitch(60, 1.0f) - (60 + 1200 * std::log2(10.0 / 9) / 100)) < 0.004, "ji_7 wheel up = 10/9");
        scala::buildTable(j7, scala::linearMapping(60, 0), 24, 24, scala::BendCurve::Symmetric, 0, t);
        CHECK(scala::bendFor(t, 60, 1.0f) == 0, "In Bend 0 ignores the wheel");

        // Root tune and a .kbm
        scala::buildTable(s, scala::linearMapping(57, 50), 24, 24, scala::BendCurve::Symmetric, 2, t);
        CHECK(std::fabs(played(t, 57, false) - 57.5) < 0.004, "root tune");
        scala::Scale s31; scala::Mapping k31;
        CHECK(mpc::readTextFile(mpc::contentPath("Scales/31-edo.scl"), text) && scala::parseScl(text, s31), "31 parse");
        CHECK(mpc::readTextFile(mpc::contentPath("Scales/31edo.kbm"), text) && scala::parseKbm(text, k31), "kbm parse");
        scala::buildTable(s31, k31, 24, 24, scala::BendCurve::Symmetric, 2, t);
        CHECK(std::fabs(played(t, 60, false) - 60) < 0.004 && std::fabs(played(t, 91, false) - 72) < 0.004 &&
              std::fabs(played(t, 61, false) - (60 + 12.0 / 31)) < 0.004, "31edo kbm");

        // bad input
        CHECK(!scala::parseScl("! x\nfoo\n3\n100.\n", s), "short scale rejected");
        CHECK(!scala::parseScl("", s), "empty rejected");
    }

    // --- 2. The plugin: lists, routing, rotation, state ---------------------------------
    {
        ScalaRetuner p;
        p.internalSetSampleRate(44100); p.internalSetBlockSize(64); p.internalResume();
        takeLog(p);
        const int i22 = indexOf(p, kScale, "22edo");
        CHECK(i22 >= 0, "22edo listed");
        CHECK(indexOf(p, kMapping, "31edo") == 1, "kbm listed");
        CHECK(p.param(kScale).choices[(size_t)p.getInt(kScale)] == "12-edo", "default 12-edo");

        // 12-edo: identity, bend 0 (sent once per channel since the channel's bend is unknown)
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 64, 100)});
        auto log = takeLog(p);
        CHECK(log.size() == 2 && log[0][0] == 0xE0 && log[0][1] == 0 && log[0][2] == 64 && log[1][0] == 0x90 && log[1][1] == 64,
              "12-edo passthrough: %zu msgs", log.size());

        p.set(kScale, (float)i22); p.set(kVoices, 3); waitWorker();
        sendMidi(p, {mpc::MidiEvent::noteOff(0, 64)});
        takeLog(p);
        // three notes go to three channels; channel 0 was released longest ago, so it is reused last
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 60, 100), mpc::MidiEvent::noteOn(0, 61, 100), mpc::MidiEvent::noteOn(0, 62, 100)});
        log = takeLog(p);
        std::vector<int> chans;
        for (auto& m : log) if ((m[0] & 0xF0) == 0x90) chans.push_back(m[0] & 15);
        CHECK(chans.size() == 3 && chans[0] != chans[1] && chans[1] != chans[2] && chans[0] != chans[2], "3 voices on 3 channels");
        // bend always precedes its note on the same channel
        for (size_t i = 0; i + 1 < log.size(); ++i)
            if ((log[i + 1][0] & 0xF0) == 0x90 && (log[i][0] & 0xF0) == 0xE0) CHECK((log[i][0] & 15) == (log[i + 1][0] & 15), "bend/note channel");
        // 4th note steals the oldest (key 60)
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 63, 100)});
        log = takeLog(p);
        CHECK(!log.empty() && (log[0][0] & 0xF0) == 0x80 && (log[0][0] & 15) == chans[0], "steal oldest");
        sendMidi(p, {mpc::MidiEvent::noteOff(0, 61), mpc::MidiEvent::noteOff(0, 60)});
        log = takeLog(p);
        CHECK(log.size() == 1 && (log[0][0] & 15) == chans[1], "off for 61 only (60 was stolen): %zu", log.size());
        // the next free channel is the one released longest ago = chans[1]
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 70, 100)});
        log = takeLog(p);
        CHECK(!log.empty() && (log.back()[0] & 15) == chans[1], "reuse longest-released channel");
        // all notes off
        sendMidi(p, {mpc::MidiEvent::cc(0, 123, 0)});
        log = takeLog(p);
        { int offs = 0, ccs = 0; for (auto& m : log) { if ((m[0] & 0xF0) == 0x80) ++offs; if ((m[0] & 0xF0) == 0xB0 && m[1] == 123) ++ccs; }
          CHECK(offs == 3 && ccs == 3, "all off: 3 note-offs + CC123 on 3 channels: %d %d", offs, ccs); }

        // --- forwarding: 22edo, Voices 3 (channels 0..2), In Bend 2 ---
        auto chOf = [](const std::array<uint8_t, 3>& m) { return m[0] & 15; };
        auto typeOf = [](const std::array<uint8_t, 3>& m) { return m[0] & 0xF0; };
        sendMidi(p, {mpc::MidiEvent::cc(0, 1, 77)});
        log = takeLog(p);
        CHECK(log.size() == 3 && typeOf(log[0]) == 0xB0 && log[0][1] == 1 && log[0][2] == 77, "CC1 to 3 channels: %zu", log.size());
        mpc::MidiEvent at; at.size = 2; at.data[0] = 0xD0; at.data[1] = 90;
        sendMidi(p, {at});
        log = takeLog(p);
        CHECK(log.size() == 3 && typeOf(log[0]) == 0xD0 && log[0][1] == 90, "aftertouch to 3 channels");
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 61, 100)});
        log = takeLog(p);
        const int c61 = chOf(log.back());
        mpc::MidiEvent pat; pat.size = 3; pat.data[0] = 0xA0; pat.data[1] = 61; pat.data[2] = 50;
        sendMidi(p, {pat});
        log = takeLog(p);
        CHECK(log.size() == 1 && typeOf(log[0]) == 0xA0 && chOf(log[0]) == c61 && log[0][1] == 61, "poly AT to the note's channel");
        // wheel fully up: key 61 must sound like key 63 (22edo)
        mpc::MidiEvent pb; pb.size = 3; pb.data[0] = 0xE0; pb.data[1] = 0x7F; pb.data[2] = 0x7F;
        mpc::MidiEvent pb0 = pb; pb0.data[1] = 0; pb0.data[2] = 0x40;
        sendMidi(p, {pb0, pb});   // a run of bends: only the last counts
        log = takeLog(p);
        bool found = false;
        for (auto& m : log) if (typeOf(m) == 0xE0 && chOf(m) == c61)
        {
            found = true;
            const int v = (m[1] | (m[2] << 7)) - 8192;
            CHECK(std::fabs(61 + v / 8192.0 * 24 - (60 + 3 * 12.0 / 22)) < 0.004, "wheel up on held note: %d", v);
        }
        CHECK(found, "bend sent to the held note's channel");
        sendMidi(p, {pb0, mpc::MidiEvent::noteOff(0, 61), mpc::MidiEvent::cc(0, 123, 0)});
        takeLog(p);

        // sustain: pedal-held channels stay busy
        sendMidi(p, {mpc::MidiEvent::cc(0, 64, 127), mpc::MidiEvent::noteOn(0, 60, 100), mpc::MidiEvent::noteOff(0, 60)});
        log = takeLog(p);
        int cs = -1;
        for (auto& m : log) if (typeOf(m) == 0x90) cs = chOf(m);
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 62, 100), mpc::MidiEvent::noteOn(0, 64, 100)});
        log = takeLog(p);
        for (auto& m : log) if (typeOf(m) == 0x90) CHECK(chOf(m) != cs, "sustained channel not reused while others are free");
        sendMidi(p, {mpc::MidiEvent::cc(0, 64, 0), mpc::MidiEvent::noteOff(0, 62), mpc::MidiEvent::noteOff(0, 64)});
        takeLog(p);
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 66, 100)});
        log = takeLog(p);
        CHECK(!log.empty() && chOf(log.back()) == cs, "after pedal up, the channel released longest ago is reused");
        sendMidi(p, {mpc::MidiEvent::noteOff(0, 66)});
        takeLog(p);

        // --- ROLL page display parameters (22edo, Voices 3, First Ch 1) ---
        CHECK(std::fabs(p.get(kGrid0 + 1) - 1.0f / 22) < 1e-3, "grid line 2 at 1/22: %f", p.get(kGrid0 + 1));
        CHECK(std::fabs(p.get(kGrid0 + 21) - 21.0f / 22) < 1e-3, "grid line 22 at 21/22");
        CHECK(p.get(kGrid0 + 22) == 0.0f, "unused grid line parked at 0");
        CHECK(std::fabs(p.get(kGridX) - 0.5f) < 1e-6, "grid x");
        for (int i = 0; i < kNumTraces; ++i) CHECK(p.get(kTrace0 + i) == 0.0f, "trace %d silent", i);
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 67, 100)});
        log = takeLog(p);
        const int c67 = chOf(log.back());
        const float want = kTraceFloor + (1 - kTraceFloor) * 7.0f / 22;
        CHECK(std::fabs(p.get(kTrace0 + c67) - want) < 1e-3, "trace of key 67 = degree 7: %f vs %f", p.get(kTrace0 + c67), want);
        std::string txt;
        p.formatValue(kLastDeg, p.get(kLastDeg), txt);
        CHECK(txt == "7\\22", "Last Deg text: %s", txt.c_str());
        p.formatValue(kLastCents, p.get(kLastCents), txt);
        CHECK(txt == "381.8c", "Last Cents text: %s", txt.c_str());
        p.formatValue(kLastOct, p.get(kLastOct), txt);
        CHECK(txt == "Oct +0", "Last Oct text: %s", txt.c_str());
        // wheel full up on a held note: the trace moves 2 steps (In Bend 2)
        mpc::MidiEvent up = pb;
        sendMidi(p, {up});
        takeLog(p);
        CHECK(std::fabs(p.get(kTrace0 + c67) - (kTraceFloor + (1 - kTraceFloor) * 9.0f / 22)) < 1e-3, "trace follows the wheel");
        sendMidi(p, {pb0, mpc::MidiEvent::noteOff(0, 67)});
        takeLog(p);
        CHECK(p.get(kTrace0 + c67) == 0.0f, "trace silent after note-off");
        // key 60 - 22 = 38: degree 0, one period down
        sendMidi(p, {mpc::MidiEvent::noteOn(0, 38, 100), mpc::MidiEvent::noteOff(0, 38)});
        takeLog(p);
        p.formatValue(kLastOct, p.get(kLastOct), txt);
        CHECK(txt == "Oct -1", "Last Oct one period down: %s", txt.c_str());

        // --- scrolling roll: a note appears in the newest column, then moves left at each step ---
        {
            const float rp = kRollPark + (1 - kRollPark) * 7.0f / 22;
            sendMidi(p, {mpc::MidiEvent::noteOn(0, 67, 100)});
            log = takeLog(p);
            const int v67 = chOf(log.back()) - (p.getInt(kFirstCh) - 1);
            CHECK(std::fabs(p.get(kCell0 + (kRollCols - 1) * kNumTraces + v67) - rp) < 1e-3, "newest column shows the note");
            sendMidi(p, {mpc::MidiEvent::noteOff(0, 67)});
            std::this_thread::sleep_for(std::chrono::milliseconds(400));   // > 2 steps at 120 bpm (125 ms)
            float older = 0;
            for (int c = 0; c < kRollCols - 1; ++c) older = std::max(older, p.get(kCell0 + c * kNumTraces + v67));
            CHECK(std::fabs(older - rp) < 1e-3, "the note moved into an older column");
            CHECK(p.get(kCell0 + (kRollCols - 1) * kNumTraces + v67) == 0.0f, "newest column empty after note-off");
            CHECK(std::fabs(p.get(kColX0) - 0.5f / kRollCols) < 1e-3, "column 1 x");
            // a note shorter than one step still appears
            sendMidi(p, {mpc::MidiEvent::noteOn(0, 60, 100), mpc::MidiEvent::noteOff(0, 60)});
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            bool seen = false;
            for (int c = 0; c < kRollCols; ++c) for (int v = 0; v < kNumTraces; ++v)
                if (std::fabs(p.get(kCell0 + c * kNumTraces + v) - kRollPark) < 1e-3) seen = true;
            CHECK(seen, "short note captured by the roll");
            takeLog(p);
        }

        // state round trip into a fresh instance whose list may differ
        std::vector<uint8_t> st;
        p.saveExtraState(st);
        ScalaRetuner q;
        q.set(kScale, 0);
        q.loadExtraState(st.data(), st.size());
        waitWorker();
        CHECK(std::memcmp(&q.tableForTest(), &p.tableForTest(), sizeof(scala::RetuneTable)) == 0, "state restores the embedded scale");
    }

    std::printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
