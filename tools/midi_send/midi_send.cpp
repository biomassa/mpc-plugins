// midi_send - a measuring tool for the MPC (not a plugin).
//
// Opens an ALSA sequencer output port "percSeq Probe" (the same path as the plugins' own MIDI
// ports, common/alsa_out.h) and plays one note per interval. A CC goes out gapMs before each
// note. It answers: does a CC that MIDI Learn maps to a pad parameter act on the very next hit?
//
//   midi_send <seconds> <cc> <note> <gapMs> <values> [intervalMs] [channel 1-16]
//     values: comma list of CC values used in turn, e.g. 127,0 (reverse on/off) or 0,64,127
//
// Each hit prints "hit <n> cc <value> note <note>" so the listener can match what they hear.
// Build (from the SDK root):
//   zig c++ -target aarch64-linux-gnu.2.31 -O2 -o midi_send plugins/tools/midi_send/midi_send.cpp -I plugins/common -ldl
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include "alsa_out.h"

static void sleepMs(double ms)
{
    timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, nullptr);
}

int main(int argc, char** argv)
{
    if (argc < 6)
    {
        std::fprintf(stderr, "usage: midi_send <seconds> <cc> <note> <gapMs> <values> [intervalMs] [channel]\n");
        return 2;
    }
    const double seconds = std::atof(argv[1]);
    const int cc = std::atoi(argv[2]), note = std::atoi(argv[3]);
    const double gapMs = std::atof(argv[4]);
    std::vector<int> values;
    for (char* s = std::strtok(argv[5], ","); s; s = std::strtok(nullptr, ",")) values.push_back(std::atoi(s));
    const double intervalMs = argc > 6 ? std::atof(argv[6]) : 1000.0;
    const int ch = (argc > 7 ? std::atoi(argv[7]) : 1) - 1;
    if (values.empty() || ch < 0 || ch > 15) return 2;

    alsa::Output out;
    if (!out.open("percSeq Probe", "percSeq Probe")) { std::fprintf(stderr, "cannot open ALSA port\n"); return 1; }
    std::printf("port open; waiting 20 s for you to select it\n");
    std::fflush(stdout);
    sleepMs(20000);

    const int hits = (int)(seconds * 1000.0 / intervalMs);
    for (int i = 0; i < hits; ++i)
    {
        const int v = values[i % values.size()];
        const uint8_t ccMsg[3] = { (uint8_t)(0xB0 | ch), (uint8_t)cc, (uint8_t)v };
        out.send(ccMsg);
        out.flush();
        if (gapMs > 0) sleepMs(gapMs);
        const uint8_t on[3] = { (uint8_t)(0x90 | ch), (uint8_t)note, 100 };
        out.send(on);
        out.flush();
        std::printf("hit %d cc %d note %d\n", i + 1, v, note);
        std::fflush(stdout);
        sleepMs(200);
        const uint8_t off[3] = { (uint8_t)(0x80 | ch), (uint8_t)note, 0 };
        out.send(off);
        out.flush();
        sleepMs(intervalMs - 200 - gapMs);
    }
    out.close();
    return 0;
}
