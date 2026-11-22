// CC Monitor - a silent test instrument by blackdingus
//
// Purpose: find out which MIDI messages the MPC delivers to a plugin on a track. Put it on a track
// in place of the plugin under test, with the same MIDI input. It shows on its screen:
//   CC NUM, CC VALUE, CHANNEL  of the last control change it received,
//   COUNT                      how many control changes it received since it was loaded,
//   LAST NOTE                  the last note-on (number and channel), which shows that the routing
//                              works even when no CC arrives.
// It makes no sound.
//
// Signal flow: process() reads the MIDI events of each block and stores the last values in atomics.
// A notifier thread (not the audio thread) wakes every 50 ms and, for each field that changed,
// sends one audioMasterAutomate report so that the MPC reads the new value text (the same method
// percSeq and scalaSeq use for their text fields).
//
// The fields are parameters only so that the skin can show them. Their value text comes from
// formatValue(), not from the parameter value, so a host write (for example the parameter 0 nudge
// at prepare) changes nothing.

#include "mpc_plugin.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

enum ParamIndex
{
    kUnused = 0,   // parameter 0 is nudged by the host at every prepare: not shown, no effect
    kCcNum,        // text: last CC number, "-" before the first CC
    kCcVal,        // text: its value
    kCcChan,       // text: its MIDI channel 1-16
    kCount,        // text: CCs received since load
    kNote,         // text: last note-on "number  ch N"
    kNumParams
};

} // namespace

class CCMonitor : public mpc::Plugin
{
public:
    CCMonitor() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("unused", "Unused",    0, 1, 0, "", 0));
        addParam(mpc::floatParam("ccnum",  "CC Num",    0, 1, 0, "", 0));
        addParam(mpc::floatParam("ccval",  "CC Value",  0, 1, 0, "", 0));
        addParam(mpc::floatParam("ccchan", "Channel",   0, 1, 0, "", 0));
        addParam(mpc::floatParam("count",  "Count",     0, 1, 0, "", 0));
        addParam(mpc::floatParam("note",   "Last Note", 0, 1, 0, "", 0));
        addProgram("Init", {0, 0, 0, 0, 0, 0});
        notifier_ = std::thread([this] { notifyLoop(); });
    }

    ~CCMonitor() override
    {
        run_.store(false);
        if (notifier_.joinable()) notifier_.join();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // outputs are already zeroed: silence
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            const int ch = ev.channel() + 1;
            if (ev.isController())
            {
                ccNum_.store(ev.controller(), std::memory_order_relaxed);
                ccVal_.store(ev.controllerValue(), std::memory_order_relaxed);
                ccChan_.store(ch, std::memory_order_relaxed);
                count_.fetch_add(1, std::memory_order_relaxed);
                changed_.store(true, std::memory_order_release);
            }
            else if (ev.isNoteOn())
            {
                note_.store(ev.note(), std::memory_order_relaxed);
                noteChan_.store(ch, std::memory_order_relaxed);
                changed_.store(true, std::memory_order_release);
            }
        }
    }

    bool formatValue(int index, float, std::string& out) override
    {
        char b[32];
        const int n = ccNum_.load(std::memory_order_relaxed);
        switch (index)
        {
            case kCcNum:  out = n < 0 ? "-" : std::to_string(n); return true;
            case kCcVal:  out = n < 0 ? "-" : std::to_string(ccVal_.load(std::memory_order_relaxed)); return true;
            case kCcChan: out = n < 0 ? "-" : std::to_string(ccChan_.load(std::memory_order_relaxed)); return true;
            case kCount:  out = std::to_string(count_.load(std::memory_order_relaxed)); return true;
            case kNote:
            {
                const int nt = note_.load(std::memory_order_relaxed);
                if (nt < 0) { out = "-"; return true; }
                std::snprintf(b, sizeof b, "%d  ch %d", nt, noteChan_.load(std::memory_order_relaxed));
                out = b;
                return true;
            }
            case kUnused: out = ""; return true;
            default: return false;
        }
    }

#ifdef CCMON_TEST
    std::string text(int i) { std::string s; formatValue(i, 0, s); return s; }
#endif

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "CC Monitor";
        info.vendor       = "blackdingus";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('B', 'x', 'C', 'm');   // must match plugin.json "uid"
        info.version      = 1000;            // 1.0.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    // Not the audio thread. At most 5 reports per 50 ms, only when something changed.
    void notifyLoop()
    {
        while (run_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!changed_.exchange(false, std::memory_order_acquire)) continue;
            for (int i = kCcNum; i <= kNote; ++i) notifyHostParameterChanged(i);
        }
    }

    std::atomic<int> ccNum_{-1}, ccVal_{0}, ccChan_{0}, count_{0}, note_{-1}, noteChan_{0};
    std::atomic<bool> changed_{false}, run_{true};
    std::thread notifier_;
};

MPC_PLUGIN_ENTRY(CCMonitor)
