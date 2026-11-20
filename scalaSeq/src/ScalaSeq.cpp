// scalaSeq - an MPC instrument plugin by blackdingus
//
// A monophonic pitch and gate sequencer for Scala tunings: a port of dingus.pitchGateSeq
// (Max: pgRoll.js editor + gen~ engine). It makes no sound. It plays its notes as MIDI through
// its own ALSA port ("scalaSeq MIDI Out"): the nearest 12-TET note plus a pitch bend, each new
// note on the next of its output channels, so keygroup and OPx-4 tracks play them in tune.
//
// Signal flow
//   MPC song position -> loop phase -> Engine::tick() per sample (gate, velocity, pitch, step)
//     -> MIDI: note on at a gate rise, note off at a fall, pitch bend for glides
//     -> lock-free queue -> worker thread -> ALSA sequencer port -> target tracks
//   Pads / keys on this track: step entry at the cursor (when Step Entry is on) and preview.
//
// Threads
//   audio thread: owns the 32 sequences, applies knob edits and step entry, runs the engine.
//   worker:       loads scale files, sends MIDI to ALSA, writes the display parameters
//                 (grid cells, row labels, playhead, cursor, knob values after a selection).
//   UI thread:    project save / load (a copy made by the audio thread; applied in reset()).
//
// CPU: per sample the engine does comparisons and a few multiplies. Table rebuilds, chance rolls
// and display work happen only on edits, per note, or on the worker.

#include "mpc_plugin.h"
#include "seq_engine.h"
#include "../../common/alsa_out.h"
#include "../../common/rt_util.h"

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace seq;

enum ParamIndex
{
    // SETUP
    kVelocity = 0, kSequence, kSteps, kRangeStart, kRangeEnd, kSwing, kRate, kDirection, kSnap,
    kScale, kRoot, kRootTune, kFirstCh, kVoices, kBendUp, kBendDown, kCurve, kPtnMode,
    // NOTE
    kNote, kStart, kLength, kDegree, kVel, kGlide, kRatch, kFade, kDuty, kCursor, kEntryLen,
    kStepEntry, kDelete,
    // CHANCE: 4 lanes x (op, prob, ptn, amount)
    kLane0,
    // MASK
    kMask0 = kLane0 + 4 * kLanes,
    // VIEW
    kViewLow = kMask0 + 31, kPage,
    // DISPLAY (written by the plugin)
    kRow0, kPlayX = kRow0 + 16, kCursorX, kLineY,
    // 1.2: the notes as XYPad markers, one set for each quarter-step column of the page
    kColX0, kMarkA0 = kColX0 + 64, kMarkB0 = kMarkA0 + 64, kMarkS0 = kMarkB0 + 64, kEndX = kMarkS0 + 64,
    // appended in 1.1: touch surface and pads
    kTouchX, kTouchY, kPadNote, kLastKey,
    // 1.2: rows sized by cents: 17 row lines, 3 period lines, degree labels (31 degrees x 3 copies)
    kRowLine0, kPerLine0 = kRowLine0 + 17, kLab0 = kPerLine0 + 3,
    kNumParamsEnd = kLab0 + 31 * 3,
    kNumParams = kNumParamsEnd
};
constexpr int kFirstEdit = kSequence, kLastEdit = kPage;
constexpr int kRows = 16, kCols = 64;
// Note markers: the XYPad is the grid plus a masked strip below it (skin: PARKY) where unused
// markers wait at y = 0. Row r (0 = bottom) is at y = kMarkPark + (r + 0.5) / 16 * (1 - kMarkPark).
constexpr float kMarkPark = 30.0f / (480.0f + 30.0f);
// cell frames: 0 empty, 1 note start, 2 note continues, 3 selected start, 4 selected continues,
//              5 beyond the last step, 6 empty row outside the scale mask
constexpr float kTouchIdle = 1.0f;   // touch surface rest position (top right): every tap then changes the value
constexpr int kTouchReleaseMs = 150;  // no movement for this long = the finger is up
constexpr int kTouchSettleMs = 15;    // wait for both coordinates of a new touch
constexpr float kParkX = 1.0f;   // display x for a hidden playhead / cursor (under the mask)
constexpr float kGridFrac = 1152.0f / 1192.0f;   // the grid's share of the XYPad width (skin: GW / (GW + PARK))
const float kRateQ[8] = {0.125f, 1.0f / 6, 0.25f, 1.0f / 3, 0.5f, 1.0f, 2.0f, 4.0f};
const int kSnapDiv[6] = {1, 2, 3, 4, 6, 8};
const float kEntryLens[9] = {0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
const char* kBuiltin12 = "! built-in\n12-EDO\n12\n100.\n200.\n300.\n400.\n500.\n600.\n700.\n800.\n900.\n1000.\n1100.\n2/1\n";

struct Model
{
    Sequence seqs[kSeqs];
    bool mask[kMaxMask];
    Model() { for (bool& m : mask) m = true; }
};

std::atomic<int> g_instances{0};

} // namespace

class ScalaSeq : public mpc::Plugin
{
public:
    ScalaSeq() : mpc::Plugin(makeInfo()), model_(new Model), saved_(new Model), staging_(new Model),
                 tables_(new Tables[2])
    {
        scales_ = rt::listFiles(".scl");
        std::vector<std::string> scaleNames;
        int defScale = 0;
        for (size_t i = 0; i < scales_.size(); ++i)
        {
            scaleNames.push_back(scales_[i].name);
            if (scales_[i].name == "12-edo") defScale = (int)i;
        }
        if (scaleNames.empty()) scaleNames.push_back("12-EDO");

        // SETUP
        addParam(mpc::floatParam ("velocity",  "Velocity",   1, 200, 100, "%", 0));
        addParam(mpc::intParam   ("sequence",  "Sequence",   1, kSeqs, 1));
        addParam(mpc::intParam   ("steps",     "Steps",      1, kMaxSteps, 16));
        addParam(mpc::intParam   ("rstart",    "Range Start", 1, kMaxSteps, 1));
        addParam(mpc::intParam   ("rend",      "Range End",  1, kMaxSteps, 16));
        addParam(mpc::floatParam ("swing",     "Swing",      50, 75, 50, "%", 0));
        addParam(mpc::choiceParam("rate",      "Rate",       {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4", "1/2", "1 bar"}, 2));
        addParam(mpc::choiceParam("direction", "Direction",  {"Fwd", "Rev", "Rnd"}, 0));
        addParam(mpc::choiceParam("snap",      "Snap",       {"1", "1/2", "1/3", "1/4", "1/6", "1/8"}, 3));
        addParam(mpc::choiceParam("scale",     "Scale",      scaleNames, defScale));
        addParam(mpc::intParam   ("root",      "Root",       0, 127, 60));
        addParam(mpc::floatParam ("roottune",  "Root Tune",  -100, 100, 0, "ct", 1));
        addParam(mpc::intParam   ("firstch",   "Out Ch",     1, 16, 1));
        addParam(mpc::intParam   ("voices",    "Voices",     1, 16, 4));
        addParam(mpc::intParam   ("bendup",    "Bend Up",    1, 24, 24, "st"));
        addParam(mpc::intParam   ("benddown",  "Bend Down",  1, 24, 24, "st"));
        addParam(mpc::choiceParam("curve",     "Curve",      {"Sym", "OPx-4"}, 0));
        addParam(mpc::choiceParam("ptnmode",   "PTN Count",  {"Loops", "Bars"}, 0));
        // NOTE
        addParam(mpc::intParam   ("note",      "Note",       1, kMaxNotes, 1));
        addParam(mpc::floatParam ("start",     "Start",      0, kMaxSteps, 0, "st", 2));
        addParam(mpc::floatParam ("length",    "Length",     0, kMaxSteps, 1, "st", 2));
        addParam(mpc::intParam   ("degree",    "Degree",     -kCenter, kPitchN - 1 - kCenter, 0));
        addParam(mpc::floatParam ("vel",       "VEL",        0, 100, 100, "%", 0));
        addParam(mpc::floatParam ("glide",     "GLIDE",      0, 1, 0, "st", 2));
        addParam(mpc::intParam   ("ratch",     "RATCH",      1, 8, 1));
        addParam(mpc::intParam   ("fade",      "FADE",       -50, 50, 0));
        addParam(mpc::floatParam ("duty",      "DUTY",       0.05f, 0.95f, 0.5f, "", 2));
        addParam(mpc::floatParam ("cursor",    "Cursor",     0, kMaxSteps, 0, "st", 2));
        addParam(mpc::choiceParam("entrylen",  "Entry Len",  {"1/8", "1/4", "1/2", "3/4", "1", "1.5", "2", "3", "4"}, 4));
        addParam(mpc::boolParam  ("stepentry", "Step Entry", false));
        addParam(mpc::boolParam  ("delete",    "Delete",     false));
        // CHANCE
        std::vector<std::string> opNames, ptnNames{"always"};
        for (int o = 0; o < kNumOps; ++o) opNames.push_back(opInfo(o).name);
        for (int k = 1; k < kNumPtns; ++k) { int a, b; ptnOf(k, a, b); ptnNames.push_back(std::to_string(a) + ":" + std::to_string(b)); }
        for (int l = 0; l < kLanes; ++l)
        {
            const std::string n = std::to_string(l + 1);
            addParam(mpc::choiceParam(("op" + n).c_str(), ("Op " + n).c_str(), opNames, 0));
            addParam(mpc::floatParam(("prob" + n).c_str(), ("Prob " + n).c_str(), 0, 100, 0, "%", 0));
            addParam(mpc::choiceParam(("ptn" + n).c_str(), ("PTN " + n).c_str(), ptnNames, 0));
            addParam(mpc::intParam(("amt" + n).c_str(), ("Amount " + n).c_str(), 0, 100, 0));
        }
        // MASK
        for (int d = 0; d < 31; ++d)
            addParam(mpc::boolParam(("mask" + std::to_string(d)).c_str(), ("Deg " + std::to_string(d)).c_str(), true));
        // VIEW
        addParam(mpc::intParam   ("viewlow",   "View Low",   -kCenter, kPitchN - kRows - kCenter, 0));
        addParam(mpc::intParam   ("page",      "Step Page",  1, 4, 1));
        // DISPLAY
        for (int r = 0; r < kRows; ++r)
            addParam(mpc::intParam(("row" + std::to_string(r + 1)).c_str(), ("Row " + std::to_string(r + 1)).c_str(), 0, kPitchN - 1, 0));
        addParam(mpc::floatParam ("playx",     "Play X",     0, 1, kParkX, "", 3));
        addParam(mpc::floatParam ("cursorx",   "Cursor X",   0, 1, kParkX, "", 3));
        addParam(mpc::floatParam ("liney",     "Line Y",     0, 1, 0.5f, "", 2));
        for (int c = 0; c < kCols; ++c)
            addParam(mpc::floatParam(("colx" + std::to_string(c + 1)).c_str(), ("Col X " + std::to_string(c + 1)).c_str(), 0, 1, (c + 0.5f) / kCols, "", 3));
        const char* setIds[3] = {"note", "start", "sel"};
        const char* setNames[3] = {"N ", "St ", "Sel "};
        for (int k = 0; k < 3; ++k)
            for (int c = 0; c < kCols; ++c)
                addParam(mpc::floatParam((std::string("mk") + setIds[k] + std::to_string(c + 1)).c_str(),
                                         (setNames[k] + std::to_string(c + 1)).c_str(), 0, 1, 0, "", 3));
        addParam(mpc::floatParam ("endx",      "End X",      0, 1, kParkX, "", 3));
        // 1.1: touch surface over the grid (an XYPad; y = 0 is the bottom row) and pads
        addParam(mpc::floatParam ("touchx",    "Touch X",    0, 1, kTouchIdle, "", 3));
        addParam(mpc::floatParam ("touchy",    "Touch Y",    0, 1, kTouchIdle, "", 3));
        addParam(mpc::intParam   ("padnote",   "Pad 1 Note", 0, 127, 37));
        addParam(mpc::intParam   ("lastkey",   "Last Note",  0, 127, 0));
        for (int i = 0; i < 17; ++i)
            addParam(mpc::floatParam(("rowline" + std::to_string(i + 1)).c_str(), ("Row Line " + std::to_string(i + 1)).c_str(), 0, 1, 0, "", 3));
        for (int i = 0; i < 3; ++i)
            addParam(mpc::floatParam(("perline" + std::to_string(i + 1)).c_str(), ("Per Line " + std::to_string(i + 1)).c_str(), 0, 1, 0, "", 3));
        for (int d = 0; d < 31; ++d)
            for (int k = 0; k < 3; ++k)
                addParam(mpc::floatParam(("lab" + std::to_string(d) + "_" + std::to_string(k + 1)).c_str(),
                                         ("Lab " + std::to_string(d) + "." + std::to_string(k + 1)).c_str(), 0, 1, 0, "", 3));

        for (auto& f : notify_) f.store(false);
        for (int i = 0; i < kNumParams; ++i) { lastSeen_[i] = getNormalised(i); computed_[i] = -1; }
        for (int& k : keyCh_) k = -1;

        loadScale(getInt(kScale), nullptr);   // before the first block
        tuning_.sc = &scaleSlots_[scalePub_.load()];
        scaleInUse_ = scalePub_.load();
        applyRoot();
        rebuildAll();
        refreshView();
        const int n = g_instances.fetch_add(1);
        if (n == 0) std::snprintf(clientName_, sizeof clientName_, "scalaSeq");
        else        std::snprintf(clientName_, sizeof clientName_, "scalaSeq %d", n + 1);
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~ScalaSeq() override
    {
        quit_.store(true);
        wake_.post();
        if (worker_.joinable()) worker_.join();
    }

    void prepare(double, int) override {}

    // Audio thread, after resume or a state load.
    void reset() override
    {
        if (stagingReady_.exchange(false))
        {
            *model_ = *staging_;
            for (int d = 0; d < kMaxMask; ++d) tuning_.mask[d] = model_->mask[d];
            edSeq_ = engSeq_ = std::max(0, std::min(kSeqs - 1, getInt(kSequence) - 1));
            sel_ = 0;
            savedAll_ = true;
        }
        allNotesOff();
        engine_.reset();
        engine_.restart();
        // take the knob values as the host restored them
        for (int i = 0; i < kNumParams; ++i) lastSeen_[i] = getNormalised(i);
        edSeq_ = std::max(0, std::min(kSeqs - 1, getInt(kSequence) - 1));
        if (edSeq_ != engSeq_) { engSeq_ = edSeq_; pendSwap_ = false; }
        sel_ = std::max(0, std::min(ed().count - 1, getInt(kNote) - 1));
        viewLow_ = getInt(kViewLow);
        page_ = std::max(0, std::min(3, getInt(kPage) - 1));
        cursor_ = std::max(0.0f, std::min((float)ed().steps, get(kCursor)));
        applyRoot();
        rebuildAll();
        syncLaneOps();
        // after a load the restored display values stay; remember what they stand for
        primeOnly_ = true;
        refreshView();
        primeOnly_ = false;
        viewDirty_ = false;
        wake_.post();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        bool sent = false;
        // scale loaded by the worker
        const int sp = scalePub_.load();
        if (sp != scaleInUse_) { scaleInUse_ = sp; tuning_.sc = &scaleSlots_[sp]; rebuildAll(); viewDirty_ = true; }
        pollEdits();
        if (previewCh_ >= 0 && msNow() >= previewOffMs_) { stopNote(previewCh_); previewCh_ = -1; sent = true; }

        // MIDI in: step entry and preview
        for (int i = 0; i < ctx.numMidiEvents; ++i)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (e.isNoteOn()) { onKey(e.note(), e.velocity()); sent = true; }
            else if (e.isNoteOff()) { offKey(e.note()); sent = true; }
            else if (e.isAllNotesOff()) { for (int k = 0; k < 128; ++k) offKey(k); sent = true; }
        }
        if (modelDirty_) { rebuildCurrent(); modelDirty_ = false; savedDirty_ = true; viewDirty_ = true; }

        // transport -> loop phase
        const mpc::Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
        const double bpm = (tr.tempoValid && tr.bpm > 1) ? tr.bpm : 120.0;
        const double qps = bpm / 60.0 / sr;   // quarter notes per sample
        if (playing && (!wasPlaying_ || std::fabs(tr.ppqPos - expectPpq_) > 0.05)) { engine_.restart(); }
        if (!playing && wasPlaying_) { noteOffActive(); sent = true; }
        wasPlaying_ = playing;
        engine_.dir = getInt(kDirection);
        engine_.ptnBars = getInt(kPtnMode) == 1;
        const double barQ = tr.timeSigDenominator > 0 ? 4.0 * tr.timeSigNumerator / tr.timeSigDenominator : 4.0;
        engine_.barCount = (int)std::floor(tr.ppqPos / barQ);
        const double stepQ = kRateQ[std::max(0, std::min(7, getInt(kRate)))];

        if (playing)
        {
            for (int i = 0; i < ctx.numFrames; ++i)
            {
                const Tables& T = tables_[tIdx_];
                const double loopQ = stepQ * std::max(1, T.steps);
                double ph = std::fmod(tr.ppqPos + i * qps, loopQ) / loopQ;
                if (ph < 0) ph += 1;
                EngineOut o = engine_.tick(ph, true, T, sr);
                if (engine_.wrappedNow && pendSwap_) swapNow();
                if (toMidi(o)) sent = true;
                playStep_ = o.step;
            }
            expectPpq_ = tr.ppqPos + ctx.numFrames * qps;
        }
        else
        {
            playStep_ = -1;
            if (pendSwap_) swapNow();
        }
        sampleCount_ += (uint64_t)std::max(0, ctx.numFrames);
        if (viewDirty_) { refreshView(); viewDirty_ = false; }
        {
            const float px = (playStep_ - page_ * 16) / 16.0f;
            setDisp(kPlayX, (playStep_ >= 0 && px >= 0 && px < 1) ? px * kGridFrac : kParkX);
        }
        if (notifyPending_) sent = true;
        // copy for project saving: the edited sequence only (a full copy after a load)
        if ((savedDirty_ || savedAll_) && savedMutex_.try_lock())
        {
            if (savedAll_) *saved_ = *model_;
            else saved_->seqs[edSeq_] = model_->seqs[edSeq_];
            for (int d = 0; d < kMaxMask; ++d) saved_->mask[d] = tuning_.mask[d];
            savedMutex_.unlock();
            savedDirty_ = savedAll_ = false;
        }
        if (sent) { wake_.post(); notifyPending_ = false; }
    }

    void parameterChanged(int index, float) override
    {
        if (index == kScale) { scaleDirty_.store(true); wake_.post(); }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        const int v = (int)std::lround(plain);
        auto degText = [&](int idx, bool marks) {
            const int n = std::max(1, dispSize_.load());
            const int d = ((idx - kCenter) % n + n) % n;
            const int p = (int)std::floor((double)(idx - kCenter) / n);
            const bool in = d >= kMaxMask || dispMask_[d].load(std::memory_order_relaxed);
            if (d == 0) std::snprintf(b, sizeof b, marks && !in ? "(0 %+d)" : "0 %+d", p);
            else std::snprintf(b, sizeof b, marks && !in ? "(%d)" : "%d", d);
            return std::string(b);
        };
        if (index >= kRow0 && index < kRow0 + kRows) { out = degText(v, true); return true; }
        if (index == kDegree)
        {
            const int n = std::max(1, dispSize_.load());
            const int d = ((v % n) + n) % n, p = (int)std::floor((double)v / n);
            std::snprintf(b, sizeof b, "%d (%+d)", d, p); out = b; return true;
        }
        if (index == kNote)
        {
            const int c = dispCount_.load();
            if (c == 0) out = "none";
            else { std::snprintf(b, sizeof b, "%d / %d", std::min(v, c), c); out = b; }
            return true;
        }
        if (index == kRoot)
        {
            static const char* nm[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            std::snprintf(b, sizeof b, "%s%d (%d)", nm[((v % 12) + 12) % 12], v / 12 - 1, v); out = b; return true;
        }
        if (index >= kLane0 && index < kMask0 && (index - kLane0) % 4 == 3)
        {
            const int op = laneOp_[(index - kLane0) / 4].load(std::memory_order_relaxed);
            const OpInfo& o = opInfo(op);
            switch (o.unit)
            {
                case U_NONE: out = "-"; break;
                case U_GATE: std::snprintf(b, sizeof b, "%s st", gateName(v)); out = b; break;
                case U_DEG: if (v == 0) out = "period"; else { std::snprintf(b, sizeof b, "%d deg", v); out = b; } break;
                case U_X: std::snprintf(b, sizeof b, "x%d", v); out = b; break;
                case U_REP: std::snprintf(b, sizeof b, "+%d", v); out = b; break;
                case U_RND: std::snprintf(b, sizeof b, "+-%d", v); out = b; break;
                case U_TO: std::snprintf(b, sizeof b, "step %d", v); out = b; break;
                default: std::snprintf(b, sizeof b, "%d", v); out = b; break;
            }
            return true;
        }
        return false;
    }

    // Project state: the 32 sequences, the scale mask and the scale file.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(x >> (8 * i))); };
        auto putStr = [&](const std::string& s) { put32((uint32_t)s.size()); out.insert(out.end(), s.begin(), s.end()); };
        out.insert(out.end(), {'S', 'S', 'Q', '1'});
        {
            std::lock_guard<std::mutex> l(scaleMutex_);
            putStr(scaleName_); putStr(scaleText_);
        }
        std::lock_guard<std::mutex> l(savedMutex_);
        for (int d = 0; d < kMaxMask; ++d) out.push_back(saved_->mask[d] ? 1 : 0);
        for (int s = 0; s < kSeqs; ++s)
        {
            const Sequence& q = saved_->seqs[s];
            put32((uint32_t)q.steps); put32((uint32_t)q.rStart); put32((uint32_t)q.rEnd);
            uint32_t sw; std::memcpy(&sw, &q.swing, 4); put32(sw);
            put32((uint32_t)q.count);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(q.notes);
            out.insert(out.end(), p, p + sizeof(Note) * (size_t)q.count);
        }
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        size_t pos = 0;
        auto get32 = [&](uint32_t& x) { if (pos + 4 > size) return false; x = 0; for (int i = 0; i < 4; ++i) x |= (uint32_t)data[pos + i] << (8 * i); pos += 4; return true; };
        auto getStr = [&](std::string& s) { uint32_t n; if (!get32(n) || pos + n > size) return false; s.assign((const char*)data + pos, n); pos += n; return true; };
        if (size < 4 || std::memcmp(data, "SSQ1", 4) != 0) return;
        pos = 4;
        std::string name, text;
        if (!getStr(name) || !getStr(text)) return;
        if (pos + kMaxMask > size) return;
        Model& m = *staging_;
        for (int d = 0; d < kMaxMask; ++d) m.mask[d] = data[pos++] != 0;
        for (int s = 0; s < kSeqs; ++s)
        {
            Sequence& q = m.seqs[s];
            uint32_t st, rs, re, sw, cnt;
            if (!get32(st) || !get32(rs) || !get32(re) || !get32(sw) || !get32(cnt)) return;
            if (cnt > (uint32_t)kMaxNotes || pos + sizeof(Note) * cnt > size) return;
            q.steps = std::max(1, std::min(kMaxSteps, (int)st));
            q.rStart = (int)rs; q.rEnd = (int)re;
            std::memcpy(&q.swing, &sw, 4);
            q.count = (int)cnt;
            std::memcpy(q.notes, data + pos, sizeof(Note) * cnt);
            pos += sizeof(Note) * cnt;
            q.fitToSteps();
        }
        {
            std::lock_guard<std::mutex> l(scaleMutex_);
            embedName_ = name; embedText_ = text;
            appliedScale_ = getInt(kScale);   // keep the embedded scale, not what the index points to now
        }
        embedPending_.store(true);
        stagingReady_.store(true);
        wake_.post();
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "scalaSeq";
        info.vendor = "blackdingus";
        info.uniqueId = MPC_FOURCC('B', 'y', 'Z', 'q');
        info.version = 1300;
        info.isInstrument = true;
        info.numInputs = 0;
        info.numOutputs = 2;
        info.tailSamples = 1;
        return info;
    }

    // ===== audio thread: model and edits ==========================================================
    Sequence& ed() { return model_->seqs[edSeq_]; }
    Note* selNote() { Sequence& s = ed(); return (sel_ >= 0 && sel_ < s.count) ? &s.notes[sel_] : nullptr; }
    float snapQ() const { return 1.0f / kSnapDiv[std::max(0, std::min(5, getInt(kSnap)))]; }
    float snapTo(float x) const { const float q = snapQ(); return std::round(x / q) * q; }

    void applyRoot() { tuning_.rootPitch = getInt(kRoot) + get(kRootTune) / 100.0; }

    void rebuildAll()
    {
        buildTables(model_->seqs[engSeq_], tuning_, tables_[tIdx_]);
        if (pendSwap_) buildTables(model_->seqs[edSeq_], tuning_, tables_[1 - tIdx_]);
        dispSize_.store(tuning_.size());
        for (int d = 0; d < kMaxMask; ++d) dispMask_[d].store(tuning_.mask[d], std::memory_order_relaxed);
    }
    void rebuildCurrent()
    {
        if (edSeq_ == engSeq_) buildTables(ed(), tuning_, tables_[tIdx_]);
        else buildTables(ed(), tuning_, tables_[1 - tIdx_]);
    }
    void swapNow()
    {
        if (!pendSwap_) return;
        tIdx_ = 1 - tIdx_;
        engSeq_ = edSeq_;
        pendSwap_ = false;
        engine_.restart();
    }

    // value changes of the editable parameters (the knobs), compared with what we wrote ourselves
    void pollEdits()
    {
        for (int i = kFirstEdit; i <= kLastEdit; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            onEdit(i, param(i).toPlain(v));
        }
        // touch surface
        bool moved = false;
        for (int i : {kTouchX, kTouchY})
        {
            const float v = getNormalised(i);
            if (v != lastSeen_[i]) { lastSeen_[i] = v; moved = true; }
        }
        lastSeen_[kPadNote] = getNormalised(kPadNote);
        if (moved) onTouchMove();
        touchTick();
    }

    // ===== audio thread: touch on the grid ========================================================
    // Tap an empty cell: a new note (row degree, cell start, Entry Len); the cursor moves there.
    // Tap a note: select it. Tap the selected note again (no drag): delete it.
    // Drag sideways after the touch: the end of the touched note follows the finger.
    // The XYPad gives no release event: no movement for kTouchReleaseMs = released.
    double msNow() const { return sampleCount_ * 1000.0 / std::max(1.0, sampleRate()); }

    void touchCell(int& col, int& row) const
    {
        const float x = getNormalised(kTouchX), y = getNormalised(kTouchY);
        col = std::max(0, std::min(kCols - 1, (int)std::floor(x * kCols)));
        row = rowAtY(y);
    }

    void onTouchMove()
    {
        lastTouchMs_ = msNow();
        if (touchState_ == 0) { touchState_ = 1; touchBeginMs_ = lastTouchMs_; return; }   // settle first
        if (touchState_ != 2) return;
        int col, row;
        touchCell(col, row);
        if (col == touchCol_) return;
        touchDragged_ = true;
        Sequence& s = ed();
        if (touchNote_ < 0 || touchNote_ >= s.count) return;
        Note& n = s.notes[touchNote_];
        const float q = snapQ();
        const float end = std::ceil((page_ * 16 + (col + 1) * 0.25f) / q - kEps) * q;
        const float len = std::max(q, std::min(end - n.st, s.maxLen(n.st, touchNote_)));
        if (std::fabs(len - n.len) > kEps) { n.len = len; modelDirty_ = true; echo(kLength, len); }
    }

    void touchTick()
    {
        if (touchState_ == 1 && msNow() - touchBeginMs_ >= kTouchSettleMs) { touchState_ = 2; touchBegin(); }
        if (touchState_ == 2 && msNow() - lastTouchMs_ >= kTouchReleaseMs) touchEnd();
    }

    void touchBegin()
    {
        int col, row;
        touchCell(col, row);
        touchCol_ = col;
        touchDragged_ = false;
        touchDelete_ = false;
        Sequence& s = ed();
        const float pos = page_ * 16 + col * 0.25f;
        const int pitch = std::max(0, std::min(kPitchN - 1, kCenter + viewLow_ + row));
        touchNote_ = -1;
        for (int i = 0; i < s.count; ++i)
        {
            const Note& n = s.notes[i];
            if (n.pitch == pitch && pos + 0.25f > n.st + kEps && pos < n.st + n.len - kEps) { touchNote_ = i; break; }
        }
        if (touchNote_ >= 0)
        {
            if (touchNote_ == sel_) touchDelete_ = true;   // deleted at release if not dragged
            sel_ = touchNote_;
            echoNote();
            return;
        }
        if (pos >= s.steps - kEps) return;
        const float q = snapQ();
        const float st = std::floor(pos / q + kEps) * q;
        if (s.noteAt(st) >= 0) return;
        const float elen = kEntryLens[std::max(0, std::min(8, getInt(kEntryLen)))];
        const float len = std::min(elen, s.maxLen(st, -1));
        if (len < kMinLen) return;
        Note nt;
        nt.st = st; nt.len = len; nt.pitch = (int16_t)pitch;
        const int idx = s.insert(nt);
        if (idx < 0) return;
        sel_ = touchNote_ = idx;
        cursor_ = st;
        echo(kCursor, cursor_);
        modelDirty_ = true;
        echoNote();
        // preview the new note briefly
        previewCh_ = startNote((float)tuning_.midiOf(pitch), 0.8f);
        previewOffMs_ = msNow() + 200;
    }

    void touchEnd()
    {
        Sequence& s = ed();
        if (touchDelete_ && !touchDragged_ && touchNote_ >= 0 && touchNote_ < s.count)
        {
            s.remove(touchNote_);
            sel_ = std::min(sel_, s.count - 1);
            modelDirty_ = true;
            echoNote();
        }
        touchState_ = 0;
        touchNote_ = -1;
        // move the (invisible) puck away, so the next tap on the same place is a change again
        setRT(kTouchX, kTouchIdle);
        setRT(kTouchY, kTouchIdle);
    }

    // Write a parameter value now (audio thread: an atomic store). The worker later tells the
    // host to redraw it. Values never change behind the audio thread's back, so a project
    // reload gives the same values.
    void setRT(int i, float norm)
    {
        if (getNormalised(i) == norm) return;
        setNormalised(i, norm);
        lastSeen_[i] = norm;
        notify_[i].store(true, std::memory_order_release);
        notifyPending_ = true;
    }
    void echo(int i, float plain) { setRT(i, param(i).toNormalised(plain)); }
    // Display parameters: written when the computed value changes (a fresh instance draws
    // everything; values restored from a project stay until the display really changes).
    void setDisp(int i, float norm)
    {
        if (computed_[i] == norm) return;
        computed_[i] = norm;
        if (primeOnly_ || getNormalised(i) == norm) return;
        setNormalised(i, norm);
        lastSeen_[i] = norm;
        notify_[i].store(true, std::memory_order_release);
        notifyPending_ = true;
    }

    void syncLaneOps()
    {
        Note* n = selNote();
        for (int l = 0; l < kLanes; ++l) laneOp_[l].store(n ? n->ch[l].op : 0);
        dispCount_.store(ed().count);
    }

    void echoNote()
    {
        viewDirty_ = true;   // the selection highlight
        Sequence& s = ed();
        dispCount_.store(s.count);
        echo(kNote, (float)(std::max(0, sel_) + 1));
        Note* n = selNote();
        if (!n) { for (int l = 0; l < kLanes; ++l) laneOp_[l].store(0); return; }
        echo(kStart, n->st); echo(kLength, n->len); echo(kDegree, (float)(n->pitch - kCenter));
        echo(kVel, n->v * 100); echo(kGlide, n->gl); echo(kRatch, n->r); echo(kFade, n->fd); echo(kDuty, n->d);
        for (int l = 0; l < kLanes; ++l)
        {
            const Cell& c = n->ch[l];
            laneOp_[l].store(c.op);
            echo(kLane0 + l * 4, c.op); echo(kLane0 + l * 4 + 1, c.prob * 100);
            echo(kLane0 + l * 4 + 2, c.ptn); echo(kLane0 + l * 4 + 3, c.amt);
        }
        // keep the selected note on screen
        const int page = std::max(0, std::min(3, (int)std::floor(n->st / 16)));
        if (page != page_) { page_ = page; echo(kPage, page + 1); }
        const int rel = n->pitch - kCenter;
        if (rel < viewLow_ || rel >= viewLow_ + kRows) { viewLow_ = std::max(-kCenter, rel - kRows / 2); echo(kViewLow, viewLow_); }
    }

    void echoSequence()
    {
        Sequence& s = ed();
        echo(kSteps, s.steps); echo(kRangeStart, s.rStart + 1); echo(kRangeEnd, s.rEnd + 1); echo(kSwing, s.swing);
        echo(kCursor, cursor_);
        for (int d = 0; d < 31; ++d) echo(kMask0 + d, tuning_.mask[d] ? 1.0f : 0.0f);
        sel_ = std::min(sel_, s.count - 1);
        if (sel_ < 0 && s.count > 0) sel_ = 0;
        echoNote();
    }

    void onEdit(int i, float v)
    {
        Sequence& s = ed();
        Note* n = selNote();
        const int iv = (int)std::lround(v);
        switch (i)
        {
            case kSequence:
            {
                const int q = std::max(0, std::min(kSeqs - 1, iv - 1));
                if (q == edSeq_) break;
                edSeq_ = q;
                sel_ = 0;
                if (edSeq_ != engSeq_) { pendSwap_ = true; buildTables(ed(), tuning_, tables_[1 - tIdx_]); }
                else pendSwap_ = false;
                echoSequence();
                break;
            }
            case kSteps:
                s.steps = std::max(1, std::min(kMaxSteps, iv));
                s.fitToSteps();
                cursor_ = std::min(cursor_, (float)s.steps - snapQ());
                echo(kRangeStart, s.rStart + 1); echo(kRangeEnd, s.rEnd + 1);
                modelDirty_ = true; echoNote();
                break;
            case kRangeStart: s.rStart = std::max(0, std::min(s.steps - 1, iv - 1)); s.rEnd = std::max(s.rEnd, s.rStart);
                echo(kRangeEnd, s.rEnd + 1); modelDirty_ = true; break;
            case kRangeEnd: s.rEnd = std::max(0, std::min(s.steps - 1, iv - 1)); s.rStart = std::min(s.rStart, s.rEnd);
                echo(kRangeStart, s.rStart + 1); modelDirty_ = true; break;
            case kSwing: s.swing = std::max(50.0f, std::min(75.0f, v)); modelDirty_ = true; break;
            case kRoot: case kRootTune: applyRoot(); modelDirty_ = true; break;
            case kNote:
                sel_ = std::max(0, std::min(s.count - 1, iv - 1));
                echoNote();
                break;
            case kStart:
                if (n)
                {
                    const float st = std::max(0.0f, snapTo(v));
                    if (std::fabs(st - n->st) > kEps && s.fits(st, n->len, sel_))
                    {
                        Note moved = *n;
                        moved.st = st;
                        s.remove(sel_);
                        sel_ = s.insert(moved);
                        modelDirty_ = true;
                    }
                    viewDirty_ = true;
                }
                break;
            case kLength:
                if (n)
                {
                    float len = std::max(snapQ(), snapTo(v));
                    len = std::max(kMinLen, std::min(len, s.maxLen(n->st, sel_)));
                    if (std::fabs(len - n->len) > kEps) { n->len = len; modelDirty_ = true; }
                    if (std::fabs(len - v) > snapQ()) echo(kLength, len);
                }
                break;
            case kDegree:
                if (n)
                {
                    int p = std::max(0, std::min(kPitchN - 1, kCenter + iv));
                    const int dirn = p >= n->pitch ? 1 : -1;
                    for (int k = 0; k < tuning_.size() && !tuning_.inScale(p); ++k) p = std::max(0, std::min(kPitchN - 1, p + dirn));
                    if (p != n->pitch) { n->pitch = (int16_t)p; modelDirty_ = true; }
                    if (p - kCenter != iv) echo(kDegree, (float)(p - kCenter));
                }
                break;
            case kVel: if (n) { n->v = v / 100; modelDirty_ = true; } break;
            case kGlide: if (n) { n->gl = v; modelDirty_ = true; } break;
            case kRatch: if (n) { n->r = (int8_t)iv; modelDirty_ = true; } break;
            case kFade: if (n) { n->fd = (int8_t)iv; modelDirty_ = true; } break;
            case kDuty: if (n) { n->d = v; modelDirty_ = true; } break;
            case kCursor: cursor_ = std::max(0.0f, std::min((float)s.steps - snapQ(), snapTo(v))); viewDirty_ = true; break;
            case kDelete:
                if (v >= 0.5f)
                {
                    if (n) { s.remove(sel_); sel_ = std::min(sel_, s.count - 1); modelDirty_ = true; }
                    echo(kDelete, 0);
                    echoNote();
                }
                break;
            case kViewLow: viewLow_ = iv; viewDirty_ = true; break;
            case kPage: page_ = std::max(0, std::min(3, iv - 1)); viewDirty_ = true; break;
            default:
                if (i >= kLane0 && i < kMask0 && n)
                {
                    const int l = (i - kLane0) / 4, f = (i - kLane0) % 4;
                    Cell& c = n->ch[l];
                    if (f == 0)
                    {
                        c.op = (uint8_t)std::max(0, std::min(kNumOps - 1, iv));
                        c.amt = (uint8_t)opInfo(c.op).def;
                        if (c.op && c.prob <= 0) { c.prob = 1.0f; echo(kLane0 + l * 4 + 1, 100); }
                        laneOp_[l].store(c.op);
                        echo(kLane0 + l * 4 + 3, c.amt);
                    }
                    else if (f == 1) c.prob = v / 100;
                    else if (f == 2) c.ptn = (uint8_t)std::max(0, std::min(kNumPtns - 1, iv));
                    else
                    {
                        const OpInfo& o = opInfo(c.op);
                        const int lo = o.unit == U_DEG ? 0 : o.lo;
                        const int a = std::max(lo, std::min(o.hi, iv));
                        c.amt = (uint8_t)a;
                        if (a != iv) echo(i, a);
                    }
                    modelDirty_ = true;
                }
                else if (i >= kMask0 && i < kMask0 + 31)
                {
                    const int d = i - kMask0;
                    tuning_.mask[d] = v >= 0.5f;
                    int on = 0;
                    for (int k = 0; k < tuning_.size() && k < kMaxMask; ++k) on += tuning_.mask[k] ? 1 : 0;
                    if (on == 0) { tuning_.mask[d] = true; echo(i, 1); }
                    for (int k = 0; k < kMaxMask; ++k) model_->mask[k] = tuning_.mask[k];
                    rebuildAll();
                    modelDirty_ = true;
                }
                break;
        }
    }

    // ===== audio thread: keys (step entry, preview) ================================================
    // Pads 1-16 (Pad 1 Note .. +15) are the 16 visible rows, pad 1 = the bottom row.
    int keyToPitch(int key) const
    {
        const int row = key - getInt(kPadNote);
        if (row < 0 || row >= kRows) return -1;
        return std::max(0, std::min(kPitchN - 1, kCenter + viewLow_ + row));
    }

    void onKey(int key, int vel)
    {
        echo(kLastKey, (float)key);
        const int p = keyToPitch(key);
        if (p < 0) return;
        // preview: play the tuned note while the key is held
        if (keyCh_[key] >= 0) offKey(key);
        keyCh_[key] = startNote((float)tuning_.midiOf(p), vel / 127.0f);
        if (!getBool(kStepEntry)) return;
        Sequence& s = ed();
        const float elen = kEntryLens[std::max(0, std::min(8, getInt(kEntryLen)))];
        const int at = s.noteAt(cursor_);
        if (at >= 0 && std::fabs(s.notes[at].st - cursor_) < kEps)
        {
            s.notes[at].pitch = (int16_t)p;
            s.notes[at].len = std::max(kMinLen, std::min(elen, s.maxLen(cursor_, at)));
            sel_ = at;
        }
        else if (at < 0)
        {
            const float len = std::min(elen, s.maxLen(cursor_, -1));
            if (len >= kMinLen)
            {
                Note nt;
                nt.st = cursor_; nt.len = len; nt.pitch = (int16_t)p; nt.v = vel / 127.0f;
                const int idx = s.insert(nt);
                if (idx >= 0) sel_ = idx;
            }
        }
        cursor_ += elen;
        if (cursor_ >= s.steps - kEps) cursor_ = 0;
        echo(kCursor, cursor_);
        modelDirty_ = true;
        echoNote();
    }

    void offKey(int key)
    {
        if (key < 0 || key > 127 || keyCh_[key] < 0) return;
        stopNote(keyCh_[key]);
        keyCh_[key] = -1;
    }

    // ===== audio thread: MIDI out ==================================================================
    struct Chan { bool on = false; int note = 0; float ref = 0; int bend = 0x7FFFFFFF; uint32_t started = 0, released = 0; };

    int pickChannel()
    {
        const int first = getInt(kFirstCh) - 1;
        const int count = std::max(1, std::min(getInt(kVoices), 16 - first));
        int best = -1, oldest = first;
        for (int c = first; c < first + count; ++c)
        {
            if (!chans_[c].on) { if (best < 0 || chans_[c].released < chans_[best].released) best = c; }
            else if (chans_[c].started < chans_[oldest].started) oldest = c;
        }
        if (best >= 0) return best;
        stopNote(oldest);
        return oldest;
    }

    int bendFor(float cents) const
    {
        return scala::bendValue(cents, getInt(kBendUp), getInt(kBendDown),
                                getInt(kCurve) == 1 ? scala::BendCurve::Opx4 : scala::BendCurve::Symmetric);
    }
    bool inBendRange(float semis) const { return semis <= getInt(kBendUp) && semis >= -getInt(kBendDown); }

    void sendBend(int c, int b)
    {
        if (b == chans_[c].bend) return;
        const int v = b + 8192;
        ring_.push((uint8_t)(0xE0 | c), (uint8_t)(v & 0x7F), (uint8_t)(v >> 7));
        chans_[c].bend = b;
    }

    int startNote(float pitch, float vel01)
    {
        const int c = pickChannel();
        const int n12 = std::max(0, std::min(127, (int)std::lround(pitch)));
        sendBend(c, bendFor((pitch - n12) * 100.0f));
        int v = (int)std::lround(vel01 * 127.0f * get(kVelocity) / 100.0f);
        v = v < 1 ? 1 : (v > 127 ? 127 : v);
        ring_.push((uint8_t)(0x90 | c), (uint8_t)n12, (uint8_t)v);
        Chan& ch = chans_[c];
        ch.on = true; ch.note = n12; ch.ref = (float)n12; ch.started = ++clock_;
        return c;
    }

    void stopNote(int c)
    {
        if (c < 0 || c > 15 || !chans_[c].on) return;
        ring_.push((uint8_t)(0x80 | c), (uint8_t)chans_[c].note, 0);
        chans_[c].on = false;
        chans_[c].released = ++clock_;
    }

    // engine output -> MIDI; returns true if something was queued
    bool toMidi(const EngineOut& o)
    {
        bool sent = false;
        const bool high = o.gate > 0;
        if (high && !outHigh_)
        {
            outCh_ = startNote(o.pitch, o.vel);
            lastPitch_ = o.pitch; lastBendAt_ = sampleCount_;
            sent = true;
        }
        else if (!high && outHigh_)
        {
            stopNote(outCh_);
            outCh_ = -1;
            sent = true;
        }
        else if (high && outCh_ >= 0 && std::fabs(o.pitch - lastPitch_) > 0.0005f && sampleCount_ - lastBendAt_ >= 64)
        {
            const float semis = o.pitch - chans_[outCh_].ref;
            if (inBendRange(semis)) sendBend(outCh_, bendFor(semis * 100.0f));
            else { stopNote(outCh_); outCh_ = startNote(o.pitch, o.vel); }
            lastPitch_ = o.pitch; lastBendAt_ = sampleCount_;
            sent = true;
        }
        outHigh_ = high;
        return sent;
    }

    void noteOffActive()
    {
        if (outCh_ >= 0) stopNote(outCh_);
        outCh_ = -1;
        outHigh_ = false;
    }

    void allNotesOff()
    {
        for (int c = 0; c < 16; ++c) stopNote(c);
        for (int& k : keyCh_) k = -1;
        outCh_ = -1;
        outHigh_ = false;
    }

    // ===== row geometry: rows sized by cents =====================================================
    // rowY_[r] = bottom of visible row r as a fraction of the grid height (0 = bottom); rowY_[16] = 1.
    double centsOf(int idx) const { return tuning_.periodOf(idx) * tuning_.period() + tuning_.cents(tuning_.degreeOf(idx)); }
    void computeRows()
    {
        const int low = kCenter + viewLow_;
        const double c0 = centsOf(low), span = std::max(1e-6, centsOf(low + kRows) - c0);
        for (int r = 0; r <= kRows; ++r) rowY_[r] = (float)((centsOf(low + r) - c0) / span);
    }
    static float markY(float frac) { return kMarkPark + frac * (1.0f - kMarkPark); }
    int rowAtY(float y) const
    {
        for (int r = 0; r < kRows; ++r) if (y < rowY_[r + 1]) return r;
        return kRows - 1;
    }

    // ===== audio thread -> worker: the grid view ==================================================
    // The grid, row labels and cursor as display parameters (audio thread; only changed values).
    void refreshView()
    {
        Sequence& s = ed();
        computeRows();
        int copies[31] = {0};
        float labY[31][3] = {};
        int periods = 0;
        float perY[3] = {0, 0, 0};
        for (int r = 0; r < kRows; ++r)
        {
            const int idx = std::max(0, std::min(kPitchN - 1, kCenter + viewLow_ + r));   // row 0 = bottom
            setDisp(kRow0 + r, param(kRow0 + r).toNormalised((float)idx));
            const int d = tuning_.degreeOf(idx);
            if (d < 31 && copies[d] < 3) labY[d][copies[d]++] = markY((rowY_[r] + rowY_[r + 1]) * 0.5f);
            if (d == 0 && periods < 3) perY[periods++] = markY(rowY_[r]);
        }
        for (int i = 0; i <= kRows; ++i) setDisp(kRowLine0 + i, markY(rowY_[i]));
        for (int i = 0; i < 3; ++i) setDisp(kPerLine0 + i, perY[i]);
        for (int d = 0; d < 31; ++d)
            for (int k = 0; k < 3; ++k) setDisp(kLab0 + d * 3 + k, labY[d][k]);
        // one note at most in each quarter-step column (monophonic)
        for (int c = 0; c < kCols; ++c)
        {
            const float a = page_ * 16 + c * 0.25f, b = a + 0.25f;
            float yA = 0, yB = 0, yS = 0;
            for (int i = 0; i < s.count; ++i)
            {
                const Note& n = s.notes[i];
                if (b <= n.st + kEps || a >= n.st + n.len - kEps) continue;
                const int r = n.pitch - (kCenter + viewLow_);
                if (r >= 0 && r < kRows)
                {
                    const float y = markY((rowY_[r] + rowY_[r + 1]) * 0.5f);
                    yA = y;
                    if (n.st >= a - kEps && n.st < b - kEps) yB = y;
                    if (i == sel_) yS = y;
                }
                break;
            }
            setDisp(kColX0 + c, (c + 0.5f) / kCols);
            setDisp(kMarkA0 + c, yA);
            setDisp(kMarkB0 + c, yB);
            setDisp(kMarkS0 + c, yS);
        }
        {
            const float ex = (s.steps - page_ * 16) / 16.0f;
            setDisp(kEndX, (ex > 0 && ex < 1) ? ex * kGridFrac : kParkX);
        }
        const float cx = (cursor_ - page_ * 16) / 16.0f;
        setDisp(kCursorX, (cx >= 0 && cx < 1) ? (cx + 0.5f / 64) * kGridFrac : kParkX);
        setDisp(kLineY, 0.5f);
        dispCount_.store(s.count);
    }

    // ===== worker thread ==========================================================================
    void loadScale(int idx, const std::string* embeddedText)
    {
        std::string text, name;
        if (embeddedText) { text = *embeddedText; name = embedName_; }
        else if (idx >= 0 && idx < (int)scales_.size() && mpc::readTextFile(scales_[(size_t)idx].path, text)) name = scales_[(size_t)idx].name;
        else { text = kBuiltin12; name = "12-EDO"; }
        const int pub = scalePub_.load(), use = scaleInUse_;
        int slot = 0;
        while (slot == pub || slot == use) ++slot;
        if (!scala::parseScl(text, scaleSlots_[slot])) { scala::parseScl(kBuiltin12, scaleSlots_[slot]); text = kBuiltin12; name = "12-EDO"; }
        {
            std::lock_guard<std::mutex> l(scaleMutex_);
            scaleName_ = name; scaleText_ = text;
        }
        scalePub_.store(slot);
    }

    void workerLoop()
    {
        alsa_.open(clientName_, "MIDI Out");
        while (!quit_.load())
        {
            wake_.waitMs(10);
            uint8_t m[3];
            int n = 0;
            while (ring_.pop(m)) { deliver(m); ++n; }
            if (n) alsa_.flush();
            if (embedPending_.exchange(false))
            {
                std::string t;
                { std::lock_guard<std::mutex> l(scaleMutex_); t = embedText_; }
                loadScale(-1, &t);
            }
            else if (scaleDirty_.exchange(false))
            {
                const int si = getInt(kScale);
                bool skip;
                { std::lock_guard<std::mutex> l(scaleMutex_); skip = (si == appliedScale_); appliedScale_ = -1; }
                if (!skip) loadScale(si, nullptr);
            }
            // tell the host which values changed (the audio thread wrote them)
            for (int i = 0; i < kNumParams; ++i)
                if (notify_[i].exchange(false, std::memory_order_acquire)) notifyHostParameterChanged(i);
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
#ifdef SCALASEQ_TEST
        std::lock_guard<std::mutex> l(testMutex_);
        testLog_.push_back({m[0], m[1], m[2]});
#endif
    }

    // ----- state ----------------------------------------------------------------------------------
    std::unique_ptr<Model> model_, saved_, staging_;
    std::unique_ptr<Tables[]> tables_;
    std::atomic<bool> stagingReady_{false};
    std::mutex savedMutex_;
    bool savedDirty_ = false, savedAll_ = true, modelDirty_ = false, viewDirty_ = true;

    std::vector<rt::FileEntry> scales_;
    scala::Scale scaleSlots_[3];
    std::atomic<int> scalePub_{0};
    int scaleInUse_ = 0;
    std::mutex scaleMutex_;
    std::string scaleName_, scaleText_, embedName_, embedText_;
    int appliedScale_ = -1;
    std::atomic<bool> scaleDirty_{false}, embedPending_{false};

    Tuning tuning_;
    Engine engine_;
    int tIdx_ = 0, edSeq_ = 0, engSeq_ = 0, sel_ = 0, page_ = 0, viewLow_ = 0;
    bool pendSwap_ = false, wasPlaying_ = false;
    float cursor_ = 0;
    double expectPpq_ = 0;
    int playStep_ = -1;

    float lastSeen_[kNumParams];
    float computed_[kNumParams];   // display parameters: the last value the plugin computed
    bool primeOnly_ = false;
    float rowY_[kRows + 1] = {};
    std::atomic<bool> notify_[kNumParams];
    bool notifyPending_ = false;

    Chan chans_[16];
    uint32_t clock_ = 0;
    int keyCh_[128];
    int outCh_ = -1;
    bool outHigh_ = false;
    float lastPitch_ = 0;
    uint64_t sampleCount_ = 0, lastBendAt_ = 0;
    // touch gesture
    int touchState_ = 0;   // 0 idle, 1 settling, 2 down
    int touchNote_ = -1, touchCol_ = 0;
    bool touchDragged_ = false, touchDelete_ = false;
    double touchBeginMs_ = 0, lastTouchMs_ = 0;
    int previewCh_ = -1;
    double previewOffMs_ = 0;

    std::atomic<int> dispSize_{12}, dispCount_{0};
    std::atomic<bool> dispMask_[kMaxMask];
    std::atomic<int> laneOp_[kLanes] = {};

    rt::MsgRing ring_;
    rt::Wakeup wake_;
    alsa::Output alsa_;
    char clientName_[48] = {};
    std::atomic<bool> quit_{false};
    std::thread worker_;

#ifdef SCALASEQ_TEST
public:
    std::mutex testMutex_;
    std::vector<std::array<uint8_t, 3>> testLog_;
    Model& testModel() { return *model_; }
    int testSel() const { return sel_; }
    float testCursor() const { return cursor_; }
#endif
};

MPC_PLUGIN_ENTRY(ScalaSeq)
