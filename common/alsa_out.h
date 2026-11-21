// alsa_out.h - a MIDI output port in the ALSA sequencer, loaded at run time with dlopen.
//
// SPDX-License-Identifier: MIT
//
// The MPC ignores MIDI that a plugin hands back to the host. Opening our own ALSA sequencer
// client makes the MPC list it as a MIDI input device, so other tracks can take their MIDI
// from it (MIDI Input Port). This is NOT part of the official SDK host contract (the same
// approach is used by sd88me/mpc-vst-maze, MIT).
//
// The SDK only allows linking libc/libm/libpthread/libdl/librt/libgcc_s, so libasound.so.2 is
// opened with dlopen. The few structures used are declared here from the stable ALSA ABI
// (alsa/seq_event.h, alsa/seq.h). On macOS and wherever libasound is missing, open() fails
// and every call becomes a no-op.
//
// Call only from a non-realtime thread: these are system calls.

#ifndef SCALA_RETUNER_ALSA_OUT_H
#define SCALA_RETUNER_ALSA_OUT_H

#include <cstdint>
#include <cstring>

// MPC_NO_ALSA: build without the ALSA port (used by tests/bench.cpp on the MPC, so the bench
// cannot reach the MPC's tracks).
#if defined(__linux__) && !defined(MPC_NO_ALSA)
  #define MPC_HAVE_ALSA 1
#else
  #define MPC_HAVE_ALSA 0
#endif

#if MPC_HAVE_ALSA
  #include <dlfcn.h>
#endif

namespace alsa {

// --- ABI subset (alsa-lib, unchanged since 1.0) ---------------------------------------------
struct SeqAddr { uint8_t client, port; };
struct SeqEvent                      // snd_seq_event_t, 28 bytes
{
    uint8_t type, flags, tag, queue;
    uint32_t time[2];                // snd_seq_timestamp_t
    SeqAddr source, dest;
    union
    {
        struct { uint8_t channel, note, velocity, off_velocity; uint32_t duration; } note;
        struct { uint8_t channel, unused[3]; uint32_t param; int32_t value; } control;
        uint8_t raw8[12];
    } data;
};
static_assert(sizeof(SeqEvent) == 28, "snd_seq_event_t layout");

enum : uint8_t { kNoteOn = 6, kNoteOff = 7, kKeyPressure = 8, kController = 10, kChanPressure = 12, kPitchBend = 13 };
enum : uint8_t { kQueueDirect = 253, kAddrSubscribers = 254, kAddrUnknown = 253 };
enum : uint8_t { kTimeStampReal = 1, kTimeModeRel = 2 };   // SND_SEQ_TIME_STAMP_REAL, SND_SEQ_TIME_MODE_REL
enum : int { kQueueStart = 30 };                            // SND_SEQ_EVENT_START
enum : int { kOpenOutput = 1, kNonBlock = 1 };
enum : unsigned { kCapRead = 1u << 0, kCapSubsRead = 1u << 5,
                  kTypeMidiGeneric = 1u << 1, kTypeApplication = 1u << 20 };

class Output
{
public:
    ~Output() { close(); }

    bool open(const char* clientName, const char* portName)
    {
#if MPC_HAVE_ALSA
        lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib_) return false;
        if (!sym(open_, "snd_seq_open") || !sym(close_, "snd_seq_close") ||
            !sym(setName_, "snd_seq_set_client_name") || !sym(makePort_, "snd_seq_create_simple_port") ||
            !sym(output_, "snd_seq_event_output") || !sym(drain_, "snd_seq_drain_output"))
        { unload(); return false; }
        sym(pool_, "snd_seq_set_client_pool_output");   // optional
        sym(allocQueue_, "snd_seq_alloc_queue");          // optional: scheduled output
        sym(controlQueue_, "snd_seq_control_queue");
        if (open_(&seq_, "default", kOpenOutput, kNonBlock) < 0) { seq_ = nullptr; unload(); return false; }
        if (pool_) pool_(seq_, 2048);
        setName_(seq_, clientName);
        port_ = makePort_(seq_, portName, kCapRead | kCapSubsRead, kTypeMidiGeneric | kTypeApplication);
        if (port_ < 0) { close(); return false; }
        return true;
#else
        (void)clientName; (void)portName;
        return false;
#endif
    }

    bool isOpen() const { return seq_ != nullptr; }

    // Scheduled output: allocate and start a queue. After this, sendIn() delivers each message
    // a given time after the moment the sequencer receives it (relative real time). Returns false
    // (direct output stays) if the queue cannot be made.
    bool startQueue()
    {
        if (!seq_ || !allocQueue_ || !controlQueue_) return false;
        const int q = allocQueue_(seq_);
        if (q < 0) return false;
        if (controlQueue_(seq_, q, kQueueStart, 0, nullptr) < 0) return false;
        drain_(seq_);
        queue_ = q;
        return true;
    }
    bool hasQueue() const { return queue_ >= 0; }

    // Queue one message for delivery relNs from now (0 or no queue: at once).
    void sendIn(const uint8_t* m, int64_t relNs)
    {
        SeqEvent ev;
        if (!makeEvent(m, ev)) return;
        if (queue_ >= 0 && relNs > 0)
        {
            ev.queue = (uint8_t)queue_;
            ev.flags = kTimeStampReal | kTimeModeRel;
            ev.time[0] = (uint32_t)(relNs / 1000000000LL);
            ev.time[1] = (uint32_t)(relNs % 1000000000LL);
        }
        output_(seq_, &ev);
    }

    // Queue one MIDI channel message (status byte + up to 2 data bytes). Call flush() after a batch.
    void send(const uint8_t* m)
    {
        SeqEvent ev;
        if (makeEvent(m, ev)) output_(seq_, &ev);   // buffered, non-blocking: drops instead of blocking when full
    }

    void flush() { if (seq_) drain_(seq_); }

    void close()
    {
        if (seq_ && close_) close_(seq_);
        seq_ = nullptr;
        port_ = -1;
        queue_ = -1;
        unload();
    }

private:
    bool makeEvent(const uint8_t* m, SeqEvent& ev) const
    {
        if (!seq_) return false;
        std::memset(&ev, 0, sizeof ev);
        ev.queue = kQueueDirect;
        ev.source.port = (uint8_t)port_;
        ev.dest.client = kAddrSubscribers;
        ev.dest.port = kAddrUnknown;
        const uint8_t type = m[0] & 0xF0, ch = m[0] & 0x0F;
        switch (type)
        {
            case 0x90: ev.type = m[2] ? kNoteOn : kNoteOff; ev.data.note.channel = ch; ev.data.note.note = m[1]; ev.data.note.velocity = m[2]; break;
            case 0x80: ev.type = kNoteOff; ev.data.note.channel = ch; ev.data.note.note = m[1]; break;
            case 0xA0: ev.type = kKeyPressure; ev.data.note.channel = ch; ev.data.note.note = m[1]; ev.data.note.velocity = m[2]; break;
            case 0xD0: ev.type = kChanPressure; ev.data.control.channel = ch; ev.data.control.value = m[1]; break;
            case 0xB0: ev.type = kController; ev.data.control.channel = ch; ev.data.control.param = m[1]; ev.data.control.value = m[2]; break;
            case 0xE0: ev.type = kPitchBend; ev.data.control.channel = ch; ev.data.control.value = (int32_t)(m[1] | (m[2] << 7)) - 8192; break;
            default: return false;
        }
        return true;
    }

    template <typename F> bool sym(F& f, const char* name)
    {
#if MPC_HAVE_ALSA
        f = reinterpret_cast<F>(dlsym(lib_, name));
        return f != nullptr;
#else
        (void)name; f = nullptr; return false;
#endif
    }
    void unload()
    {
#if MPC_HAVE_ALSA
        if (lib_) dlclose(lib_);
#endif
        lib_ = nullptr;
    }

    void* lib_ = nullptr;
    void* seq_ = nullptr;
    int port_ = -1;
    [[maybe_unused]] int (*open_)(void**, const char*, int, int) = nullptr;
    int (*close_)(void*) = nullptr;
    [[maybe_unused]] int (*setName_)(void*, const char*) = nullptr;
    [[maybe_unused]] int (*makePort_)(void*, const char*, unsigned, unsigned) = nullptr;
    int (*output_)(void*, SeqEvent*) = nullptr;
    int (*drain_)(void*) = nullptr;
    [[maybe_unused]] int (*pool_)(void*, size_t) = nullptr;
    int (*allocQueue_)(void*) = nullptr;
    int (*controlQueue_)(void*, int, int, int, SeqEvent*) = nullptr;
    int queue_ = -1;
};

} // namespace alsa

#endif
