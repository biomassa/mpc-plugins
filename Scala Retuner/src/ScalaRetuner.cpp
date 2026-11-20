// Scala Retuner - an MPC instrument plugin by blackdingus
//
// What it does
//   A silent instrument that retunes the notes played on its track to a Scala (.scl) scale,
//   optionally with a keyboard mapping (.kbm), and sends them out of its own ALSA MIDI port
//   ("Scala Retuner") as nearest 12-TET note + pitch bend. Other tracks (keygroups, OPx-4)
//   set MIDI Input Port = Scala Retuner and a MIDI channel, and play the retuned notes.
//
// Signal flow
//   MIDI in (this track) -> per-key lookup in a 128-entry RetuneTable (note + bend)
//     -> channel allocator (one note per channel, rotating over First Ch .. First Ch+Voices-1)
//     -> lock-free queue -> worker thread -> ALSA sequencer port -> target tracks
//   The audio outputs stay silent.
//
// CPU (the MPC is a slow ARM device)
//   * process() does no per-sample work at all: outputs are already zeroed by the framework.
//     Per note it does one table lookup and pushes 2 small messages into a ring buffer.
//   * All maths (log2/pow, Scala parsing, file reading) runs on the worker thread when a
//     parameter changes, into one of three tables; the audio thread just switches pointer.
//   * The worker sleeps on a semaphore and wakes only when there are events or changes:
//     zero CPU while idle. ALSA system calls never run on the audio thread.
//
// Parameters (ids are permanent; append new ones at the end)
//   0 Velocity  1..200 %  scales outgoing velocity (parameter 0 is nudged by the host: harmless)
//   1 Scale     .scl files in Content/Scales
//   2 Mapping   Linear, or .kbm files in Content/Scales
//   3 Root      key where scale degree 0 sits (Linear mapping only)
//   4 Root Tune cents offset of the root from its 12-TET pitch (Linear mapping only)
//   5 Bend Up   semitones; must equal the target's pitch-bend range up (MPC Global Pitch Bend)
//   6 Bend Down semitones; must equal the target's range down
//   7 Curve     Symmetric (keygroups) / OPx-4 (up scaled by 8064, as measured on MPC 3.10.1)
//   8 First Ch  first output MIDI channel
//   9 Voices    number of output channels used in rotation (= polyphony)
//  10 In Bend   your bend wheel's range in scale steps: fully up = the pitch of the key that many
//               steps above; 0 = ignore the wheel
//
// Forwarded to the output channels: pitch bend (added to each note's retuning bend), every CC
// (mod wheel, sustain, expression ...), channel aftertouch; polyphonic aftertouch goes to the
// channel of its note. Sustain keeps a pedal-held note's channel busy until the pedal is up.

#include "mpc_plugin.h"
#include "scala.h"
#include "alsa_out.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <dirent.h>
#if defined(__linux__)
  #include <semaphore.h>
#endif

namespace {

enum ParamIndex
{
    kVelocity = 0,
    kScale,
    kMapping,
    kRoot,
    kRootTune,
    kBendUp,
    kBendDown,
    kCurve,
    kFirstCh,
    kVoices,
    kInBend,      // appended in 1.1: incoming pitch-bend range in scale steps
    kNumParams
};

const char* kBuiltin12 = "! built-in\n12-EDO\n12\n100.\n200.\n300.\n400.\n500.\n600.\n700.\n800.\n900.\n1000.\n1100.\n2/1\n";

struct FileEntry { std::string name, path; };

// .scl / .kbm files in Content/Scales, sorted by name (case-insensitive).
std::vector<FileEntry> listFiles(const char* ext)
{
    std::vector<FileEntry> out;
    const std::string dir = mpc::contentPath("Scales");
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || n.size() <= 4) continue;
            std::string x = n.substr(n.size() - 4);
            for (char& c : x) c = (char)std::tolower((unsigned char)c);
            if (x != ext) continue;
            out.push_back({n.substr(0, n.size() - 4), dir + "/" + n});
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        std::string x = a.name, y = b.name;
        for (char& c : x) c = (char)std::tolower((unsigned char)c);
        for (char& c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    });
    return out;
}

// Wakes the worker. sem_post is safe to call from the audio thread; macOS (host_sim only)
// polls instead.
class Wakeup
{
public:
#if defined(__linux__)
    Wakeup() { sem_init(&sem_, 0, 0); }
    ~Wakeup() { sem_destroy(&sem_); }
    void post() { sem_post(&sem_); }
    void wait() { while (sem_wait(&sem_) != 0) {} }
private:
    sem_t sem_;
#else
    void post() { flag_.store(true); }
    void wait()
    {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait_for(l, std::chrono::milliseconds(2), [this] { return flag_.load(); });
        flag_.store(false);
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> flag_{false};
#endif
};

// Single-producer (audio thread) / single-consumer (worker) queue of 3-byte MIDI messages.
class MsgRing
{
public:
    bool push(uint8_t a, uint8_t b, uint8_t c)
    {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= kSize) return false;   // full: drop
        uint8_t* m = buf_[h & (kSize - 1)];
        m[0] = a; m[1] = b; m[2] = c;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(uint8_t out[3])
    {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        const uint8_t* m = buf_[t & (kSize - 1)];
        out[0] = m[0]; out[1] = m[1]; out[2] = m[2];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
private:
    static constexpr uint32_t kSize = 2048;   // power of two
    uint8_t buf_[kSize][3];
    std::atomic<uint32_t> head_{0}, tail_{0};
};

std::atomic<int> g_instances{0};

} // namespace

class ScalaRetuner : public mpc::Plugin
{
public:
    ScalaRetuner() : mpc::Plugin(makeInfo())
    {
        scales_ = listFiles(".scl");
        maps_ = listFiles(".kbm");

        std::vector<std::string> scaleNames, mapNames{"Linear"};
        int defScale = 0;
        for (size_t i = 0; i < scales_.size(); ++i)
        {
            scaleNames.push_back(scales_[i].name);
            if (scales_[i].name == "12-edo") defScale = (int)i;
        }
        if (scaleNames.empty()) scaleNames.push_back("12-EDO");
        for (const FileEntry& f : maps_) mapNames.push_back(f.name);

        addParam(mpc::floatParam ("velocity", "Velocity",  1.0f, 200.0f, 100.0f, "%", 0));
        addParam(mpc::choiceParam("scale",    "Scale",     scaleNames, defScale));
        addParam(mpc::choiceParam("mapping",  "Mapping",   mapNames, 0));
        addParam(mpc::intParam   ("root",     "Root",      0, 127, 60));
        addParam(mpc::floatParam ("roottune", "Root Tune", -100.0f, 100.0f, 0.0f, "ct", 1));
        addParam(mpc::intParam   ("bendup",   "Bend Up",   1, 24, 24, "st"));
        addParam(mpc::intParam   ("benddown", "Bend Down", 1, 24, 24, "st"));
        addParam(mpc::choiceParam("curve",    "Curve",     {"Symmetric", "OPx-4"}, 0));
        addParam(mpc::intParam   ("firstch",  "First Ch",  1, 16, 1));
        addParam(mpc::intParam   ("voices",   "Voices",    1, 16, 4));
        addParam(mpc::intParam   ("inbend",   "In Bend",   0, 24, 2, "steps"));

        addProgram("Keygroups ch1-4",  {100, (float)defScale, 0, 60, 0, 24, 24, 0, 1, 4, 2});
        addProgram("OPx-4 ch1-4",      {100, (float)defScale, 0, 60, 0, 24, 24, 1, 1, 4, 2});
        addProgram("Keygroups ch1-8",  {100, (float)defScale, 0, 60, 0, 24, 24, 0, 1, 8, 2});
        addProgram("Mono ch1",         {100, (float)defScale, 0, 60, 0, 24, 24, 0, 1, 1, 2});

        for (int& k : keyChan_) k = -1;
        rebuild();   // table ready before the first block
        const int n = g_instances.fetch_add(1);
        if (n == 0) std::snprintf(clientName_, sizeof clientName_, "Scala Retuner");
        else        std::snprintf(clientName_, sizeof clientName_, "Scala Retuner %d", n + 1);
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~ScalaRetuner() override
    {
        quit_.store(true);
        wake_.post();
        if (worker_.joinable()) worker_.join();
    }

    void prepare(double, int) override {}

    // Audio thread, after resume or a state load: release whatever we were holding.
    void reset() override
    {
        for (int c = 0; c < 16; ++c)
        {
            if (chans_[c].key >= 0) ring_.push((uint8_t)(0x80 | c), (uint8_t)chans_[c].note, 0);
            chans_[c] = Chan();
        }
        for (int& k : keyChan_) k = -1;
        wheel_ = 0.0f;
        sustain_ = false;
        wake_.post();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // Outputs are already zeroed: this plugin makes no sound.
        if (ctx.numMidiEvents == 0) return;

        const scala::RetuneTable& t = acquireTable();
        const float velScale = get(kVelocity) * 0.01f;
        const int first = getInt(kFirstCh) - 1;
        const int count = std::min(getInt(kVoices), 16 - first);
        bool sent = false;

        for (int i = 0; i < ctx.numMidiEvents; ++i)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            const int type = e.data[0] & 0xF0;
            if (e.isNoteOn())
            {
                const int key = e.note();
                if (keyChan_[key] >= 0) release(key);
                const int out = t.note[key];
                if (out < 0) continue;
                const int c = pickChannel(first, count);
                if (chans_[c].key >= 0) release(chans_[c].key);   // steal
                chans_[c].sustained = false;
                sendBend(c, scala::bendFor(t, key, wheel_));
                int vel = (int)std::lround(e.velocity() * velScale);
                vel = vel < 1 ? 1 : (vel > 127 ? 127 : vel);
                ring_.push((uint8_t)(0x90 | c), (uint8_t)out, (uint8_t)vel);
                chans_[c].key = key;
                chans_[c].lastKey = key;
                chans_[c].note = out;
                chans_[c].started = ++clock_;
                keyChan_[key] = c;
                sent = true;
            }
            else if (e.isNoteOff())
            {
                if (keyChan_[e.note()] >= 0) { release(e.note()); sent = true; }
            }
            else if (type == 0xE0)
            {
                // only the last bend of a run in this block matters
                if (i + 1 < ctx.numMidiEvents && (ctx.midi[i + 1].data[0] & 0xF0) == 0xE0) continue;
                const int pb = e.pitchBend();
                wheel_ = pb >= 0 ? pb / 8191.0f : pb / 8192.0f;
                // every channel that has sounded, including release tails
                for (int c = 0; c < 16; ++c)
                    if (chans_[c].lastKey >= 0) sendBend(c, scala::bendFor(t, chans_[c].lastKey, wheel_));
                sent = true;
            }
            else if (type == 0xB0)
            {
                const int cc = e.data[1], val = e.data[2];
                if (cc == 120 || cc == 123)
                {
                    for (int k = 0; k < 128; ++k) if (keyChan_[k] >= 0) release(k);
                }
                else if (cc == 64)
                {
                    const bool down = val >= 64;
                    if (sustain_ && !down)
                        for (Chan& ch : chans_) if (ch.sustained) { ch.sustained = false; ch.released = ++clock_; }
                    sustain_ = down;
                }
                else if (cc == 121)
                {
                    for (Chan& ch : chans_) ch.bend = 0;   // receivers reset their bend
                }
                for (int c = first; c < first + count; ++c) ring_.push((uint8_t)(0xB0 | c), (uint8_t)cc, (uint8_t)val);
                sent = true;
            }
            else if (type == 0xD0)
            {
                for (int c = first; c < first + count; ++c) ring_.push((uint8_t)(0xD0 | c), e.data[1], 0);
                sent = true;
            }
            else if (type == 0xA0)
            {
                const int c = keyChan_[e.data[1]];
                if (c >= 0) { ring_.push((uint8_t)(0xA0 | c), (uint8_t)chans_[c].note, e.data[2]); sent = true; }
            }
        }
        if (sent) wake_.post();
    }

    void parameterChanged(int index, float) override
    {
        if ((index >= kScale && index <= kCurve) || index == kInBend)
        {
            dirty_.store(true);
            wake_.post();
        }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index != kRoot) return false;
        static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int n = (int)std::lround(plain);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%s%d (%d)", names[n % 12], n / 12 - 1, n);
        out = buf;
        return true;
    }

    // The scale and mapping travel with the project, so it still sounds right if the files
    // are renamed or removed. Format: "SRT1", then 4 length-prefixed strings.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::lock_guard<std::mutex> l(srcMutex_);
        out.insert(out.end(), {'S', 'R', 'T', '1'});
        for (const std::string* s : {&scaleName_, &scaleText_, &mapName_, &mapText_})
        {
            const uint32_t n = (uint32_t)s->size();
            for (int b = 0; b < 4; ++b) out.push_back((uint8_t)(n >> (8 * b)));
            out.insert(out.end(), s->begin(), s->end());
        }
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        if (size < 4 || std::memcmp(data, "SRT1", 4) != 0) return;
        std::string parts[4];
        size_t pos = 4;
        for (std::string& p : parts)
        {
            if (pos + 4 > size) return;
            uint32_t n = 0;
            for (int b = 0; b < 4; ++b) n |= (uint32_t)data[pos + (size_t)b] << (8 * b);
            pos += 4;
            if (pos + n > size) return;
            p.assign((const char*)data + pos, n);
            pos += n;
        }
        scala::Scale s;
        if (!scala::parseScl(parts[1], s)) return;
        std::lock_guard<std::mutex> l(srcMutex_);
        scaleName_ = parts[0]; scaleText_ = parts[1];
        mapName_ = parts[2];   mapText_ = parts[3];
        appliedScale_ = getInt(kScale);     // keep the embedded files, not what the index points to now
        appliedMap_ = getInt(kMapping);
        dirty_.store(true);
        wake_.post();
    }

private:
    struct Chan
    {
        int key = -1;            // input key held on this channel, -1 = free
        int lastKey = -1;        // last input key played here (its tail still follows the wheel)
        bool sustained = false;  // released while the pedal is down: still sounding, keep busy
        int note = 0;            // 12-TET note sent
        int bend = 0x7FFFFFFF;   // last bend sent (unknown)
        uint32_t started = 0, released = 0;
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Scala Retuner";
        info.vendor       = "blackdingus";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);
        info.version      = 1100;
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 1;               // silent
        return info;
    }
    static constexpr char kUid[5] = "Bi2r";

    // ----- audio thread ----------------------------------------------------------------------
    const scala::RetuneTable& acquireTable()
    {
        int idx;
        do { idx = published_.load(); inUse_.store(idx); } while (published_.load() != idx);
        return tables_[idx];
    }

    // Free channel released longest ago; if all are busy, the one started longest ago.
    int pickChannel(int first, int count)
    {
        int best = -1, oldest = first;
        for (int c = first; c < first + count; ++c)
        {
            if (chans_[c].key < 0 && !chans_[c].sustained) { if (best < 0 || chans_[c].released < chans_[best].released) best = c; }
            else if (chans_[c].started < chans_[oldest].started) oldest = c;
        }
        return best >= 0 ? best : oldest;
    }

    void sendBend(int c, int bend)
    {
        if (bend == chans_[c].bend) return;
        const int v = bend + 8192;
        ring_.push((uint8_t)(0xE0 | c), (uint8_t)(v & 0x7F), (uint8_t)(v >> 7));
        chans_[c].bend = bend;
    }

    void release(int key)
    {
        const int c = keyChan_[key];
        keyChan_[key] = -1;
        if (c < 0 || chans_[c].key != key) return;
        ring_.push((uint8_t)(0x80 | c), (uint8_t)chans_[c].note, 0);
        chans_[c].key = -1;
        if (sustain_) chans_[c].sustained = true;
        else chans_[c].released = ++clock_;
    }

    // ----- worker thread ---------------------------------------------------------------------
    void workerLoop()
    {
        alsa_.open(clientName_, "MIDI Out");
        while (!quit_.load())
        {
            wake_.wait();
            uint8_t m[3];
            int n = 0;
            while (ring_.pop(m)) { deliver(m); ++n; }
            if (n) alsa_.flush();
            if (dirty_.exchange(false)) rebuild();
        }
        uint8_t m[3];
        while (ring_.pop(m)) deliver(m);
        for (int c = 0; c < 16; ++c) { const uint8_t off[3] = {(uint8_t)(0xB0 | c), 123, 0}; deliver(off); }
        alsa_.flush();
        alsa_.close();
    }

    void deliver(const uint8_t* m)
    {
        alsa_.send(m);
#ifdef SCALA_RETUNER_TEST
        std::lock_guard<std::mutex> l(testMutex_);
        testLog_.push_back({m[0], m[1], m[2]});
#endif
    }

    // Reads files if the Scale/Mapping choice moved, parses, and publishes a new table.
    void rebuild()
    {
        scala::Scale scale;
        scala::Mapping map;
        bool useKbm = false;
        {
            std::lock_guard<std::mutex> l(srcMutex_);
            const int si = getInt(kScale), mi = getInt(kMapping);
            if (si != appliedScale_)
            {
                appliedScale_ = si;
                std::string text;
                if (si < (int)scales_.size() && mpc::readTextFile(scales_[(size_t)si].path, text))
                { scaleName_ = scales_[(size_t)si].name; scaleText_ = text; }
                else if (scales_.empty())
                { scaleName_ = "12-EDO"; scaleText_ = kBuiltin12; }
            }
            if (mi != appliedMap_)
            {
                appliedMap_ = mi;
                std::string text;
                if (mi == 0) { mapName_.clear(); mapText_.clear(); }
                else if (mi - 1 < (int)maps_.size() && mpc::readTextFile(maps_[(size_t)mi - 1].path, text))
                { mapName_ = maps_[(size_t)mi - 1].name; mapText_ = text; }
            }
            if (!scala::parseScl(scaleText_, scale)) scala::parseScl(kBuiltin12, scale);
            useKbm = !mapText_.empty() && scala::parseKbm(mapText_, map);
        }
        if (!useKbm) map = scala::linearMapping(getInt(kRoot), get(kRootTune));
        const auto curve = getInt(kCurve) == 1 ? scala::BendCurve::Opx4 : scala::BendCurve::Symmetric;

        // Write into a table the audio thread is not reading.
        const int pub = published_.load(), use = inUse_.load();
        int slot = 0;
        while (slot == pub || slot == use) ++slot;
        scala::buildTable(scale, map, getInt(kBendUp), getInt(kBendDown), curve, getInt(kInBend), tables_[slot]);
        published_.store(slot);
    }

    // file lists (constant after construction)
    std::vector<FileEntry> scales_, maps_;

    // tuning sources, shared by the worker and the UI thread (state save/load)
    std::mutex srcMutex_;
    std::string scaleName_, scaleText_, mapName_, mapText_;
    int appliedScale_ = -1, appliedMap_ = -1;

    // tables: written by the worker, read by the audio thread
    scala::RetuneTable tables_[3] = {};
    std::atomic<int> published_{0}, inUse_{0};
    std::atomic<bool> dirty_{false}, quit_{false};

    // audio-thread state
    Chan chans_[16];
    int keyChan_[128] = {};
    uint32_t clock_ = 0;
    float wheel_ = 0.0f;     // incoming bend wheel, -1..1
    bool sustain_ = false;

    MsgRing ring_;
    Wakeup wake_;
    alsa::Output alsa_;
    char clientName_[48] = {};
    std::thread worker_;

#ifdef SCALA_RETUNER_TEST
public:
    // Unit tests only: everything the worker delivered, and the current table.
    std::mutex testMutex_;
    std::vector<std::array<uint8_t, 3>> testLog_;
    const scala::RetuneTable& tableForTest() { return tables_[published_.load()]; }
#endif
};

MPC_PLUGIN_ENTRY(ScalaRetuner)
