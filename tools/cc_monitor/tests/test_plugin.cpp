// Tests for CC Monitor: the fields show the last CC, the count and the last note.
#define CCMON_TEST 1
#include "../src/CCMonitor.cpp"

#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void block(CCMonitor& p, std::vector<mpc::MidiEvent> ev)
{
    static float l[64], r[64];
    float* outs[2] = {l, r};
    mpc::ProcessContext ctx;
    ctx.outputs = outs; ctx.numOutputs = 2; ctx.numFrames = 64; ctx.sampleRate = 44100;
    ctx.midi = ev.data(); ctx.numMidiEvents = (int)ev.size();
    p.process(ctx);
}

int main()
{
    CCMonitor p;
    CHECK(p.text(kCcNum) == "-" && p.text(kCount) == "0" && p.text(kNote) == "-", "empty fields");
    block(p, {mpc::MidiEvent::cc(0, 74, 64, 3), mpc::MidiEvent::cc(9, 10, 0, 9)});
    CHECK(p.text(kCcNum) == "10", "cc num %s", p.text(kCcNum).c_str());
    CHECK(p.text(kCcVal) == "0", "cc val %s", p.text(kCcVal).c_str());
    CHECK(p.text(kCcChan) == "10", "cc chan %s", p.text(kCcChan).c_str());
    CHECK(p.text(kCount) == "2", "count %s", p.text(kCount).c_str());
    block(p, {mpc::MidiEvent::noteOn(1, 36, 100), mpc::MidiEvent::noteOn(1, 40, 0)});   // vel 0 = note off
    CHECK(p.text(kNote) == "36  ch 2", "note %s", p.text(kNote).c_str());
    CHECK(p.text(kCount) == "2", "notes do not count %s", p.text(kCount).c_str());
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
