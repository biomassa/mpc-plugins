// CPU benchmark for scalaSeq on the MPC's own CPU (not a unit test).
// Build (from the scalaSeq folder):
//   zig c++ -target aarch64-linux-gnu.2.31 -mcpu=cortex_a55 -std=c++17 -O2 -DMPC_NO_ALSA \
//       -I../../framework -o build/ssq_bench tests/bench.cpp -lpthread
// Run: ./ssq_bench   It plays a 16-note sequence (chance ops on half the notes) in real time,
// 128-frame blocks at 44.1 kHz, 10 s playing then 10 s stopped, and prints the time spent in
// process() and the CPU used by the plugin's other threads.
#define SCALASEQ_TEST 1
#include "../src/ScalaSeq.cpp"
#include <cstdio>
#include <ctime>

static double cpuNow(clockid_t c) { timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void blk(ScalaSeq& p, std::vector<mpc::MidiEvent> ev, double ppq, int frames, double sr)
{
    static float l[512], r[512];
    float* outs[2] = {l, r};
    mpc::ProcessContext ctx;
    ctx.outputs = outs; ctx.numOutputs = 2; ctx.numFrames = frames; ctx.sampleRate = sr;
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
    const double sr = 44100; const int N = 128;
    ScalaSeq p;
    p.internalSetSampleRate(sr); p.internalSetBlockSize(N); p.internalResume();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    blk(p, {}, -1, N, sr);
    // 16 notes by step entry (Entry Len 1 step), pitches walking over the pads
    p.set(kPadNote, 37); p.set(kStepEntry, 1); blk(p, {}, -1, N, sr);   // 2026-11-25: 37 (45-91 are sequence notes)
    for (int i = 0; i < 16; ++i)
    {
        const int k = 37 + (i * 5) % 12;
        blk(p, {mpc::MidiEvent::noteOn(0, k, 100), mpc::MidiEvent::noteOff(0, k)}, -1, N, sr);
    }
    p.set(kStepEntry, 0); blk(p, {}, -1, N, sr);
    // chance ops on every other note: lane 1 P? 50 %, lane 2 RATCH 50 %
    for (int i = 1; i <= 16; i += 2)
    {
        p.set(kNoteSel, (float)i); blk(p, {}, -1, N, sr);
        p.set(kLane0, (float)choiceOfOp(20)); blk(p, {}, -1, N, sr); p.set(kLane0 + 1, 50); blk(p, {}, -1, N, sr);
        p.set(kLane0 + 4, (float)choiceOfOp(15)); blk(p, {}, -1, N, sr); p.set(kLane0 + 5, 50); blk(p, {}, -1, N, sr);
    }
    std::printf("notes in sequence: %d\n", p.testModel().seqs[0].count);

    const double blockS = N / sr, qps = 120.0 / 60.0;
    const char* names[3] = {"PLAYING", "STOPPED", "PLAYING + label drag"};
    for (int phase = 0; phase < 3; ++phase)
    {
        const bool play = phase != 1, drag = phase == 2;
        int dragBlocks = 0; double dragSum = 0, dragMax = 0;
        const int blocks = (int)(10.0 / blockS);
        double sum = 0, mx = 0, ppq = 0;
        long notes = 0;
        const double c0 = cpuNow(CLOCK_PROCESS_CPUTIME_ID), t0 = cpuNow(CLOCK_THREAD_CPUTIME_ID);
        auto next = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            next += std::chrono::nanoseconds((long long)(blockS * 1e9));
            std::this_thread::sleep_until(next);
            // label drag: a finger update every 6 blocks (17 ms, the rate measured on the MPC), the
            // finger moving up and down over 8 rows
            const bool upd = drag && b % 6 == 0;
            if (upd)
            {
                const float y = 0.5f + 0.25f * std::sin(b * 0.02f);
                if (b == 0) p.set(kLabTX, 0.5f);
                p.set(kLabTX + 1, y);
            }
            const auto a = std::chrono::steady_clock::now();
            blk(p, {}, play ? ppq : -1, N, sr);
            const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
            if (upd) { ++dragBlocks; dragSum += d; dragMax = std::max(dragMax, d); }
            sum += d; mx = std::max(mx, d);
            ppq += blockS * qps;
            if ((b & 255) == 0) { std::lock_guard<std::mutex> l(p.testMutex_); for (auto& m : p.testLog_) if ((m[0] & 0xF0) == 0x90) ++notes; p.testLog_.clear(); p.testRel_.clear(); }
        }
        const double audio = cpuNow(CLOCK_THREAD_CPUTIME_ID) - t0, all = cpuNow(CLOCK_PROCESS_CPUTIME_ID) - c0;
        std::printf("%s: process() avg %.1f us (%.2f %% of a %.2f ms block), worst %.1f us; "
                    "audio thread CPU %.2f %%, other plugin threads %.2f %% of one core; note-ons %ld\n",
                    names[phase], sum / blocks * 1e6, sum / blocks / blockS * 100, blockS * 1e3, mx * 1e6,
                    audio / 10.0 * 100, (all - audio) / 10.0 * 100, notes);
        if (drag) std::printf("   blocks with a finger update: %d, avg %.1f us, worst %.1f us (%.1f %% of a block); view low now %d\n",
                              dragBlocks, dragSum / std::max(1, dragBlocks) * 1e6, dragMax * 1e6, dragMax / blockS * 100, p.testViewLow());
    }
    p.set(kLabTX, 0); blk(p, {}, -1, N, sr); p.set(kLabTX + 1, 0); blk(p, {}, -1, N, sr);
    p.internalSuspend();
    return 0;
}
