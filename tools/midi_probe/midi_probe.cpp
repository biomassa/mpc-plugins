// midi_probe - a measuring tool for the MPC (not a plugin).
//
// Subscribes to an ALSA sequencer port (for example "scalaSeq MIDI Out", client:port) and writes
// one line per arriving event with the CLOCK_MONOTONIC time at which the event was read, in ms.
// That is the same clock as scalaSeq's timing trace, so the delivery delay of each note can be
// measured exactly (arecordmidi stamps with the sequencer's system timer, 4 ms resolution on
// the MPC One G2).
//
//   midi_probe <client> <port> <seconds> <outfile>
//
// libasound is opened with dlopen, as in the plugins. Build (from the SDK root):
//   zig c++ -target aarch64-linux-gnu.2.31 -O2 -o midi_probe plugins/tools/midi_probe/midi_probe.cpp -I plugins/common -ldl
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include "alsa_out.h"   // SeqEvent layout

static int64_t monoNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char** argv)
{
    if (argc < 5) { std::fprintf(stderr, "usage: midi_probe <client> <port> <seconds> <outfile>\n"); return 2; }
    const int srcClient = std::atoi(argv[1]), srcPort = std::atoi(argv[2]);
    const double seconds = std::atof(argv[3]);
    void* lib = dlopen("libasound.so.2", RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "no libasound\n"); return 1; }
    auto open_ = (int (*)(void**, const char*, int, int))dlsym(lib, "snd_seq_open");
    auto name_ = (int (*)(void*, const char*))dlsym(lib, "snd_seq_set_client_name");
    auto port_ = (int (*)(void*, const char*, unsigned, unsigned))dlsym(lib, "snd_seq_create_simple_port");
    auto conn_ = (int (*)(void*, int, int, int))dlsym(lib, "snd_seq_connect_from");
    auto input_ = (int (*)(void*, alsa::SeqEvent**))dlsym(lib, "snd_seq_event_input");
    if (!open_ || !name_ || !port_ || !conn_ || !input_) { std::fprintf(stderr, "missing symbols\n"); return 1; }
    void* seq = nullptr;
    if (open_(&seq, "default", 2 /* SND_SEQ_OPEN_INPUT */, 0) < 0) { std::fprintf(stderr, "open failed\n"); return 1; }
    name_(seq, "midi_probe");
    const int p = port_(seq, "in", (1u << 1) | (1u << 6) /* WRITE | SUBS_WRITE */, (1u << 1) | (1u << 20));
    if (p < 0 || conn_(seq, p, srcClient, srcPort) < 0) { std::fprintf(stderr, "subscribe failed\n"); return 1; }
    FILE* f = std::fopen(argv[4], "w");
    if (!f) return 1;
    const int64_t end = monoNs() + (int64_t)(seconds * 1e9);
    while (monoNs() < end)
    {
        alsa::SeqEvent* ev = nullptr;
        if (input_(seq, &ev) < 0 || !ev) continue;   // blocking read
        const double t = monoNs() / 1e6;
        if (ev->type == alsa::kNoteOn || ev->type == alsa::kNoteOff)
            std::fprintf(f, "%.4f %d %d %d %d\n", t, ev->type, ev->data.note.channel, ev->data.note.note, ev->data.note.velocity);
        else if (ev->type == alsa::kPitchBend)
            std::fprintf(f, "%.4f %d %d %d\n", t, ev->type, ev->data.control.channel, ev->data.control.value);
    }
    std::fclose(f);
    return 0;
}
