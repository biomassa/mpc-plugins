// touchProbe: a test plugin (no sound) that measures touch on the MPC screen.
//
// The TOUCH page has one invisible XYPad over a large area. Its two handles use the
// ResetOnGestureEnd modifier: the MPC itself returns both values to their defaults (0, 0)
// when the finger lifts. So the plugin gets a real press, drag and release.
//
//   host sets Touch X / Touch Y  ->  parameterChanged() (host thread): time stamp into a ring
//   worker thread (2 ms)         ->  press / drag / release state, counters, the bar, the log
//
// Display: a dot at the finger, and a bar from the press point to the finger, drawn from
// 11 pieces of 1, 2, 4 ... 1024 px (exact length in pixels). The log /tmp/touchprobe.log (RAM)
// has each raw value with a time stamp, for measurement over SSH.
#include "mpc_plugin.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

namespace {

enum ParamIndex
{
    kVolume = 0,                 // parameter 0: harmless (the host nudges it on load)
    kTouchX, kTouchY,            // the touch surface (ResetOnGestureEnd, default 0)
    kState, kPresses, kReleases, kMoves, kRate, kLastX, kLastY,
    kDotX, kDotY,                // the dot at the finger
    kPiece0,                     // 11 bar pieces: x of each
    // 1.1: a second touch area (multi-touch test)
    kTouchX2 = kPiece0 + 11, kTouchY2, kState2, kPresses2, kDot2X, kDot2Y,
    kPieceY0,                    // y of each bar piece: 0.5 shown, 0 parked in the masked strip below
    kNumParams = kPieceY0 + 11
};

// geometry of the touch area (must match ui/make_skin.py)
constexpr float kW = 600;       // px (area 1)

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

class TouchProbe : public mpc::Plugin
{
public:
    TouchProbe() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("volume",   "Volume",   -40, 6, -6, "dB", 1));
        addParam(mpc::floatParam("touchx",   "Touch X",  0, 1, 0, "", 4));
        addParam(mpc::floatParam("touchy",   "Touch Y",  0, 1, 0, "", 4));
        addParam(mpc::choiceParam("state",   "State",    {"up", "DOWN"}, 0));
        addParam(mpc::intParam   ("presses", "Presses",  0, 100000, 0));
        addParam(mpc::intParam   ("releases","Releases", 0, 100000, 0));
        addParam(mpc::intParam   ("moves",   "Moves",    0, 1000000, 0));
        addParam(mpc::floatParam ("rate",    "Events/s", 0, 1000, 0, "/s", 0));
        addParam(mpc::floatParam ("lastx",   "Last X",   0, 1, 0, "", 4));
        addParam(mpc::floatParam ("lasty",   "Last Y",   0, 1, 0, "", 4));
        addParam(mpc::floatParam ("dotx",    "Dot X",    0, 1, 1, "", 4));
        addParam(mpc::floatParam ("doty",    "Dot Y",    0, 1, 0, "", 4));
        for (int k = 0; k < 11; ++k)
            addParam(mpc::floatParam(("piece" + std::to_string(k)).c_str(), ("Piece " + std::to_string(k)).c_str(), 0, 1, 1, "", 4));
        addParam(mpc::floatParam("touchx2",  "Touch X 2", 0, 1, 0, "", 4));
        addParam(mpc::floatParam("touchy2",  "Touch Y 2", 0, 1, 0, "", 4));
        addParam(mpc::choiceParam("state2",  "State 2",   {"up", "DOWN"}, 0));
        addParam(mpc::intParam   ("presses2","Presses 2", 0, 100000, 0));
        addParam(mpc::floatParam ("dot2x",   "Dot 2 X",   0, 1, 1, "", 4));
        addParam(mpc::floatParam ("dot2y",   "Dot 2 Y",   0, 1, 0, "", 4));
        for (int k = 0; k < 11; ++k)
            addParam(mpc::floatParam(("piecey" + std::to_string(k)).c_str(), ("Piece Y " + std::to_string(k)).c_str(), 0, 1, 0, "", 4));
        if (numParams() != kNumParams) std::abort();
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~TouchProbe() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        for (int c = 0; c < ctx.numOutputs; ++c)
            for (int i = 0; i < ctx.numFrames; ++i) ctx.outputs[c][i] = 0.0f;
    }

    // host thread: record the raw touch values with a time stamp (no work here)
    void parameterChanged(int index, float plain) override
    {
        if (index != kTouchX && index != kTouchY && index != kTouchX2 && index != kTouchY2) return;
        const uint32_t w = ringW_.load(std::memory_order_relaxed);
        ring_[w & (kRing - 1)] = {nowMs(), index, plain};
        ringW_.store(w + 1, std::memory_order_release);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        if (index == kRate) { std::snprintf(b, sizeof b, "%.0f/s", plain); out = b; return true; }
        return false;
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "touchProbe";
        info.vendor = "blackdingus";
        info.uniqueId = MPC_FOURCC('B', 'o', 'I', 'h');
        info.version = 1100;
        info.isInstrument = true;
        info.numInputs = 0;
        info.numOutputs = 2;
        info.tailSamples = 0;
        return info;
    }

    struct Ev { double t; int idx; float v; };
    static constexpr uint32_t kRing = 4096;
    Ev ring_[kRing];
    std::atomic<uint32_t> ringW_{0};
    uint32_t ringR_ = 0;
    std::atomic<bool> quit_{false};
    std::thread worker_;

    void show(int i, float plain)
    {
        const float n = param(i).toNormalised(plain);
        if (getNormalised(i) == n) return;
        setNormalised(i, n);
        notifyHostParameterChanged(i);
    }

    // bar from pixel a to pixel b (a <= b) in the touch area, from binary pieces
    void showBar(float a, float b)
    {
        int len = (int)std::lround(b - a);
        float x = a;
        for (int k = 10; k >= 0; --k)
        {
            const int w = 1 << k;
            if (len >= w)
            {
                show(kPiece0 + k, std::max(0.0f, std::min(1.0f, x / kW)));   // pad width kW + w: left edge = v * kW
                show(kPieceY0 + k, 0.5f);
                x += w; len -= w;
            }
            else show(kPieceY0 + k, 0.0f);   // parked in the strip below the area (under the mask)
        }
    }

    void workerLoop()
    {
        FILE* log = std::fopen("/tmp/touchprobe.log", "w");
        float x = 0, y = 0, lx = 0, ly = 0, startX = 0;
        float x2 = 0, y2 = 0, l2x = 0, l2y = 0;
        bool down2 = false;
        int presses2 = 0;
        bool down = false;
        int presses = 0, releases = 0, moves = 0;
        double rateT0 = nowMs(), lastNotify = 0;
        int rateN = 0;
        bool dirty = true;
        while (!quit_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const uint32_t w = ringW_.load(std::memory_order_acquire);
            for (; ringR_ != w; ++ringR_)
            {
                const Ev& e = ring_[ringR_ & (kRing - 1)];
                if (e.idx == kTouchX2 || e.idx == kTouchY2)
                {
                    if (e.idx == kTouchX2) x2 = e.v; else y2 = e.v;
                    if (log) std::fprintf(log, "%.3f %s %.5f\n", e.t, e.idx == kTouchX2 ? "x2" : "y2", e.v);
                    const bool rest2 = (x2 == 0.0f && y2 == 0.0f);
                    if (!down2 && !rest2) { down2 = true; ++presses2; if (log) std::fprintf(log, "%.3f DOWN2 (area 1 %s)\n", e.t, down ? "down" : "up"); }
                    else if (down2 && rest2) { down2 = false; if (log) std::fprintf(log, "%.3f UP2\n", e.t); }
                    if (x2 != 0.0f && y2 != 0.0f) { l2x = x2; l2y = y2; }
                    dirty = true;
                    continue;
                }
                if (e.idx == kTouchX) x = e.v; else y = e.v;
                ++rateN;
                if (log) std::fprintf(log, "%.3f %c %.5f\n", e.t, e.idx == kTouchX ? 'x' : 'y', e.v);
                const bool atRest = (x == 0.0f && y == 0.0f);
                if (!down && !atRest)
                {
                    down = true; ++presses; startX = x;
                    if (log && down2) std::fprintf(log, "%.3f (area 2 is down: two fingers)\n", e.t);
                    if (x == 0.0f || y == 0.0f) startX = -1;   // the second axis has not arrived yet
                    if (log) std::fprintf(log, "%.3f DOWN %.5f %.5f\n", e.t, x, y);
                }
                else if (down && atRest)
                {
                    down = false; ++releases;
                    if (log) std::fprintf(log, "%.3f UP\n", e.t);
                }
                else if (down && x != 0.0f && y != 0.0f) ++moves;   // one axis at 0 = the lift has begun
                if (x != 0.0f && y != 0.0f) { lx = x; ly = y; }
                dirty = true;
            }
            const double t = nowMs();
            if (t - rateT0 >= 500) { show(kRate, rateN * 1000.0f / (float)(t - rateT0)); rateN = 0; rateT0 = t; }
            // display at most every 16 ms
            if (dirty && t - lastNotify >= 16)
            {
                dirty = false; lastNotify = t;
                show(kState, down ? 1.0f : 0.0f);
                show(kPresses, (float)presses);
                show(kReleases, (float)releases);
                show(kMoves, (float)moves);
                if (down && lx > 0)
                {
                    if (startX < 0) startX = lx;
                    show(kLastX, lx); show(kLastY, ly);
                    show(kDotX, lx); show(kDotY, ly);
                    const float a = std::min(startX, lx) * kW, b = std::max(startX, lx) * kW;
                    showBar(a, b);
                }
                if (!down) lx = ly = 0;
                show(kState2, down2 ? 1.0f : 0.0f);
                show(kPresses2, (float)presses2);
                if (down2 && l2x > 0) { show(kDot2X, l2x); show(kDot2Y, l2y); }
                if (log) std::fflush(log);
            }
        }
        if (log) std::fclose(log);
    }
};

MPC_PLUGIN_ENTRY(TouchProbe)
