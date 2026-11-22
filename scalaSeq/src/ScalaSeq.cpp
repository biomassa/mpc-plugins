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
//     -> lock-free queue -> MIDI thread (real-time priority) -> ALSA sequencer port
//   Pads / keys on this track: step entry at the cursor (when Step Entry is on) and preview.
//
// The SEQ page (2.0) is a piano roll drawn from XYPad markers that the plugin places through
// display parameters. Touch comes from invisible XYPads whose handles use ResetOnGestureEnd:
// the MPC returns both values to 0 when the finger lifts, so press, drag and release are exact.
//   roll:   press-drag on empty = draw a note; on a note: body = move, right third = length;
//           double tap = delete
//   ruler:  drag sideways = scroll, up/down = zoom (Steps/Page)
//   labels: drag up/down = scroll the rows
//   lane:   drag a velocity bar = velocity
//   FREE:   while held (second finger), drags do not snap
//
// Threads
//   audio thread: owns the 32 sequences, applies knob and touch edits, runs the engine, computes
//                 the display values.
//   MIDI thread:  sends the queued MIDI to ALSA.
//   worker:       loads scale files and tells the host which display values changed.
//   UI thread:    project save / load (a copy made by the audio thread; applied in reset()).

#include "mpc_plugin.h"
#ifdef SCALASEQ_TRACE
#include "trace.h"
#endif
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
#if defined(__linux__)
  #include <pthread.h>
  #include <sched.h>
#endif

namespace {

using namespace seq;

constexpr int kRows = 16;
constexpr int kShown = 32;          // notes drawn exactly on one screen
constexpr int kPieces = 11;         // bar pieces of 1, 2, 4 ... 1024 px
constexpr int kGaps = 7;            // ratchet gaps per note (8 ratchets)
constexpr int kStepLines = 65, kSnapLines = 89, kGrpLines = 65, kNums = 64;

enum ParamIndex
{
    // SETUP
    kVelocity = 0, kSteps, kRangeStart, kRangeEnd, kSwing, kRate, kDirection, kSnap,
    kScale, kRoot, kRootTune, kFirstCh, kVoices, kBendUp, kBendDown, kCurve, kPtnMode,
    kGroup, kZoom, kPadNote,
    // NOTE (knobs)
    kNoteSel, kDeg, kPeriod, kMove, kLen, kNudge, kVel, kGlide, kRatch, kFade, kDuty,
    kCursor, kEntryLen, kStepEntry, kDelete,
    // VIEW (knobs)
    kViewLow, kViewStart, kSequence,
    // CHANCE: 4 lanes x (op, prob, ptn, amount)
    kLane0,
    // MASK
    kMask0 = kLane0 + 4 * kLanes,
    // texts (written by the plugin)
    kLastKey = kMask0 + 31, kDegText, kViewText, kFreeLit,
    // touch surfaces (ResetOnGestureEnd: 0, 0 = no finger)
    kRollTX, kRollTY, kRulTX, kRulTY, kLabTX, kLabTY, kVelTX, kVelTY, kFreeTX, kFreeTY,
    // display markers (written by the plugin)
    kLineY, kPlayX, kCursorX, kEndX,
    kRowLine0, kPerLine0 = kRowLine0 + 17, kLab0 = kPerLine0 + 3,
    kStepLine0 = kLab0 + 31 * 3, kSnapLine0 = kStepLine0 + kStepLines, kGrpLine0 = kSnapLine0 + kSnapLines,
    kNumX0 = kGrpLine0 + kGrpLines,
    kNoteY0 = kNumX0 + kNums, kPieceX0 = kNoteY0 + kShown, kNotchX0 = kPieceX0 + kShown * kPieces,
    kGapX0 = kNotchX0 + kShown, kGlideX0 = kGapX0 + kShown * kGaps, kChanceX0 = kGlideX0 + kShown,
    kSelY = kChanceX0 + kShown, kSelPieceX0, kSelNotchX = kSelPieceX0 + kPieces,
    kVelX0, kVelY0 = kVelX0 + kShown, kSelVelX = kVelY0 + kShown, kSelVelY,
    // 2.1: delay of the scheduled MIDI output
    kMidiDelay,
    // 2.3: chance lanes of the selected note as text (SEQ page), clear switches (SETUP page)
    kChanceText0, kClearNote = kChanceText0 + kLanes, kClearAll,
    // 2.3.1: the selected note's marks, drawn above its highlight
    kSelGapX0, kSelGlideX = kSelGapX0 + kGaps, kSelChanceX,
    // 2.6: which drawn list is open (0 = none): Entry Len, Op 1-4, PTN 1-4, Rate, Scale
    kListOpen,
    // 2.7: momentary < > buttons of the step controls on the NOTE page (Note, Move, Length)
    kNotePrev, kNoteNext, kMovePrev, kMoveNext, kLenPrev, kLenNext,
    // 2.8: RATE: rate mode of the edited sequence; LOCK: the selected note's locks, the sequence's lock CCs
    kRMode, kRRatio, kRFine, kLock0, kLockCC0 = kLock0 + 4,
    // 2.8: LANE: which lane is shown (VEL, CC 1, CC 2) and the shown CC lane of the edited sequence
    kLaneSel = kLockCC0 + 4, kLnCC, kLnSteps, kLnRate, kLnMode, kLnRatio, kLnFine, kLnDir, kLnRest,
    // 2.8: LANE display: the lane's current step (a line) and a thin bar at each step of the view
    kLnPlayX, kLnBarX0, kLnBarY0 = kLnBarX0 + kMaxSteps,
    // 2.8: SETUP: copy, paste and clear the edited sequence (PASTE and CLEAR: touch twice within 3 s)
    kSeqCopy = kLnBarY0 + kMaxSteps, kSeqPaste, kSeqClear,
    // 2.8: SEND buttons (one CC at value 64 for MIDI Learn): Lock CC 1-4, the shown lane
    kSend0, kNumSends = 5,
    kNumParams = kSend0 + kNumSends
};
constexpr int kFirstEdit = kSteps, kLastEdit = kSequence;
// 2.5: MIDI notes 60-91 on this track select sequences 1-32 (fixed). The 16 pad rows (Pad 1 Note
// .. +15) must not overlap them, so Pad 1 Note cannot be 45-91.
constexpr int kSeqNote0 = 60;
constexpr int kPadLowMax = kSeqNote0 - kRows, kPadHighMin = kSeqNote0 + kSeqs;   // 44 and 92
inline bool padNoteOk(int v) { return v <= kPadLowMax || v >= kPadHighMin; }

// ---- SEQ page geometry in pixels (must match skin/make_skin.py) -------------------------------
constexpr float kGW = 880, kGH = 540;            // the roll (2.8: + the old velocity lane, was 476)
constexpr float kPARK = 40;                      // masked strip right of the roll (parked lines)
constexpr float kPARKT = 44;                     // labels / row lines park above the roll (masked)
constexpr float kOFF = 1100;                     // x-parked markers: pad starts kOFF px left of the roll
constexpr float kNoteH = 24;
constexpr float kNotchW = 4, kGapW = 2, kBadgeW = 16, kNumW = 40, kVelW = 5, kVelH = 280, kFreeW = 272;   // 2.8: kVelH = the LANE page lane height
constexpr float kRulerPadH = 2628;               // ruler touch pad height (tall, so drags have room)
constexpr float kMinLinePx = 10;                 // snap lines closer than this are hidden
constexpr float kNumMinPx = 30;                  // ruler numbers at least this far apart
constexpr float kDragPx = 6;                     // a touch that moves less is a tap
constexpr double kDoubleTapMs = 300;
constexpr float kZoomPx = 12;                    // ruler: px of vertical drag for one step of zoom
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

// One touch surface: the two ResetOnGestureEnd values. Both 0 = no finger. On lift the MPC
// resets x first, then y: one value at 0 = the lift has begun (ignored).
struct Touch
{
    bool down = false, dragged = false;
    float x = 0, y = 0;      // last position (0..1)
    float x0 = 0, y0 = 0;    // press position
    double t0 = 0;
};

std::atomic<int> g_instances{0};
// 2.8: the copied sequence, shared by all scalaSeq instances (lost when the MPC app restarts)
Sequence g_clip;
bool g_clipHas = false;
std::mutex g_clipMutex;

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
        auto num = [](const char* a, int i) { return std::string(a) + std::to_string(i + 1); };

        // SETUP
        addParam(mpc::floatParam ("velocity",  "Velocity",   1, 200, 100, "%", 0));
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
        addParam(mpc::intParam   ("group",     "Group",      0, 64, 0));
        addParam(mpc::intParam   ("zoom",      "Steps/Page", 1, kMaxSteps, 16));
        addParam(mpc::intParam   ("padnote",   "Pad 1 Note", 0, 127, 37));
        // NOTE: knobs with 64 values or fewer (the MPC gives a knob about 128 positions)
        addParam(mpc::intParam   ("note",      "Note",       1, 64, 1));
        addParam(mpc::intParam   ("degree",    "Degree",     0, 63, 0));   // spread over the degrees of the scale mask
        addParam(mpc::intParam   ("period",    "Period",     -4, 4, 0));
        addParam(mpc::intParam   ("move",      "Move",       0, 63, 0));   // snap steps
        addParam(mpc::intParam   ("length",    "Length",     1, 64, 1));   // snap steps
        addParam(mpc::intParam   ("nudge",     "Nudge",      -32, 31, 0)); // 1/96 step
        addParam(mpc::floatParam ("vel",       "VEL",        0, 100, 100, "%", 0));
        addParam(mpc::floatParam ("glide",     "GLIDE",      0, 1, 0, "st", 2));
        addParam(mpc::intParam   ("ratch",     "RATCH",      1, 8, 1));
        addParam(mpc::intParam   ("fade",      "FADE",       -50, 50, 0));
        addParam(mpc::floatParam ("duty",      "DUTY",       0.05f, 0.95f, 0.5f, "", 2));
        addParam(mpc::floatParam ("cursor",    "Cursor",     0, kMaxSteps, 0, "st", 2));
        addParam(mpc::choiceParam("entrylen",  "Entry Len",  {"1/8", "1/4", "1/2", "3/4", "1", "1.5", "2", "3", "4"}, 4));
        addParam(mpc::boolParam  ("stepentry", "Step Entry", false));
        addParam(mpc::boolParam  ("delete",    "Delete",     false));
        // VIEW
        addParam(mpc::intParam   ("viewlow",   "View Low",   -32, 31, 0));
        addParam(mpc::intParam   ("viewstart", "View Start", 1, kMaxSteps, 1));
        addParam(mpc::intParam   ("sequence",  "Sequence",   1, kSeqs, 1));
        // CHANCE
        std::vector<std::string> opNames, ptnNames{"always"};
        for (int o = 0; o < kNumOps; ++o) opNames.push_back(opInfo(opOfChoice(o)).name);
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
        // texts
        addParam(mpc::intParam   ("lastkey",   "Last Note",  0, 127, 0));
        addParam(mpc::floatParam ("degtext",   "Deg Text",   0, 1, 0, "", 3));
        addParam(mpc::floatParam ("viewtext",  "View Text",  0, 1, 0, "", 3));
        addParam(mpc::floatParam ("freelit",   "Free Lit",   0, 1, 1, "", 4));   // 1 = parked off the right edge
        // touch surfaces
        const char* tn[5] = {"roll", "ruler", "labels", "lane", "free"};
        for (int t = 0; t < 5; ++t)
        {
            addParam(mpc::floatParam((std::string(tn[t]) + "tx").c_str(), (std::string("Touch ") + tn[t] + " X").c_str(), 0, 1, 0, "", 4));
            addParam(mpc::floatParam((std::string(tn[t]) + "ty").c_str(), (std::string("Touch ") + tn[t] + " Y").c_str(), 0, 1, 0, "", 4));
        }
        // display markers
        auto disp = [&](const std::string& id, const std::string& name, float def) { addParam(mpc::floatParam(id.c_str(), name.c_str(), 0, 1, def, "", 4)); };
        disp("liney", "Line Y", 0.5f);
        disp("playx", "Play X", 1); disp("cursorx", "Cursor X", 1); disp("endx", "End X", 1);
        for (int i = 0; i < 17; ++i) disp(num("rowline", i), num("Row Line ", i), 1);
        for (int i = 0; i < 3; ++i) disp(num("perline", i), num("Per Line ", i), 1);
        for (int d = 0; d < 31; ++d)
            for (int k = 0; k < 3; ++k)
                disp("lab" + std::to_string(d) + "_" + std::to_string(k + 1), "Lab " + std::to_string(d) + "." + std::to_string(k + 1), 1);
        for (int i = 0; i < kStepLines; ++i) disp(num("stepline", i), num("Step Line ", i), 1);
        for (int i = 0; i < kSnapLines; ++i) disp(num("snapline", i), num("Snap Line ", i), 1);
        for (int i = 0; i < kGrpLines; ++i) disp(num("grpline", i), num("Grp Line ", i), 1);
        for (int i = 0; i < kNums; ++i) disp(num("numx", i), num("Num X ", i), 0);
        for (int j = 0; j < kShown; ++j) disp(num("notey", j), num("Note Y ", j), 0);
        for (int j = 0; j < kShown; ++j)
            for (int k = 0; k < kPieces; ++k) disp("piece" + std::to_string(j + 1) + "_" + std::to_string(k), "Piece " + std::to_string(j + 1) + "." + std::to_string(k), 0);
        for (int j = 0; j < kShown; ++j) disp(num("notchx", j), num("Notch X ", j), 0);
        for (int j = 0; j < kShown; ++j)
            for (int k = 0; k < kGaps; ++k) disp("gap" + std::to_string(j + 1) + "_" + std::to_string(k + 1), "Gap " + std::to_string(j + 1) + "." + std::to_string(k + 1), 0);
        for (int j = 0; j < kShown; ++j) disp(num("glidex", j), num("Glide X ", j), 0);
        for (int j = 0; j < kShown; ++j) disp(num("chancex", j), num("Chance X ", j), 0);
        disp("sely", "Sel Y", 0);
        for (int k = 0; k < kPieces; ++k) disp("selpiece" + std::to_string(k), "Sel Piece " + std::to_string(k), 0);
        disp("selnotchx", "Sel Notch X", 0);
        for (int j = 0; j < kShown; ++j) disp(num("velx", j), num("Vel X ", j), 0);
        for (int j = 0; j < kShown; ++j) disp(num("vely", j), num("Vel Y ", j), 0);
        disp("selvelx", "Sel Vel X", 0); disp("selvely", "Sel Vel Y", 0);
        // 2.1: MIDI goes out scheduled at the exact time of its sample, plus this delay
        addParam(mpc::intParam   ("mididelay", "MIDI Delay*", 0, 30, 12, "ms"));   // not used in 2.1.1 (hidden)
        for (int l = 0; l < kLanes; ++l) disp(num("chancetext", l), num("Chance ", l), 0);
        addParam(mpc::boolParam  ("clearnote", "Clear Note", false));   // clears the chance lanes of the selected note
        addParam(mpc::boolParam  ("clearall",  "Clear All",  false));   // 1st touch arms, 2nd touch within 3 s clears all
        for (int k = 0; k < kGaps; ++k) disp("selgap" + std::to_string(k + 1), "Sel Gap " + std::to_string(k + 1), 0);
        disp("selglidex", "Sel Glide X", 0); disp("selchancex", "Sel Chance X", 0);
        addParam(mpc::intParam   ("listopen",  "List Open",  0, 14, 0));   // 2.8: + Ratio, Lane Rate, Lane Ratio
        // 2.7: momentary step buttons (the plugin sets them back to off); appended, so old projects keep their indices
        addParam(mpc::boolParam  ("noteprev",  "Note Prev",  false));
        addParam(mpc::boolParam  ("notenext",  "Note Next",  false));
        addParam(mpc::boolParam  ("moveprev",  "Move Prev",  false));
        addParam(mpc::boolParam  ("movenext",  "Move Next",  false));
        addParam(mpc::boolParam  ("lenprev",   "Len Prev",   false));
        addParam(mpc::boolParam  ("lennext",   "Len Next",   false));
        // 2.8 (appended)
        {
            std::vector<std::string> ratios, mrates{"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4", "1/2", "1 bar"};
            for (int k = 0; k < kNumRatios; ++k) ratios.push_back(ratioNames()[k]);
            auto nm = [](const char* a, int k) { return std::string(a) + std::to_string(k + 1); };
            addParam(mpc::choiceParam("rmode",     "Rate Mode",  {"Musical", "Ratio"}, 0));
            addParam(mpc::choiceParam("rratio",    "Ratio",      ratios, kDefRatio));
            addParam(mpc::intParam   ("rfine",     "Fine",       -10, 10, 0));
            for (int k = 0; k < 4; ++k) addParam(mpc::intParam(nm("lock", k).c_str(), nm("Lock ", k).c_str(), -1, 127, -1));
            for (int k = 0; k < 4; ++k) addParam(mpc::intParam(nm("lockcc", k).c_str(), nm("Lock CC ", k).c_str(), 0, numCCChoices() - 1, 0));
            addParam(mpc::choiceParam("lanesel",   "Lane",       {"VEL", "CC 1", "CC 2"}, 0));
            addParam(mpc::intParam   ("lncc",      "Lane CC",    0, numCCChoices() - 1, 0));   // the LANE page shows "CC Num"
            addParam(mpc::intParam   ("lnsteps",   "Lane Steps", 1, kMaxSteps, 16));
            addParam(mpc::choiceParam("lnrate",    "Lane Rate",  mrates, kDefLaneRate));
            addParam(mpc::choiceParam("lnmode",    "Lane Mode",  {"Musical", "Ratio"}, 0));
            addParam(mpc::choiceParam("lnratio",   "Lane Ratio", ratios, kDefRatio));
            addParam(mpc::intParam   ("lnfine",    "Lane Fine",  -10, 10, 0));
            addParam(mpc::choiceParam("lndir",     "Lane Dir",   {"Fwd", "Rev", "Rnd"}, 0));
            addParam(mpc::intParam   ("lnrest",    "Rest",       0, 127, 64));
            addParam(mpc::floatParam ("lnplayx",   "Lane Play X", 0, 1, 1, "", 4));
            for (int k = 0; k < kMaxSteps; ++k) addParam(mpc::floatParam(nm("lnbarx", k).c_str(), nm("Lane Bar X ", k).c_str(), 0, 1, 0, "", 4));
            for (int k = 0; k < kMaxSteps; ++k) addParam(mpc::floatParam(nm("lnbary", k).c_str(), nm("Lane Bar Y ", k).c_str(), 0, 1, 0, "", 4));
            addParam(mpc::boolParam  ("seqcopy",   "Copy",       false));
            addParam(mpc::boolParam  ("seqpaste",  "Paste",      false));
            addParam(mpc::boolParam  ("seqclear",  "Clear Seq",  false));
            for (int k = 0; k < 4; ++k) addParam(mpc::boolParam(nm("sendlock", k).c_str(), nm("Send Lock ", k).c_str(), false));
            addParam(mpc::boolParam  ("sendlane",  "Send Lane",  false));
        }

        if (numParams() != kNumParams) std::abort();   // the index layout and the parameter list must match
        for (auto& f : notify_) f.store(false);
        for (int l = 0; l < kCcLanes; ++l) { lnN_[l] = kNoStep; lnIdx_[l] = -1; }
        for (int& c : ccSent_) c = -1;
        for (int i = 0; i < kNumParams; ++i) { lastSeen_[i] = getNormalised(i); computed_[i] = -1; }
        for (int& k : keyCh_) k = -1;

        loadScale(choiceAt(kScale), nullptr);   // before the first block
        tuning_.sc = &scaleSlots_[scalePub_.load()];
        scaleInUse_ = scalePub_.load();
        applyRoot();
        rebuildAll();
        refreshView();
        const int n = g_instances.fetch_add(1);
        if (n == 0) std::snprintf(clientName_, sizeof clientName_, "scalaSeq");
        else        std::snprintf(clientName_, sizeof clientName_, "scalaSeq %d", n + 1);
        midiThread_ = std::thread([this] { midiLoop(); });
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~ScalaSeq() override
    {
        quit_.store(true);
        wake_.post();
        wakeMidi_.post();
        if (worker_.joinable()) worker_.join();
        if (midiThread_.joinable()) midiThread_.join();
    }

    void prepare(double, int) override {}

    // Audio thread, after resume or a state load.
    void reset() override
    {
        bool loaded20 = false;   // a project state saved by 2.0 (its display values match this layout)
        if (stagingReady_.exchange(false))
        {
            *model_ = *staging_;
            for (int d = 0; d < kMaxMask; ++d) tuning_.mask[d] = model_->mask[d];
            edSeq_ = engSeq_ = std::max(0, std::min(kSeqs - 1, stagingView_[0]));
            sel_ = stagingView_[1]; viewStart_ = stagingView_[2]; viewLow_ = stagingView_[3];
            savedAll_ = true;
            loaded20 = true;
        }
        allNotesOff();
        engine_.reset();
        engine_.restart();
        // take the knob values as the host restored them; the touch surfaces are at rest
        for (int i = 0; i < kNumParams; ++i) lastSeen_[i] = getNormalised(i);
        for (Touch& t : touch_) t = Touch();
        if (edSeq_ != engSeq_) { engSeq_ = edSeq_; pendSwap_ = false; }
        sel_ = std::max(0, std::min(ed().count - 1, sel_));
        viewLow_ = std::max(-kCenter, std::min(kPitchN - kRows - kCenter, viewLow_));
        viewStart_ = std::max(0, std::min(ed().steps - 1, viewStart_));
        cursor_ = std::max(0.0f, std::min((float)ed().steps, get(kCursor)));
        // 2.5: a Pad 1 Note from an older project that overlaps the sequence notes moves up
        echo(kListOpen, 0);
        for (int i = kNotePrev; i <= kLenNext; ++i) echo(i, 0);   // momentary buttons start off
        for (int i = kSeqCopy; i <= kSeqClear; ++i) echo(i, 0);
        for (int i = kSend0; i < kSend0 + kNumSends; ++i) echo(i, 0);
        padNote_ = getInt(kPadNote);
        if (!padNoteOk(padNote_)) { padNote_ = kPadHighMin; echo(kPadNote, (float)padNote_); }
        seqEvN_ = 0;
        anchorQ_ = 0;
        applyRoot();
        rebuildAll();
        syncLaneOps();
        echoNote();
        // After a 2.0 state the restored display values stay (remember what they stand for). After
        // anything else (a 1.x project restores some display parameters of an older layout, or no
        // state at all) every display value is written again.
        if (loaded20)
        {
            primeOnly_ = true;
            refreshView();
            primeOnly_ = false;
            trustRestored_ = true;
        }
        else
        {
            for (int i = 0; i < kNumParams; ++i) computed_[i] = -1;
            refreshView();
        }
        viewDirty_ = false;
        wake_.post();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        bool sent = false;
#ifdef SCALASEQ_TRACE
        // Notice test (trace build only): with nobody touching the screen, move the row view one
        // row up and down every 50 ms, through the same path as a label drag. Schedule from the
        // first block: 20 s quiet, 20 s moving, 20 s quiet, 20 s moving, then quiet. A trace
        // record of kind 5 marks each phase (op = phase number).
        {
            const double t = sampleCount_ / (ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0);
            const int phase = t < 20 ? 0 : t < 40 ? 1 : t < 60 ? 2 : t < 80 ? 3 : 4;
            if (phase != testPhase_) { testPhase_ = phase; ssqtrace::put(ssqtrace::K_MARK, phase, 0, ssqtrace::nowNs()); }
            const long tick = (long)(t / 0.05);
            if ((phase == 1 || phase == 3) && tick != testTick_)
            {
                testTick_ = tick;
                const int vl = viewLow_ + ((tick & 1) ? 1 : -1);
                viewLow_ = vl; viewDirty_ = true; echo(kViewLow, (float)std::max(-32, std::min(31, vl)));
            }
        }
#endif
        {
            const int64_t now = rt::monoNs();
            sclock_.update(now, sampleCount_, ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0);
        }
        curSample_ = sampleCount_;
        // scale loaded by the worker
        const int sp = scalePub_.load();
        if (sp != scaleInUse_) { scaleInUse_ = sp; tuning_.sc = &scaleSlots_[sp]; rebuildAll(); echoKnobs(); viewDirty_ = true; }
        pollEdits();
        if (previewCh_ >= 0 && msNow() >= previewOffMs_) { stopNote(previewCh_); previewCh_ = -1; sent = true; }
        if (previewSent_) { previewSent_ = false; sent = true; }
        if (sendPending_) { sendPending_ = false; sent = true; }   // 2.8: a SEND

        // MIDI in: step entry and preview
        for (int i = 0; i < ctx.numMidiEvents; ++i)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (e.isNoteOn() && isSeqNote(e.note()))
            {
                // 2.5: a sequence note. Stopped: select it now. Playing: switch at its sample.
                echo(kLastKey, (float)e.note());
                if (!(ctx.transport.valid && ctx.transport.playing && ctx.transport.ppqValid)) selectSequence(e.note() - kSeqNote0);
                else if (seqEvN_ < kMaxSeqEv) { seqEv_[seqEvN_].at = std::max(0, std::min(ctx.numFrames - 1, e.sampleOffset)); seqEv_[seqEvN_].seq = e.note() - kSeqNote0; ++seqEvN_; }
            }
            else if (e.isNoteOff() && isSeqNote(e.note())) {}   // ignored
            else if (e.isNoteOn()) { onKey(e.note(), e.velocity()); sent = true; }
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
        // The MPC reports the song position rounded to small ticks (about +-0.0007 quarter from block
        // to block). Following it directly makes the phase step back a little at block starts, and
        // at a step boundary the engine then counts the same boundary twice. So the plugin runs its
        // own steady clock and follows the MPC only slowly (forward only); a jump (start, loop,
        // locate) restarts the engine at the new position.
        const bool jump = !wasPlaying_ || std::fabs(tr.ppqPos - expectPpq_) > 0.05;
        const double base = jump ? tr.ppqPos : expectPpq_ + std::max(0.0, 0.05 * (tr.ppqPos - expectPpq_));
        if (playing && jump)
        {
            engine_.restart();
            anchorQ_ = 0;   // 2.5: a jump goes back to the song grid
            lnAnchor_ = 0;
            for (int l = 0; l < kCcLanes; ++l) { lnN_[l] = kNoStep; lnIdx_[l] = -1; }
        }
        if (!playing && wasPlaying_) { noteOffActive(); restLanes(); sent = true; }
        wasPlaying_ = playing;
        engine_.dir = choiceAt(kDirection);
        engine_.ptnBars = choiceAt(kPtnMode) == 1;
        const double barQ = tr.timeSigDenominator > 0 ? 4.0 * tr.timeSigNumerator / tr.timeSigDenominator : 4.0;
        engine_.barCount = (int)std::floor(tr.ppqPos / barQ);
        const double stepQ = kRateQ[std::max(0, std::min(7, choiceAt(kRate)))];

        if (playing)
        {
            int ev = 0;
            for (int i = 0; i < ctx.numFrames; ++i)
            {
                // 2.5: sequence notes switch at their sample and start the sequence there
                while (ev < seqEvN_ && seqEv_[ev].at <= i)
                {
                    const Sequence& nq = model_->seqs[seqEv_[ev].seq];
                    startSequenceAt(seqEv_[ev].seq, base + i * qps, nq.mode == 1 ? stepQ / ratioOf(nq.ratio, nq.fine) : stepQ);
                    ++ev;
                }
                const Tables& T = tables_[tIdx_];
                const Sequence& ps = model_->seqs[engSeq_];   // 2.8: ratio mode of the playing sequence
                const double sQ = ps.mode == 1 ? stepQ / ratioOf(ps.ratio, ps.fine) : stepQ;
                const double loopQ = sQ * std::max(1, T.steps);
                double ph = std::fmod(base + i * qps - anchorQ_, loopQ) / loopQ;
                if (ph < 0) ph += 1;
                EngineOut o = engine_.tick(ph, true, T, sr);
                if (engine_.wrappedNow && pendSwap_) swapNow();
                curSample_ = sampleCount_ + (uint64_t)i;
                if (tickLanes(base + i * qps)) sent = true;   // 2.8: before the note, so a lock comes after a lane value
                if (toMidi(o)) sent = true;
#ifdef SCALASEQ_TEST
                if (o.step != playStep_) testSteps_.push_back({(int64_t)curSample_, o.step, engSeq_});
#endif
                playStep_ = o.step;
                playPos_ = ph * std::max(1, T.steps);
            }
            expectPpq_ = base + ctx.numFrames * qps;
        }
        else
        {
            playStep_ = -1;
            if (pendSwap_) swapNow();
        }
        seqEvN_ = 0;
        sampleCount_ += (uint64_t)std::max(0, ctx.numFrames);
        if (viewDirty_) { refreshView(); viewDirty_ = false; }
        {
            const float f = (playStep_ - (float)viewStart_) / zoom();
            setDisp(kPlayX, (playStep_ >= 0 && f >= 0 && f < 1) ? padPos(f * kGW, 2, kGW + kPARK) : 1.0f);
            // 2.8: LANE: the shown CC lane's current step (a line, as the SEQ playhead)
            const int ls = choiceAt(kLaneSel);
            const int st = (playing && ls > 0) ? lnIdx_[ls - 1] : -1;
            const float g = (st - (float)viewStart_) / zoom();
            setDisp(kLnPlayX, (st >= 0 && g >= 0 && g < 1) ? padPos(g * kGW, 2, kGW + kPARK) : 1.0f);
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
        if (sent) { wakeMidi_.post(); notifyPending_ = false; }
    }

    void parameterChanged(int index, float) override
    {
        if (index == kScale) { scaleDirty_.store(true); wake_.post(); }
    }

    // the text of a chance lane's Amount (as the Amount knob shows it)
    static std::string amountText(int op, int v)
    {
        char b[32];
        switch (opInfo(op).unit)
        {
            case U_NONE: return "-";
            case U_GATE: std::snprintf(b, sizeof b, "%s st", gateName(v)); break;
            case U_DEG: if (v == 0) return "period"; std::snprintf(b, sizeof b, "%d deg", v); break;
            case U_X: std::snprintf(b, sizeof b, "x%d", v); break;
            case U_REP: std::snprintf(b, sizeof b, "+%d", v); break;
            case U_RND: std::snprintf(b, sizeof b, "+-%d", v); break;
            case U_PCT: std::snprintf(b, sizeof b, "%d%%", v); break;
            case U_TO: std::snprintf(b, sizeof b, "step %d", v); break;
            default: std::snprintf(b, sizeof b, "%d", v); break;
        }
        return b;
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        const int v = (int)std::lround(plain);
        auto degTxt = [&](int rel) {
            const int n = std::max(1, dispSize_.load());
            const int d = ((rel % n) + n) % n, p = (int)std::floor((double)rel / n);
            std::snprintf(b, sizeof b, "%d (%+d)", d, p);
            return std::string(b);
        };
        switch (index)
        {
            case kNoteSel:
                if (tSel_.load() == 0) out = "none";
                else { std::snprintf(b, sizeof b, "%d/%d", tSel_.load(), dispCount_.load()); out = b; }
                return true;
            case kDeg:
            {
                if (tSel_.load() == 0) { out = "-"; return true; }
                const int n = std::max(1, dispSize_.load());
                int list[kPitchN], m = 0;
                for (int d = 0; d < n && m < kPitchN; ++d)
                    if (d >= kMaxMask || dispMask_[d].load(std::memory_order_relaxed)) list[m++] = d;
                if (m == 0) list[m++] = 0;
                std::snprintf(b, sizeof b, "%d", list[std::max(0, std::min(m - 1, v * m / 64))]);
                out = b;
                return true;
            }
            case kDegText: out = tSel_.load() == 0 ? std::string("-") : degTxt(tDeg_.load()); return true;
            case kViewText: std::snprintf(b, sizeof b, "%d-%d", tView0_.load() + 1, tView0_.load() + tZoom_.load()); out = b; return true;
            case kPeriod: case kNudge: std::snprintf(b, sizeof b, v == 0 ? "0" : "%+d", v); out = b; return true;
            // 2.8
            case kLock0: case kLock0 + 1: case kLock0 + 2: case kLock0 + 3:
                if (v < 0) out = "off"; else { std::snprintf(b, sizeof b, "%d", v); out = b; }
                return true;
            case kLockCC0: case kLockCC0 + 1: case kLockCC0 + 2: case kLockCC0 + 3: case kLnCC:
            {
                const int c = ccOfIndex(v);
                if (c == 0) out = "off"; else { std::snprintf(b, sizeof b, "%d", c); out = b; }
                return true;
            }
            case kRFine: case kLnFine:   // the sum of Ratio and Fine
                std::snprintf(b, sizeof b, "%.2f", ratioOf(choiceAt(index == kRFine ? kRRatio : kLnRatio), v));
                out = b;
                return true;
            case kMove: case kLen:
            {
                const float q = 1.0f / kSnapDiv[std::max(0, std::min(5, choiceAt(kSnap)))];
                std::snprintf(b, sizeof b, "%g", std::round(v * q * 1000) / 1000); out = b; return true;
            }
            case kViewLow: out = degTxt(v); return true;
            case kZoom: std::snprintf(b, sizeof b, "%d st", v); out = b; return true;
            case kChanceText0: case kChanceText0 + 1: case kChanceText0 + 2: case kChanceText0 + 3:
            {
                const int l = index - kChanceText0, op = tChOp_[l].load();
                if (tSel_.load() == 0) out = "";
                else if (op == 0) { std::snprintf(b, sizeof b, "%d -", l + 1); out = b; }
                else
                {
                    std::snprintf(b, sizeof b, "%d %s %d%%", l + 1, opInfo(op).name, tChProb_[l].load());
                    out = b;
                    if (opInfo(op).unit != U_NONE) out += " " + amountText(op, tChAmt_[l].load());   // the Amount knob's text
                }
                return true;
            }
            case kGroup: if (v == 0) out = "off"; else { std::snprintf(b, sizeof b, "%d st", v); out = b; } return true;
            case kRoot:
            {
                static const char* nm[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
                std::snprintf(b, sizeof b, "%s%d (%d)", nm[((v % 12) + 12) % 12], v / 12 - 1, v); out = b; return true;
            }
            default: break;
        }
        if (index >= kLane0 && index < kMask0 && (index - kLane0) % 4 == 3)
        {
            out = amountText(laneOp_[(index - kLane0) / 4].load(std::memory_order_relaxed), v);
            return true;
        }
        return false;
    }

    // Project state (format SSQ4, 2.0.0): the 32 sequences, the scale mask and the scale file.
    // Projects saved by 1.x (SSQ1-SSQ3) are not loaded.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(x >> (8 * i))); };
        auto putStr = [&](const std::string& s) { put32((uint32_t)s.size()); out.insert(out.end(), s.begin(), s.end()); };
        out.insert(out.end(), {'S', 'S', 'Q', '5'});   // 2.8: + rate mode, lock CCs, CC lanes, note locks
        {
            std::lock_guard<std::mutex> l(scaleMutex_);
            putStr(scaleName_); putStr(scaleText_);
        }
        put32((uint32_t)edSeq_); put32((uint32_t)sel_); put32((uint32_t)viewStart_); put32((uint32_t)viewLow_);
        std::lock_guard<std::mutex> l(savedMutex_);
        for (int d = 0; d < kMaxMask; ++d) out.push_back(saved_->mask[d] ? 1 : 0);
        for (int s = 0; s < kSeqs; ++s)
        {
            const Sequence& q = saved_->seqs[s];
            put32((uint32_t)q.steps); put32((uint32_t)q.rStart); put32((uint32_t)q.rEnd);
            uint32_t sw; std::memcpy(&sw, &q.swing, 4); put32(sw);
            put32((uint32_t)q.mode); put32((uint32_t)q.ratio); put32((uint32_t)q.fine);
            for (int k = 0; k < 4; ++k) out.push_back(q.lockCC[k]);
            for (const CcLane& L : q.lane)
            {
                out.insert(out.end(), {L.cc, L.steps, L.rate, L.mode, L.ratio, L.dir, L.rest, (uint8_t)L.fine});
                out.insert(out.end(), L.val, L.val + kMaxSteps);
            }
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
        if (size < 4 || (std::memcmp(data, "SSQ4", 4) != 0 && std::memcmp(data, "SSQ5", 4) != 0))
        {
            // a project saved with 1.x: its notes are not loaded, and its parameter values (some ids
            // are the same, with other meanings) are set back to the defaults
            for (int i = 0; i < numParams(); ++i) setNormalised(i, param(i).toNormalised(param(i).def));
            return;
        }
        const bool v5 = data[3] == '5';
        pos = 4;
        std::string name, text;
        if (!getStr(name) || !getStr(text)) return;
        int view[4] = {0, 0, 0, 0};
        for (int& x : view) { uint32_t u; if (!get32(u)) return; x = (int)u; }
        if (pos + kMaxMask > size) return;
        Model& m = *staging_;
        for (int d = 0; d < kMaxMask; ++d) m.mask[d] = data[pos++] != 0;
        for (int s = 0; s < kSeqs; ++s)
        {
            Sequence& q = m.seqs[s];
            uint32_t st, rs, re, sw, cnt;
            if (!get32(st) || !get32(rs) || !get32(re) || !get32(sw)) return;
            Sequence fresh;   // 2.8 settings of SSQ4 projects: defaults
            q.mode = fresh.mode; q.ratio = fresh.ratio; q.fine = fresh.fine;
            for (int k = 0; k < 4; ++k) q.lockCC[k] = 0;
            for (int l = 0; l < kCcLanes; ++l) q.lane[l] = CcLane();
            if (v5)
            {
                uint32_t md, ra, fi;
                if (!get32(md) || !get32(ra) || !get32(fi)) return;
                q.mode = std::max(0, std::min(1, (int)md)); q.ratio = std::max(0, std::min(kNumRatios - 1, (int)ra));
                q.fine = std::max(-10, std::min(10, (int)(int32_t)fi));
                if (pos + 4 + kCcLanes * (8 + kMaxSteps) > size) return;
                for (int k = 0; k < 4; ++k) { const int c = data[pos++]; q.lockCC[k] = (uint8_t)(ccAllowed(c) ? c : 0); }
                for (CcLane& L : q.lane)
                {
                    const uint8_t* b = data + pos;
                    L.cc = (uint8_t)(ccAllowed(b[0]) ? b[0] : 0); L.steps = (uint8_t)std::max(1, std::min(kMaxSteps, (int)b[1]));
                    L.rate = (uint8_t)std::min(kNumMusRates - 1, (int)b[2]); L.mode = (uint8_t)std::min(1, (int)b[3]);
                    L.ratio = (uint8_t)std::min(kNumRatios - 1, (int)b[4]); L.dir = (uint8_t)std::min(2, (int)b[5]);
                    L.rest = (uint8_t)std::min(127, (int)b[6]); L.fine = (int8_t)std::max(-10, std::min(10, (int)(int8_t)b[7]));
                    for (int k = 0; k < kMaxSteps; ++k) L.val[k] = (uint8_t)std::min(127, (int)b[8 + k]);
                    pos += 8 + kMaxSteps;
                }
            }
            if (!get32(cnt)) return;
            const size_t nsz = v5 ? sizeof(Note) : sizeof(NoteV4);
            if (cnt > (uint32_t)kMaxNotes || pos + nsz * cnt > size) return;
            q.steps = std::max(1, std::min(kMaxSteps, (int)st));
            q.rStart = (int)rs; q.rEnd = (int)re;
            std::memcpy(&q.swing, &sw, 4);
            q.count = (int)cnt;
            if (v5) std::memcpy(q.notes, data + pos, sizeof(Note) * cnt);
            else
                for (uint32_t i = 0; i < cnt; ++i)   // SSQ4 notes: no locks
                {
                    NoteV4 o;
                    std::memcpy(&o, data + pos + i * sizeof(NoteV4), sizeof(NoteV4));
                    Note& nn = q.notes[i];
                    nn = Note();
                    nn.st = o.st; nn.len = o.len; nn.pitch = o.pitch; nn.r = o.r; nn.fd = o.fd; nn.v = o.v; nn.gl = o.gl; nn.d = o.d;
                    std::memcpy(nn.ch, o.ch, sizeof nn.ch);
                }
            for (uint32_t i = 0; i < cnt; ++i) for (int8_t& lk : q.notes[i].lock) lk = std::max<int8_t>(-1, lk);
            pos += nsz * cnt;
            q.fitToSteps();
        }
        {
            std::lock_guard<std::mutex> l(scaleMutex_);
            embedName_ = name; embedText_ = text;
            appliedScale_ = choiceAt(kScale);   // keep the embedded scale, not what the index points to now
        }
        for (int k = 0; k < 4; ++k) stagingView_[k] = view[k];
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
        info.version = 2800;
        info.isInstrument = true;
        info.numInputs = 0;
        info.numOutputs = 2;
        info.tailSamples = 1;
        return info;
    }

    // ===== audio thread: model and edits ==========================================================
    Sequence& ed() { return model_->seqs[edSeq_]; }
    Note* selNote() { Sequence& s = ed(); return (sel_ >= 0 && sel_ < s.count) ? &s.notes[sel_] : nullptr; }
    // A choice parameter as the MPC shows it. A Q-Link can leave a value between two choices
    // (measured: Entry Len 0.6171875 = choice 4.94); the MPC shows the lower choice, while the
    // framework's getInt rounds to the nearest. Reading it rounded down keeps sound and screen equal.
    int choiceAt(int i) const
    {
        const int n = (int)param(i).choices.size();
        if (n < 2) return 0;
        return std::max(0, std::min(n - 1, (int)std::floor(getNormalised(i) * (float)(n - 1) + 1e-4f)));
    }
    float plainOf(int i, float norm) const
    {
        const int n = (int)param(i).choices.size();
        if (n >= 2) return (float)std::max(0, std::min(n - 1, (int)std::floor(norm * (float)(n - 1) + 1e-4f)));
        return param(i).toPlain(norm);
    }
    float snapQ() const { return 1.0f / kSnapDiv[std::max(0, std::min(5, choiceAt(kSnap)))]; }
    float snapTo(float x) const { const float q = snapQ(); return std::round(x / q) * q; }
    float zoom() const { return (float)std::max(1, std::min(kMaxSteps, getInt(kZoom))); }

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
    // the edited sequence (the Sequence knob); the playing one follows at the loop end (swapNow)
    void setEdSeq(int q)
    {
        if (q == edSeq_) return;
        edSeq_ = q;
        sel_ = 0;
        if (edSeq_ != engSeq_) { pendSwap_ = true; buildTables(ed(), tuning_, tables_[1 - tIdx_]); }
        else pendSwap_ = false;
        echoSequence();
    }
    static bool isSeqNote(int n) { return n >= kSeqNote0 && n < kSeqNote0 + kSeqs; }
    // 2.5: a sequence note while stopped: the screen and the Sequence knob switch now
    void selectSequence(int q)
    {
        setEdSeq(q);
        echo(kSequence, (float)(edSeq_ + 1));
    }
    // 2.5: a sequence note while playing, at the song position ppq: switch now and start the
    // sequence at its Range Start (the engine starts at the step the phase points to)
    void startSequenceAt(int q, double ppq, double stepQ)
    {
        selectSequence(q);
        if (pendSwap_) swapNow(); else engine_.restart();
        const Tables& T = tables_[tIdx_];
        const int n = std::max(1, std::min(kMaxSteps, T.steps));
        const int lo = std::max(0, std::min(n - 1, T.rs));
        // the unswung position where step lo begins (the engine swings pairs of steps)
        const double msw = std::max(50.0f, std::min(75.0f, T.swing)) / 100.0;
        const double pos0 = ((lo & 1) && lo < n) ? (lo - 1) + 2 * msw : (double)lo;
        anchorQ_ = ppq - (pos0 + 1e-6) * stepQ;
        lnAnchor_ = ppq;   // 2.8: the lanes start at their step 1
        for (int l = 0; l < kCcLanes; ++l) { lnN_[l] = kNoStep; lnIdx_[l] = -1; }
    }
    void swapNow()
    {
        if (!pendSwap_) return;
        tIdx_ = 1 - tIdx_;
        engSeq_ = edSeq_;
        pendSwap_ = false;
        engine_.restart();
    }

    // knob changes (compared with what we wrote ourselves) and the touch surfaces
    void pollEdits()
    {
        for (int i = kFirstEdit; i <= kLastEdit; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            onEdit(i, plainOf(i, v));
            snapChoice(i, v);
            closeList(i);
        }
        for (int i : {(int)kClearNote, (int)kClearAll})
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            onEdit(i, plainOf(i, v));
        }
        for (int i = kRMode; i <= kLnRest; ++i)   // 2.8: RATE, LOCK and LANE settings
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            onEdit(i, plainOf(i, v));
            snapChoice(i, v);
            closeList(i);
        }
        for (int i = kNotePrev; i <= kLenNext; ++i)   // 2.7: step buttons
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            if (v >= 0.5f) { stepButton(i); echo(i, 0); }
        }
        // 2.8: COPY, PASTE, CLEAR of the edited sequence
        for (int i = kSeqCopy; i <= kSeqClear; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            seqButton(i, v >= 0.5f);
        }
        for (int i = 0; i < 2; ++i)
            if (seqArmed_[i] && msNow() - seqArmMs_[i] > 3000) { seqArmed_[i] = false; echo(kSeqPaste + i, 0); }
        if (pastePending_) doPaste();
        // 2.8: SEND: on a touch the CC goes out at 64 on First Ch; the button stays lit for 150 ms
        for (int i = kSend0; i < kSend0 + kNumSends; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            if (v >= 0.5f) sendLearn(i - kSend0);                       // tap: start (lit)
            else if (burstSend_ == i - kSend0) stopBurst(false);         // tap again: stop
        }
        tickBurst();
        if (clearArmed_ && msNow() - clearArmMs_ > 3000) { clearArmed_ = false; echo(kClearAll, 0); }   // not confirmed: disarm
        for (int i = kLane0; i < kMask0 + 31; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            onEdit(i, plainOf(i, v));
            snapChoice(i, v);
            closeList(i);
        }
        pollTouch(0, kRollTX); pollTouch(1, kRulTX); pollTouch(2, kLabTX); pollTouch(3, kVelTX); pollTouch(4, kFreeTX);
    }

    // the degrees of the period that are in the scale mask (the Degree knob is spread over them)
    int maskedDegrees(int* out) const
    {
        int m = 0;
        for (int d = 0; d < tuning_.size() && m < kPitchN; ++d)
            if (d >= kMaxMask || tuning_.mask[d]) out[m++] = d;
        if (m == 0) out[m++] = 0;
        return m;
    }
    float degreeKnob(const Note& n) const
    {
        int list[kPitchN];
        const int m = maskedDegrees(list), d = tuning_.degreeOf(n.pitch);
        int k = 0;
        while (k < m - 1 && list[k] < d) ++k;
        return (float)std::min(63, (int)std::floor((k + 0.5f) / m * 64));   // the knob value (0-63) of degree k
    }

    // A value text that depends on other state: tell the host to redraw it.
    void textNotify(int i) { notify_[i].store(true, std::memory_order_release); notifyPending_ = true; }

    // 2.6 (as percSeq 1.1): a Q-Link writes values between two choices (measured on the MPC One G2 with
    // percSeq: steps of 1/128), and the MPC then showed a name one choice away from the one the plugin
    // used. After an edit, write back the exact value of the choice taken.
    void snapChoice(int i, float before)
    {
        const int n = (int)param(i).choices.size();
        if (n < 2 || getNormalised(i) != before) return;
        echo(i, (float)choiceAt(i));
    }
    // 2.6: a pick in a drawn list (or a Q-Link turn of its parameter) closes the open list
    static bool isListParam(int i)
    {
        if (i == kEntryLen || i == kRate || i == kScale || i == kRRatio || i == kLnRate || i == kLnRatio) return true;
        return i >= kLane0 && i < kMask0 && ((i - kLane0) % 4 == 0 || (i - kLane0) % 4 == 2);
    }
    void closeList(int i) { if (isListParam(i) && getInt(kListOpen) != 0) echo(kListOpen, 0); }

    void updateTexts()
    {
        Sequence& s = ed();
        Note* n = selNote();
        tSel_.store(n ? sel_ + 1 : 0);
        for (int l = 0; l < kLanes; ++l)
        {
            tChOp_[l].store(n ? n->ch[l].op : 0);
            tChProb_[l].store(n ? (int)std::lround(n->ch[l].prob * 100) : 0);
            // the amount the engine uses (0 = the op's default; clamped to the sequence), as opValue
            int eff = 0;
            if (n)
            {
                const Cell& c = n->ch[l];
                const Unit u = opInfo(c.op).unit;
                if (u == U_GATE || (u == U_DEG && c.amt == 0)) eff = c.amt;   // gate: an index; degrees 0 = "period"
                else if (u != U_NONE) eff = (int)std::lround(opValue(c, s, tuning_, sel_));
            }
            tChAmt_[l].store(eff);
        }
        tDeg_.store(n ? n->pitch - kCenter : 0);
        tView0_.store(viewStart_);
        tZoom_.store((int)zoom());
        dispCount_.store(s.count);
        for (int j : {kNoteSel, kDeg, kDegText, kViewText}) textNotify(j);
        for (int l = 0; l < kLanes; ++l) textNotify(kChanceText0 + l);
    }

    // Write a parameter value now (audio thread: an atomic store). The worker later tells the
    // host to redraw it.
    void setRT(int i, float norm)
    {
        if (getNormalised(i) == norm) return;
        setNormalised(i, norm);
        lastSeen_[i] = norm;
        notify_[i].store(true, std::memory_order_release);
        notifyPending_ = true;
    }
    void echo(int i, float plain) { setRT(i, param(i).toNormalised(plain)); }
    // Display parameters: written when the computed value changes (values restored from a
    // project stay until the display really changes).
    void setDisp(int i, float norm)
    {
        // also repairs values moved from outside, except right after a project load (the restored
        // values stay until the first edit; the host simulator writes random display values)
        if (computed_[i] == norm && (primeOnly_ || trustRestored_ || getNormalised(i) == norm)) return;
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

    // the knobs show the selected note
    void echoNote()
    {
        viewDirty_ = true;
        Sequence& s = ed();
        dispCount_.store(s.count);
        Note* n = selNote();
        if (!n) { for (int l = 0; l < kLanes; ++l) laneOp_[l].store(0); for (int k = 0; k < 4; ++k) echo(kLock0 + k, -1); return; }
        for (int k = 0; k < 4; ++k) echo(kLock0 + k, (float)n->lock[k]);   // 2.8
        echo(kVel, n->v * 100); echo(kGlide, n->gl); echo(kRatch, n->r); echo(kFade, n->fd); echo(kDuty, n->d);
        for (int l = 0; l < kLanes; ++l)
        {
            const Cell& c = n->ch[l];
            laneOp_[l].store(c.op);
            echo(kLane0 + l * 4, choiceOfOp(c.op)); echo(kLane0 + l * 4 + 1, c.prob * 100);
            echo(kLane0 + l * 4 + 2, c.ptn); echo(kLane0 + l * 4 + 3, c.amt);
        }
        echoKnobs();
    }
    void echoKnobs()
    {
        echo(kSequence, (float)(edSeq_ + 1));
        echo(kViewLow, (float)std::max(-32, std::min(31, viewLow_)));
        echo(kViewStart, (float)(viewStart_ + 1));
        Note* n = selNote();
        if (!n) return;
        const float q = snapQ();
        const float grid = std::round(n->st / q) * q;
        echo(kNoteSel, (float)(sel_ + 1));
        echo(kDeg, degreeKnob(*n));
        echo(kPeriod, (float)tuning_.periodOf(n->pitch));
        echo(kMove, (float)std::lround(grid / q));
        echo(kLen, (float)std::max(1L, std::lround(n->len / q)));
        echo(kNudge, (float)std::lround((n->st - grid) * 96.0f));
    }

    // keep the selected note on screen
    void keepVisible()
    {
        Note* n = selNote();
        if (!n) return;
        const float z = zoom();
        if (n->st < viewStart_ - kEps || n->st >= viewStart_ + z - kEps)
        {
            viewStart_ = std::max(0, std::min(ed().steps - 1, (int)std::floor(n->st) - (int)(z / 4)));
            viewDirty_ = true;
        }
        const int rel = n->pitch - kCenter;
        if (rel < viewLow_ || rel >= viewLow_ + kRows)
        {
            viewLow_ = std::max(-kCenter, std::min(kPitchN - kRows - kCenter, rel - kRows / 2));
            viewDirty_ = true;
        }
    }

    // 2.8: the LANE settings show the shown CC lane of the edited sequence
    void echoLane()
    {
        const int ls = choiceAt(kLaneSel);
        if (ls <= 0) return;
        const CcLane& L = ed().lane[ls - 1];
        echo(kLnCC, (float)indexOfCC(L.cc)); echo(kLnSteps, (float)L.steps); echo(kLnRate, (float)L.rate);
        echo(kLnMode, (float)L.mode); echo(kLnRatio, (float)L.ratio); echo(kLnFine, (float)L.fine);
        echo(kLnDir, (float)L.dir); echo(kLnRest, (float)L.rest);
        textNotify(kLnFine);
    }
    // 2.8: COPY takes the edited sequence (all its data); PASTE (armed, then a 2nd touch within 3 s)
    // puts the copy into the edited sequence; CLEAR (the same way) sets it back to a new sequence.
    void seqButton(int i, bool on)
    {
        if (i == kSeqCopy)
        {
            if (!on) return;
            if (g_clipMutex.try_lock()) { g_clip = ed(); g_clipHas = true; g_clipMutex.unlock(); }
            echo(kSeqCopy, 0);
            return;
        }
        const int k = i - kSeqPaste;
        if (on) { seqArmed_[k] = true; seqArmMs_[k] = msNow(); return; }   // armed: the button stays lit
        if (!seqArmed_[k]) return;
        seqArmed_[k] = false;
        if (i == kSeqPaste) { pastePending_ = true; doPaste(); }
        else { ed() = Sequence(); afterSeqChange(); }
    }
    // 2.8: SEND k: 0-3 Lock CC 1-4 of the edited sequence, 4 the shown lane (nothing while off)
    void sendLearn(int k)
    {
        const Sequence& q = ed();
        int cc = 0;
        if (k < 4) cc = q.lockCC[k];
        else { const int ls = choiceAt(kLaneSel); cc = ls > 0 ? q.lane[ls - 1].cc : 0; }
        if (cc <= 0) { echo(kSend0 + k, 0); return; }   // the CC is off: nothing
        // measured 2026-11-22 (percSeq): MIDI Learn ignores a single CC, learns a moving CC, and turns off
        // when its page is left; so SEND keeps sending until it is touched again (or 15 s), then sends 64
        if (burstSend_ >= 0) stopBurst(true);
        burstSend_ = k; burstCC_ = cc; burstStep_ = 0; burstMs_ = msNow();
        tickBurst();
    }
    void burstOut(int v)
    {
        sendCC(burstCC_, v);
        sendPending_ = true;
    }
    // SEND zigzag: 0 and 127 in turn every 250 ms; ends after 15 s or on a second touch with 64
    void tickBurst()
    {
        if (burstSend_ < 0) return;
        if (msNow() - burstMs_ >= 15000.0) { stopBurst(true); return; }
        while (msNow() >= burstMs_ + 250.0 * burstStep_)
        {
            burstOut((burstStep_ & 1) ? 127 : 0);
            ++burstStep_;
        }
    }
    // end the zigzag at 64; setOff: also switch the SEND button off (time limit, or another SEND started)
    void stopBurst(bool setOff)
    {
        if (burstSend_ < 0) return;
        burstOut(64);
        if (setOff) echo(kSend0 + burstSend_, 0);
        burstSend_ = -1;
    }
    void doPaste()
    {
        if (!g_clipMutex.try_lock()) return;   // another instance is copying: try again in the next block
        if (g_clipHas) ed() = g_clip;
        g_clipMutex.unlock();
        pastePending_ = false;
        afterSeqChange();
    }
    void afterSeqChange()
    {
        sel_ = 0;
        modelDirty_ = true;
        savedDirty_ = true;
        echoSequence();
    }
    void echoSequence()
    {
        Sequence& s = ed();
        echo(kSteps, s.steps); echo(kRangeStart, s.rStart + 1); echo(kRangeEnd, s.rEnd + 1); echo(kSwing, s.swing);
        echo(kRMode, (float)s.mode); echo(kRRatio, (float)s.ratio); echo(kRFine, (float)s.fine); textNotify(kRFine);   // 2.8
        for (int k = 0; k < 4; ++k) echo(kLockCC0 + k, (float)indexOfCC(s.lockCC[k]));
        echoLane();
        echo(kCursor, cursor_);
        for (int d = 0; d < 31; ++d) echo(kMask0 + d, tuning_.mask[d] ? 1.0f : 0.0f);
        sel_ = std::min(sel_, s.count - 1);
        if (sel_ < 0 && s.count > 0) sel_ = 0;
        viewStart_ = std::min(viewStart_, s.steps - 1);
        echoNote();
        keepVisible();
    }

    // move note i to start st (keeps the list sorted); returns its new index
    // 2.7: the < > step buttons. Each stops at the ends. Move skips places where the note does not fit
    // and goes to the next free one in that direction.
    void stepButton(int i)
    {
        Sequence& s = ed();
        Note* n = selNote();
        const int dir = (i == kNoteNext || i == kMoveNext || i == kLenNext) ? 1 : -1;
        switch (i)
        {
            case kNotePrev: case kNoteNext:
                if (s.count > 0) { sel_ = std::max(0, std::min(s.count - 1, sel_ + dir)); echoNote(); keepVisible(); }
                break;
            case kMovePrev: case kMoveNext:
            {
                if (!n) break;
                const float q = snapQ();
                const float grid = std::round(n->st / q) * q, off = n->st - grid;
                const long b = std::lround(grid / q);
                for (long c = b + dir; ; c += dir)
                {
                    const float st = c * q + off;
                    if (st < -kEps || st + n->len > s.steps + kEps) break;   // the sequence start or end
                    if (s.fitsPitch(std::max(0.0f, st), n->len, n->pitch, sel_)) { onEdit(kMove, (float)c); break; }
                }
                break;
            }
            case kLenPrev: case kLenNext:
                if (n) onEdit(kLen, (float)(std::max(1L, std::lround(n->len / snapQ())) + dir));
                break;
            default: break;
        }
    }

    int moveNote(int i, float st)
    {
        Sequence& s = ed();
        Note moved = s.notes[i];
        moved.st = st;
        s.remove(i);
        return s.insert(moved);
    }

    void onEdit(int i, float v)
    {
        trustRestored_ = false;
        Sequence& s = ed();
        Note* n = selNote();
        const int iv = (int)std::lround(v);
        switch (i)
        {
            case kSequence: setEdSeq(std::max(0, std::min(kSeqs - 1, iv - 1))); break;
            // 2.8: rate mode, locks, lock CCs and lanes of the edited sequence
            case kRMode: s.mode = std::max(0, std::min(1, iv)); echo(kRMode, (float)s.mode); modelDirty_ = true; break;
            case kRRatio: s.ratio = std::max(0, std::min(kNumRatios - 1, iv)); echo(kRRatio, (float)s.ratio); modelDirty_ = true; textNotify(kRFine); break;
            case kRFine: s.fine = std::max(-10, std::min(10, iv)); modelDirty_ = true; textNotify(kRFine); break;
            case kLock0: case kLock0 + 1: case kLock0 + 2: case kLock0 + 3:
                if (n) { n->lock[i - kLock0] = (int8_t)std::max(-1, std::min(127, iv)); modelDirty_ = true; }
                else echo(i, -1);
                break;
            case kLockCC0: case kLockCC0 + 1: case kLockCC0 + 2: case kLockCC0 + 3:
                s.lockCC[i - kLockCC0] = (uint8_t)ccOfIndex(iv); modelDirty_ = true;
                break;
            case kLaneSel: echo(kLaneSel, (float)choiceAt(kLaneSel)); echoLane(); viewDirty_ = true; break;
            case kLnCC: case kLnSteps: case kLnRate: case kLnMode: case kLnRatio: case kLnFine: case kLnDir: case kLnRest:
            {
                const int ls = choiceAt(kLaneSel);
                if (ls <= 0) break;   // VEL has no settings
                CcLane& L = s.lane[ls - 1];
                switch (i)
                {
                    case kLnCC: L.cc = (uint8_t)ccOfIndex(iv); break;
                    case kLnSteps: L.steps = (uint8_t)std::max(1, std::min(kMaxSteps, iv)); viewDirty_ = true; break;
                    case kLnRate: L.rate = (uint8_t)std::max(0, std::min(kNumMusRates - 1, iv)); break;
                    case kLnMode: L.mode = (uint8_t)std::max(0, std::min(1, iv)); break;
                    case kLnRatio: L.ratio = (uint8_t)std::max(0, std::min(kNumRatios - 1, iv)); textNotify(kLnFine); break;
                    case kLnFine: L.fine = (int8_t)std::max(-10, std::min(10, iv)); textNotify(kLnFine); break;
                    case kLnDir: L.dir = (uint8_t)std::max(0, std::min(2, iv)); break;
                    case kLnRest: L.rest = (uint8_t)std::max(0, std::min(127, iv)); break;
                }
                modelDirty_ = true;
                break;
            }
            case kSteps:
                s.steps = std::max(1, std::min(kMaxSteps, iv));
                s.fitToSteps();
                cursor_ = std::min(cursor_, (float)s.steps - snapQ());
                viewStart_ = std::min(viewStart_, s.steps - 1);
                echo(kRangeStart, s.rStart + 1); echo(kRangeEnd, s.rEnd + 1);
                modelDirty_ = true; echoNote();
                break;
            case kRangeStart: s.rStart = std::max(0, std::min(s.steps - 1, iv - 1)); s.rEnd = std::max(s.rEnd, s.rStart);
                echo(kRangeEnd, s.rEnd + 1); modelDirty_ = true; break;
            case kRangeEnd: s.rEnd = std::max(0, std::min(s.steps - 1, iv - 1)); s.rStart = std::min(s.rStart, s.rEnd);
                echo(kRangeStart, s.rStart + 1); modelDirty_ = true; break;
            case kSwing: s.swing = std::max(50.0f, std::min(75.0f, v)); modelDirty_ = true; break;
            case kRoot: case kRootTune: applyRoot(); modelDirty_ = true; break;
            case kSnap: echoNote(); break;
            case kPadNote:
                // 2.5: jump over 45-91 (the sequence notes) in the direction of the turn
                if (!padNoteOk(iv)) { const int nv = iv > padNote_ ? kPadHighMin : kPadLowMax; echo(kPadNote, (float)nv); padNote_ = nv; }
                else padNote_ = iv;
                viewDirty_ = true;
                break;
            case kGroup: case kStepEntry: viewDirty_ = true; break;
            case kZoom: keepVisible(); viewDirty_ = true; break;
            case kNoteSel:
                if (s.count > 0) { sel_ = std::max(0, std::min(s.count - 1, iv - 1)); echoNote(); keepVisible(); }
                break;
            case kDeg:
                if (n)
                {
                    int list[kPitchN];
                    const int m = maskedDegrees(list);
                    const int d = list[std::max(0, std::min(m - 1, iv * m / 64))];
                    const int p = std::max(0, std::min(kPitchN - 1, kCenter + tuning_.periodOf(n->pitch) * tuning_.size() + d));
                    if (p != n->pitch && s.fitsPitch(n->st, n->len, p, sel_)) { n->pitch = (int16_t)p; modelDirty_ = true; keepVisible(); }
                    else echoKnobs();
                }
                break;
            case kPeriod:
                if (n)
                {
                    const int p = std::max(0, std::min(kPitchN - 1, kCenter + iv * tuning_.size() + tuning_.degreeOf(n->pitch)));
                    if (p != n->pitch && s.fitsPitch(n->st, n->len, p, sel_)) { n->pitch = (int16_t)p; modelDirty_ = true; keepVisible(); }
                    else echoKnobs();
                }
                break;
            case kMove:
                if (n)
                {
                    const float q = snapQ();
                    const float grid = std::round(n->st / q) * q, off = n->st - grid;
                    const float st = std::max(0.0f, std::min((float)s.steps - n->len, iv * q + off));   // end stops at the sequence end
                    if (std::fabs(st - n->st) > kEps && s.fitsPitch(st, n->len, n->pitch, sel_))
                    {
                        sel_ = moveNote(sel_, st);
                        modelDirty_ = true;
                    }
                    echoNote();
                    keepVisible();
                }
                break;
            case kLen:
                if (n)
                {
                    const float q = snapQ();
                    const float len = std::max(kMinLen, std::min(iv * q, s.maxLenPitch(n->st, n->pitch, sel_)));
                    if (std::fabs(len - n->len) > kEps) { n->len = len; modelDirty_ = true; }
                    echo(kLen, (float)std::max(1L, std::lround(n->len / q)));
                }
                break;
            case kNudge:
                if (n)
                {
                    const float q = snapQ();
                    const int half = std::max(0, std::min(31, (int)std::lround(q * 48.0f) - 1));   // inside half a snap step
                    const int nd = std::max(-half, std::min(half, iv));
                    const float grid = std::round(n->st / q) * q;
                    const float st = std::max(0.0f, grid + nd / 96.0f);
                    if (std::fabs(st - n->st) > kEps && s.fitsPitch(st, n->len, n->pitch, sel_)) { sel_ = moveNote(sel_, st); modelDirty_ = true; }
                    echo(kNudge, (float)std::lround((ed().notes[sel_].st - grid) * 96.0f));
                }
                break;
            case kViewLow: viewLow_ = std::max(-kCenter, std::min(kPitchN - kRows - kCenter, iv)); viewDirty_ = true; break;
            case kViewStart: viewStart_ = std::max(0, std::min(s.steps - 1, iv - 1)); viewDirty_ = true; break;
            case kVel: if (n) { n->v = v / 100; modelDirty_ = true; } break;
            case kGlide: if (n) { n->gl = v; modelDirty_ = true; } break;
            case kRatch: if (n) { n->r = (int8_t)iv; modelDirty_ = true; } break;
            case kFade: if (n) { n->fd = (int8_t)iv; modelDirty_ = true; } break;
            case kDuty: if (n) { n->d = v; modelDirty_ = true; } break;
            case kCursor: cursor_ = std::max(0.0f, std::min((float)s.steps - snapQ(), snapTo(v))); viewDirty_ = true; break;
            case kClearNote:
                if (v >= 0.5f)
                {
                    if (n) { for (Cell& c : n->ch) c = Cell(); modelDirty_ = true; }
                    echo(kClearNote, 0);
                    echoNote();
                }
                break;
            case kClearAll:
                if (v >= 0.5f) { clearArmed_ = true; clearArmMs_ = msNow(); }   // armed: the switch stays lit
                else if (clearArmed_)
                {
                    clearArmed_ = false;   // second touch within 3 s: clear every chance op of the sequence
                    for (int k = 0; k < s.count; ++k) for (Cell& c : s.notes[k].ch) c = Cell();
                    modelDirty_ = true;
                    echoNote();
                }
                break;
            case kDelete:
                if (v >= 0.5f)
                {
                    deleteSelected();
                    echo(kDelete, 0);
                }
                break;
            default:
                if (i >= kLane0 && i < kMask0 && n)
                {
                    const int l = (i - kLane0) / 4, f = (i - kLane0) % 4;
                    Cell& c = n->ch[l];
                    if (f == 0)
                    {
                        c.op = (uint8_t)opOfChoice(iv);
                        c.amt = (uint8_t)opInfo(c.op).def;
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
                    echoKnobs();
                    modelDirty_ = true;
                }
                break;
        }
    }

    void deleteSelected()
    {
        Sequence& s = ed();
        if (sel_ < 0 || sel_ >= s.count) return;
        s.remove(sel_);
        sel_ = std::min(sel_, s.count - 1);
        modelDirty_ = true;
        echoNote();
    }

    // a short preview of the selected note (touch and knob edits)
    void preview()
    {
        Note* n = selNote();
        if (!n) return;
        if (previewCh_ >= 0) stopNote(previewCh_);
        previewCh_ = startNote((float)tuning_.midiOf(n->pitch), n->v);
        previewOffMs_ = msNow() + 200;
        previewSent_ = true;
    }

    // ===== audio thread: touch =====================================================================
    double msNow() const { return sampleCount_ * 1000.0 / std::max(1.0, sampleRate()); }

    void pollTouch(int t, int px)
    {
        const float x = getNormalised(px), y = getNormalised(px + 1);
        if (x == lastSeen_[px] && y == lastSeen_[px + 1]) return;
        lastSeen_[px] = x; lastSeen_[px + 1] = y;
        Touch& tc = touch_[t];
        const bool rest = (x == 0.0f && y == 0.0f), half = (x == 0.0f || y == 0.0f);
        if (!tc.down)
        {
            if (rest || half) return;   // the second value of a press has not arrived yet
            tc = Touch();
            tc.down = true; tc.x = tc.x0 = x; tc.y = tc.y0 = y; tc.t0 = msNow();
            onPress(t, tc);
            return;
        }
        if (rest)
        {
            tc.down = false;
            onRelease(t, tc);
            return;
        }
        if (half) return;   // the lift has begun
        tc.x = x; tc.y = y;
        onDrag(t, tc);
    }

    // pad value -> pixels (the touch puck is 2 px: centre = 1 + v * (pad - 2))
    static float padPx(float v, float pad) { return 1.0f + v * (pad - 2.0f); }
    float rollX(const Touch& t, bool start = false) const { return padPx(start ? t.x0 : t.x, kGW); }
    float rollYUp(const Touch& t, bool start = false) const { return padPx(start ? t.y0 : t.y, kGH); }   // from the bottom
    float posOfPx(float px) const { return viewStart_ + px / kGW * zoom(); }
    float pxOfPos(float pos) const { return (pos - viewStart_) / zoom() * kGW; }
    bool freeHeld() const { return touch_[4].down; }
    // touch edits snap to the finest lines on screen: the Snap lines, or whole steps when the Snap
    // lines are hidden (closer than kMinLinePx)
    float touchQ() const { const float q = snapQ(); return q * (kGW / zoom()) >= kMinLinePx ? q : 1.0f; }
    float snapOrFree(float pos) const { const float q = touchQ(); return freeHeld() ? std::round(pos * 96.0f) / 96.0f : std::round(pos / q) * q; }
    float minLen() const { return freeHeld() ? kMinLen : touchQ(); }
    int rowPitch(float yUp) const { return std::max(0, std::min(kPitchN - 1, kCenter + viewLow_ + rowAtY(yUp / kGH))); }

    void onPress(int t, Touch& tc)
    {
        trustRestored_ = false;
        Sequence& s = ed();
        if (t == 4) { setDisp(kFreeLit, xPark(0, kFreeW, true)); return; }
        if (t == 1) { gView0_ = viewStart_; gZoom0_ = (int)zoom(); gAxis_ = 0; return; }
        if (t == 2) { gLow0_ = viewLow_; return; }
        if (t == 3 && choiceAt(kLaneSel) > 0) { gLnLast_ = -1; drawLane(tc); return; }   // 2.8: CC lane
        if (t == 3)
        {
            // the velocity bar nearest to the finger (within 14 px)
            const float fx = padPx(tc.x, kGW);
            int best = -1;
            float bd = 14;
            for (int i = 0; i < s.count; ++i)
            {
                const float d = std::fabs(pxOfPos(s.notes[i].st) + kVelW * 0.5f - fx);
                if (s.notes[i].st >= viewStart_ - kEps && s.notes[i].st < viewStart_ + zoom() - kEps && d < bd) { bd = d; best = i; }
            }
            gVelNote_ = best;
            if (best >= 0) { sel_ = best; setVelFromTouch(tc); echoNote(); }
            return;
        }
        // the roll
        const float fx = rollX(tc), pos = posOfPx(fx);
        const int pitch = rowPitch(rollYUp(tc));
        int hit = -1;
        for (int i = s.count - 1; i >= 0; --i)   // the later note is drawn on top
        {
            const Note& n = s.notes[i];
            if (n.pitch != pitch) continue;
            const float a = pxOfPos(n.st), b = pxOfPos(n.st + n.len);
            if (fx >= a - 4 && fx < std::max(b, a + 12)) { hit = i; break; }
        }
        gDelete_ = false;
        if (hit >= 0)
        {
            const Note& n = s.notes[hit];
            const float a = pxOfPos(n.st), b = pxOfPos(n.st + n.len);
            const float edge = std::max(12.0f, std::min(24.0f, (b - a) / 3));
            gMode_ = (fx >= b - edge && b - a >= 12) ? 3 : 2;   // 3 length, 2 move
            if (hit == lastTapNote_ && msNow() - lastTapMs_ <= kDoubleTapMs) gDelete_ = true;
            sel_ = hit;
            gOrig_ = n;
            echoNote();
            preview();
            return;
        }
        // empty: a new note on the nearest snap line (or exactly, while FREE is held)
        float st = std::max(0.0f, snapOrFree(pos));
        st = std::max(st, s.minStartPitch(st + kEps, pitch, -1));   // not inside a same-pitch note
        if (st >= s.steps - kEps || s.maxLenPitch(st, pitch, -1) < kMinLen) { gMode_ = 0; return; }
        const float elen = kEntryLens[std::max(0, std::min(8, choiceAt(kEntryLen)))];
        Note nt;
        const float room = s.maxLenPitch(st, pitch, -1);
        nt.st = st; nt.len = std::min(minLen(), room); nt.pitch = (int16_t)pitch;   // a tap gets Entry Len at release
        gEntryLen_ = std::min(elen, room);
        const int idx = s.insert(nt);
        if (idx < 0) { gMode_ = 0; return; }
        sel_ = idx;
        gMode_ = 1;
        gAnchor_ = st;
        gOrig_ = nt;
        modelDirty_ = true;
        echoNote();
        preview();
    }

    void onDrag(int t, Touch& tc)
    {
        Sequence& s = ed();
        if (t == 4) return;
        if (t == 1)
        {
            const float dx = (tc.x - tc.x0) * (kGW - 2), dyDown = -(tc.y - tc.y0) * (kRulerPadH - 2);
            if (gAxis_ == 0)
            {
                if (std::fabs(dx) < kDragPx && std::fabs(dyDown) < kDragPx) return;
                gAxis_ = std::fabs(dx) >= std::fabs(dyDown) ? 1 : 2;
            }
            if (gAxis_ == 1)
            {
                const float stepPx = kGW / gZoom0_;
                const int vs = std::max(0, std::min(s.steps - 1, gView0_ - (int)std::lround(dx / stepPx)));
                if (vs != viewStart_) { viewStart_ = vs; viewDirty_ = true; echo(kViewStart, (float)(vs + 1)); }
            }
            else
            {
                const int z = std::max(1, std::min(kMaxSteps, gZoom0_ + (int)std::lround(dyDown / kZoomPx)));
                if (z != (int)zoom())
                {
                    const float fx0 = padPx(tc.x0, kGW);
                    const float anchor = gView0_ + fx0 / kGW * gZoom0_;   // the step under the finger stays
                    echo(kZoom, (float)z);
                    viewStart_ = std::max(0, std::min(s.steps - 1, (int)std::lround(anchor - fx0 / kGW * z)));
                    echo(kViewStart, (float)(viewStart_ + 1));
                    viewDirty_ = true;
                }
            }
            return;
        }
        if (t == 2)
        {
            const float dyDown = -(tc.y - tc.y0) * (kGH - 2);
            const int vl = std::max(-kCenter, std::min(kPitchN - kRows - kCenter, gLow0_ + (int)std::lround(dyDown / (kGH / kRows))));
            if (vl != viewLow_) { viewLow_ = vl; viewDirty_ = true; echo(kViewLow, (float)std::max(-32, std::min(31, vl))); }
            return;
        }
        if (t == 3) { if (choiceAt(kLaneSel) > 0) drawLane(tc); else if (gVelNote_ >= 0) setVelFromTouch(tc); return; }
        // the roll
        if (gMode_ == 0 || sel_ < 0 || sel_ >= s.count) return;
        if (!tc.dragged)
        {
            const float dx = (tc.x - tc.x0) * (kGW - 2), dy = (tc.y - tc.y0) * (kGH - 2);
            if (std::fabs(dx) < kDragPx && std::fabs(dy) < kDragPx) return;
            tc.dragged = true;
            gDelete_ = false;
        }
        const float pos = posOfPx(rollX(tc));
        Note& n = s.notes[sel_];
        if (gMode_ == 1)   // drawing: the press point stays one end
        {
            const float cur = std::max(0.0f, std::min((float)s.steps, snapOrFree(pos)));
            const float ml = minLen();
            float st, len;
            if (cur >= gAnchor_) { st = gAnchor_; len = std::max(ml, cur - gAnchor_); }
            else
            {
                // leftwards: stop at the end of the same-pitch note before
                st = std::max(cur, s.minStartPitch(gAnchor_, n.pitch, sel_));
                len = std::max(std::min(ml, s.maxLenPitch(st, n.pitch, sel_)), gAnchor_ - st);
            }
            len = std::min(len, s.maxLenPitch(st, n.pitch, sel_));   // rightwards: stop at the next same-pitch note
            if (std::fabs(st - n.st) > kEps) sel_ = moveNote(sel_, st);
            Note& m = s.notes[sel_];
            if (std::fabs(len - m.len) > kEps || std::fabs(st - n.st) > kEps) { m.len = len; modelDirty_ = true; echoNote(); }
        }
        else if (gMode_ == 2)   // move in time and pitch
        {
            const float d = (rollX(tc) - rollX(tc, true)) / kGW * zoom();
            float st;
            if (freeHeld()) st = std::round((gOrig_.st + d) * 96.0f) / 96.0f;
            else
            {
                st = snapOrFree(gOrig_.st + d);   // the start lands on the nearest visible snap line
            }
            st = std::max(0.0f, std::min((float)s.steps - n.len, st));   // its end stops at the sequence end
            const int dr = rowAtY(rollYUp(tc) / kGH) - rowAtY(rollYUp(tc, true) / kGH);
            const int pitch = std::max(0, std::min(kPitchN - 1, gOrig_.pitch + dr));
            if (!s.fitsPitch(st, std::min(n.len, s.steps - st), pitch, sel_)) return;   // stays where it last fitted
            const bool pitchChanged = pitch != n.pitch;
            if (std::fabs(st - n.st) > kEps) sel_ = moveNote(sel_, st);
            Note& m = s.notes[sel_];
            if (pitchChanged) m.pitch = (int16_t)pitch;
            if (pitchChanged || std::fabs(st - n.st) > kEps) { modelDirty_ = true; echoNote(); }
            if (pitchChanged) preview();
        }
        else if (gMode_ == 3)   // length from the right end
        {
            const float end = std::max(n.st + std::min(minLen(), s.maxLenPitch(n.st, n.pitch, sel_)),
                                       std::min(n.st + s.maxLenPitch(n.st, n.pitch, sel_), snapOrFree(pos)));
            const float len = end - n.st;
            if (std::fabs(len - n.len) > kEps) { n.len = len; modelDirty_ = true; echoNote(); }
        }
    }

    void onRelease(int t, Touch& tc)
    {
        if (t == 4) { setDisp(kFreeLit, 1.0f); return; }   // parked off the right edge of the screen
        if (t != 0) return;
        if (!tc.dragged && gMode_ == 1 && sel_ >= 0 && sel_ < ed().count)
        {
            ed().notes[sel_].len = gEntryLen_;   // a tap on an empty place: a note of Entry Len
            modelDirty_ = true;
            echoNote();
        }
        if (!tc.dragged && gMode_ >= 2)
        {
            if (gDelete_) { deleteSelected(); lastTapNote_ = -1; }
            else { lastTapNote_ = sel_; lastTapMs_ = msNow(); }
        }
        else lastTapNote_ = -1;
        gMode_ = 0;
        gDelete_ = false;
    }

    // 2.8: a finger on a CC lane sets the step under it to the finger height; a sweep fills the steps between
    void drawLane(const Touch& tc)
    {
        const int ls = choiceAt(kLaneSel);
        if (ls <= 0) return;
        CcLane& L = ed().lane[ls - 1];
        const int step = (int)std::floor(posOfPx(padPx(tc.x, kGW)));
        const int v = (int)std::lround(std::max(0.0f, std::min(1.0f, padPx(tc.y, kVelH) / kVelH)) * 127.0f);
        if (step < 0 || step >= L.steps) return;
        if (gLnLast_ >= 0 && gLnLast_ != step)
        {
            const int a = gLnLast_, va = L.val[a], d = step > a ? 1 : -1;
            for (int k = a + d; k != step; k += d)
                L.val[k] = (uint8_t)std::lround(va + (v - va) * (double)(k - a) / (step - a));
        }
        L.val[step] = (uint8_t)v;
        gLnLast_ = step;
        viewDirty_ = true;
        savedDirty_ = true;
    }
    void setVelFromTouch(const Touch& tc)
    {
        Sequence& s = ed();
        if (gVelNote_ < 0 || gVelNote_ >= s.count) return;
        const float v = std::max(0.01f, std::min(1.0f, padPx(tc.y, kVelH) / kVelH));
        if (std::fabs(v - s.notes[gVelNote_].v) > 1e-4f) { s.notes[gVelNote_].v = v; modelDirty_ = true; echo(kVel, v * 100); }
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
        trustRestored_ = false;
        echo(kLastKey, (float)key);
        const int p = keyToPitch(key);
        if (p < 0) return;
        if (keyCh_[key] >= 0) offKey(key);
        keyCh_[key] = startNote((float)tuning_.midiOf(p), vel / 127.0f);
        if (!getBool(kStepEntry)) return;
        Sequence& s = ed();
        const float elen = kEntryLens[std::max(0, std::min(8, choiceAt(kEntryLen)))];
        int at = -1;
        for (int i = 0; i < s.count; ++i) if (std::fabs(s.notes[i].st - cursor_) < kEps) at = i;
        if (at >= 0)
        {
            const float room = s.maxLenPitch(cursor_, p, at);
            if (room >= kMinLen && s.minStartPitch(cursor_ + kEps, p, at) <= cursor_ + kEps)
            {
                s.notes[at].pitch = (int16_t)p;
                s.notes[at].len = std::max(kMinLen, std::min(elen, room));
            }
            sel_ = at;
        }
        else
        {
            // a new note; not inside a same-pitch note, and it stops at the next one
            const float room = s.maxLenPitch(cursor_, p, -1);
            if (room >= kMinLen && s.minStartPitch(cursor_ + kEps, p, -1) <= cursor_ + kEps)
            {
                Note nt;
                nt.st = cursor_; nt.len = std::max(kMinLen, std::min(elen, room)); nt.pitch = (int16_t)p; nt.v = vel / 127.0f;
                const int idx = s.insert(nt);
                if (idx >= 0) sel_ = idx;
            }
        }
        cursor_ += elen;
        if (cursor_ >= s.steps - kEps) cursor_ = 0;
        echo(kCursor, cursor_);
        modelDirty_ = true;
        echoNote();
        keepVisible();
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
                                choiceAt(kCurve) == 1 ? scala::BendCurve::Opx4 : scala::BendCurve::Symmetric);
    }
    bool inBendRange(float semis) const { return semis <= getInt(kBendUp) && semis >= -getInt(kBendDown); }

    // force: send even if the channel already has this bend (before every note-on: a synth that
    // takes all channels has one bend for all of them; measured 2026-11-21: skipped bends left
    // notes at another channel's pitch)
    void sendBend(int c, int b, bool force = false)
    {
        if (b == chans_[c].bend && !force) return;
        const int v = b + 8192;
        ring_.push((uint8_t)(0xE0 | c), (uint8_t)(v & 0x7F), (uint8_t)(v >> 7), outTime());
        chans_[c].bend = b;
    }

    int startNote(float pitch, float vel01)
    {
        const int c = pickChannel();
        const int n12 = std::max(0, std::min(127, (int)std::lround(pitch)));
        sendBend(c, bendFor((pitch - n12) * 100.0f), true);
        int v = (int)std::lround(vel01 * 127.0f * get(kVelocity) / 100.0f);
        v = v < 1 ? 1 : (v > 127 ? 127 : v);
        ring_.push((uint8_t)(0x90 | c), (uint8_t)n12, (uint8_t)v, outTime());
        Chan& ch = chans_[c];
        ch.on = true; ch.note = n12; ch.ref = (float)n12; ch.started = ++clock_;
        return c;
    }

    void stopNote(int c)
    {
        if (c < 0 || c > 15 || !chans_[c].on) return;
        ring_.push((uint8_t)(0x80 | c), (uint8_t)chans_[c].note, 0, outTime());
        chans_[c].on = false;
        chans_[c].released = ++clock_;
    }

    // the delivery time of a message made now: the steady time of the current sample + MIDI Delay
    int64_t outTime() const { return sclock_.timeOf(curSample_) + (int64_t)getInt(kMidiDelay) * 1000000LL; }

    // engine output -> MIDI; returns true if something was queued
    // 2.8: CCs go out on First Ch (user: one MIDI Learn assignment per CC)
    int ccChannel() const { return std::max(0, std::min(15, getInt(kFirstCh) - 1)); }
    void sendCC(int cc, int v)
    {
        ring_.push((uint8_t)(0xB0 | ccChannel()), (uint8_t)cc, (uint8_t)v, outTime());
        ccSent_[cc & 127] = v;
    }
    // the note's locks on the sequence's Lock CCs (always sent, so they win over a lane)
    void sendLocks(int ni)
    {
        const Sequence& q = model_->seqs[engSeq_];
        if (ni < 0 || ni >= q.count) return;
        for (int k = 0; k < 4; ++k)
            if (q.notes[ni].lock[k] >= 0 && q.lockCC[k] > 0) sendCC(q.lockCC[k], q.notes[ni].lock[k]);
    }
    // the CC lanes at song position p: a new step sends its value when it differs from the last one sent
    bool tickLanes(double p)
    {
        const Sequence& q = model_->seqs[engSeq_];
        bool sent = false;
        for (int l = 0; l < kCcLanes; ++l)
        {
            const CcLane& L = q.lane[l];
            if (L.cc == 0) { lnIdx_[l] = -1; continue; }
            const double sq = (double)kRateQ[std::max(0, std::min(kNumMusRates - 1, (int)L.rate))] / (L.mode == 1 ? ratioOf(L.ratio, L.fine) : 1.0);
            const long nn = (long)std::floor((p - lnAnchor_) / sq + 1e-9);
            if (nn == lnN_[l]) continue;
            lnN_[l] = nn;
            const int steps = std::max(1, (int)L.steps);
            const int m = (int)(((nn % steps) + steps) % steps);
            int idx = m;
            if (L.dir == 1) idx = steps - 1 - m;
            else if (L.dir == 2) { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; idx = (int)(rng_ % (uint32_t)steps); }
            lnIdx_[l] = idx;
            if (ccSent_[L.cc] != L.val[idx]) { sendCC(L.cc, L.val[idx]); sent = true; }
        }
        return sent;
    }
    // at stop: the lanes send their Rest value; the first lane step after a start always sends
    void restLanes()
    {
        const Sequence& q = model_->seqs[engSeq_];
        for (const CcLane& L : q.lane) if (L.cc > 0) sendCC(L.cc, L.rest);
        for (int& c : ccSent_) c = -1;
        for (int& x : lnIdx_) x = -1;
    }

    bool toMidi(const EngineOut& o)
    {
        bool sent = false;
        const bool high = o.gate > 0;
        if (high && !outHigh_)
        {
            sendLocks(o.note);   // 2.8
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

    // ===== display geometry ========================================================================
    // rowY_[r] = bottom of visible row r as a fraction of the roll height (0 = bottom); rowY_[16] = 1.
    double centsOf(int idx) const { return tuning_.periodOf(idx) * tuning_.period() + tuning_.cents(tuning_.degreeOf(idx)); }
    void computeRows()
    {
        const int low = kCenter + viewLow_;
        const double c0 = centsOf(low), span = std::max(1e-6, centsOf(low + kRows) - c0);
        for (int r = 0; r <= kRows; ++r) rowY_[r] = (float)((centsOf(low + r) - c0) / span);
    }
    int rowAtY(float y) const
    {
        for (int r = 0; r < kRows; ++r) if (y < rowY_[r + 1]) return r;
        return kRows - 1;
    }
    // The MPC keeps an XYPad picture inside its pad: value v puts the picture's centre at
    // size / 2 + v * (pad - size). padPos turns a wanted centre (pixels from the pad's left or
    // bottom edge) into that value.
    static float padPos(float centre, float size, float pad)
    {
        return std::max(0.0f, std::min(1.0f, (centre - size * 0.5f) / (pad - size)));
    }
    // x-parked markers: the pad starts kOFF px left of the roll and is kOFF + kGW + w wide, so the
    // picture's left edge is roll left + v * (kOFF + kGW) - kOFF. v = 0 puts it off the screen.
    static float xPark(float left, float, bool show)
    {
        return show ? std::max(0.0f, std::min(1.0f, (left + kOFF) / (kOFF + kGW))) : 0.0f;
    }

    // marks of note n drawn from a to b (px): ratchet gaps, glide ramp, chance ring (x-park values).
    // The ring shows for any chance operation that is set (also at 0 %).
    void noteMarks(const Note& n, float a, float b, bool startShown, float* out) const
    {
        for (int k = 0; k < kGaps; ++k)
        {
            const bool show = k < n.r - 1;
            const float gx = pxOfPos(n.st + n.len * (k + 1) / std::max(1, (int)n.r)) - kGapW * 0.5f;
            out[k] = xPark(gx, kGapW, show && gx > a && gx < b - kGapW);
        }
        bool chance = false;
        for (int l = 0; l < kLanes; ++l) chance = chance || n.ch[l].op != 0;
        const bool glide = n.gl > 0;
        out[kGaps] = xPark(a + 6, kBadgeW, glide && startShown && b - a >= 6 + kBadgeW);
        const float cx = a + (glide ? 6 + kBadgeW + 2 : 6);
        out[kGaps + 1] = xPark(cx, kBadgeW, chance && startShown && b - cx >= kBadgeW);
    }

    // ===== audio thread -> worker: the piano roll ==================================================
    void refreshView()
    {
        Sequence& s = ed();
        computeRows();
        const float q = snapQ(), z = zoom(), v0 = (float)viewStart_, stepPx = kGW / z;
        const float rowPad = kGH + kPARKT;   // row lines / labels: parked at the top (under the ruler)
        auto rowCentre = [&](int r) { return (rowY_[r] + rowY_[r + 1]) * 0.5f * kGH; };   // from the bottom
        // rows: lines, period lines, degree labels
        int copies[31] = {0};
        float labY[31][3];
        for (auto& a : labY) for (float& x : a) x = 1.0f;
        int periods = 0;
        float perY[3] = {1, 1, 1};
        for (int r = 0; r < kRows; ++r)
        {
            const int idx = std::max(0, std::min(kPitchN - 1, kCenter + viewLow_ + r));
            const int d = tuning_.degreeOf(idx);
            if (d < 31 && copies[d] < 3) labY[d][copies[d]++] = padPos(rowCentre(r), 17, rowPad);
            if (d == 0 && periods < 3) perY[periods++] = padPos(rowY_[r] * kGH, 2, rowPad);
        }
        for (int i = 0; i <= kRows; ++i) setDisp(kRowLine0 + i, padPos(rowY_[i] * kGH, 1, rowPad));
        for (int i = 0; i < 3; ++i) setDisp(kPerLine0 + i, perY[i]);
        for (int d = 0; d < 31; ++d)
            for (int k = 0; k < 3; ++k) setDisp(kLab0 + d * 3 + k, labY[d][k]);

        // vertical lines (pad = roll + the parking strip at the right)
        const float wide = kGW + kPARK;
        auto lineX = [&](float pos, float w) {
            const float f = (pos - v0) / z;
            return (f >= -kEps && f <= 1 + kEps) ? padPos(f * kGW, w, wide) : 1.0f;
        };
        {
            int k = 0;
            for (int m = viewStart_; m <= v0 + z + kEps && k < kStepLines; ++m) setDisp(kStepLine0 + k++, lineX((float)m, 1));
            for (; k < kStepLines; ++k) setDisp(kStepLine0 + k, 1.0f);
            k = 0;
            const int div = (int)std::lround(1.0f / q);
            if (div > 1 && q * stepPx >= kMinLinePx)
                for (int m = (int)std::ceil(v0 / q - kEps); m * q <= v0 + z + kEps && k < kSnapLines; ++m)
                    if (m % div != 0) setDisp(kSnapLine0 + k++, lineX(m * q, 1));
            for (; k < kSnapLines; ++k) setDisp(kSnapLine0 + k, 1.0f);
            const int g = getInt(kGroup);
            k = 0;
            if (g > 0)
                for (int m = (int)std::ceil(v0 / g - kEps); m * g <= v0 + z + kEps && k < kGrpLines; ++m)
                    setDisp(kGrpLine0 + k++, lineX((float)(m * g), 2));
            for (; k < kGrpLines; ++k) setDisp(kGrpLine0 + k, 1.0f);
            // ruler numbers: every step if there is room, else at Group lines, else every 2nd, 4th ...
            int every = 1;
            if (stepPx < kNumMinPx)
            {
                if (g > 0 && g * stepPx >= kNumMinPx) every = g;
                else { every = 2; while (every * stepPx < kNumMinPx && every < 64) every *= 2; }
            }
            for (int m = 0; m < kNums; ++m)
            {
                const bool show = m >= viewStart_ && m < v0 + z - kEps && m < s.steps && m % every == 0;
                setDisp(kNumX0 + m, xPark(pxOfPos((float)m) + 3, kNumW, show));
            }
        }
        setDisp(kEndX, s.steps < v0 + z - kEps ? lineX((float)s.steps, 3) : 1.0f);
        setDisp(kCursorX, getBool(kStepEntry) && cursor_ >= v0 - kEps && cursor_ < v0 + z - kEps
                              ? padPos(pxOfPos(cursor_) + 6.5f, 13, wide) : 1.0f);

        // notes: exact bodies from binary pieces, notch, ratchet gaps, glide and chance badges,
        // velocity bars. The later note is drawn on top.
        // whole pixels: the MPC places each piece on a whole pixel, and the pictures are 1 px wider
        // than their step (each piece overlaps the next), so no gap opens between pieces
        auto bodyPieces = [&](int base, float a, float b, bool show) {   // pieces k at base + k
            const float ra = std::round(a);
            int len = show ? std::max(1, (int)std::lround(b) - (int)ra) : 0;
            float x = ra;
            for (int k = kPieces - 1; k >= 0; --k)
            {
                const int w = 1 << k;
                if (len >= w) { setDisp(base + k, xPark(x, (float)w, true)); x += w; len -= w; }
                else setDisp(base + k, 0.0f);
            }
        };
        int j = 0, selJ = -1;
        for (int i = 0; i < s.count && j < kShown; ++i)
        {
            const Note& n = s.notes[i];
            const int r = n.pitch - (kCenter + viewLow_);
            const float a = std::max(0.0f, pxOfPos(n.st)), b = std::min(kGW, pxOfPos(n.st + n.len));
            if (r < 0 || r >= kRows || b <= 0 || a >= kGW) continue;
            const bool startShown = n.st >= v0 - kEps;
            setDisp(kNoteY0 + j, padPos(rowCentre(r), kNoteH, kGH));
            bodyPieces(kPieceX0 + j * kPieces, a, b, true);
            setDisp(kNotchX0 + j, xPark(a, kNotchW, startShown));
            float marks[kGaps + 2];
            noteMarks(n, a, b, startShown, marks);
            for (int k = 0; k < kGaps; ++k) setDisp(kGapX0 + j * kGaps + k, marks[k]);
            setDisp(kGlideX0 + j, marks[kGaps]);
            setDisp(kChanceX0 + j, marks[kGaps + 1]);
            setDisp(kVelX0 + j, xPark(a, kVelW, startShown));
            setDisp(kVelY0 + j, n.v);
            if (i == sel_) selJ = j;
            ++j;
        }
        for (; j < kShown; ++j)
        {
            bodyPieces(kPieceX0 + j * kPieces, 0, 0, false);
            setDisp(kNotchX0 + j, 0.0f);
            for (int k = 0; k < kGaps; ++k) setDisp(kGapX0 + j * kGaps + k, 0.0f);
            setDisp(kGlideX0 + j, 0.0f); setDisp(kChanceX0 + j, 0.0f); setDisp(kVelX0 + j, 0.0f);
        }
        // the selected note again, in its own colour, on top
        if (selJ >= 0)
        {
            const Note& n = s.notes[sel_];
            const float a = std::max(0.0f, pxOfPos(n.st)), b = std::min(kGW, pxOfPos(n.st + n.len));
            setDisp(kSelY, computed_[kNoteY0 + selJ]);
            bodyPieces(kSelPieceX0, a, b, true);
            setDisp(kSelNotchX, xPark(a, kNotchW, n.st >= v0 - kEps));
            setDisp(kSelVelX, computed_[kVelX0 + selJ]);
            setDisp(kSelVelY, n.v);
            float marks[kGaps + 2];
            noteMarks(n, a, b, n.st >= v0 - kEps, marks);
            for (int k = 0; k < kGaps; ++k) setDisp(kSelGapX0 + k, marks[k]);
            setDisp(kSelGlideX, marks[kGaps]);
            setDisp(kSelChanceX, marks[kGaps + 1]);
        }
        else
        {
            bodyPieces(kSelPieceX0, 0, 0, false);
            setDisp(kSelNotchX, 0.0f);
            setDisp(kSelVelX, 0.0f);
            for (int k = 0; k < kGaps; ++k) setDisp(kSelGapX0 + k, 0.0f);
            setDisp(kSelGlideX, 0.0f);
            setDisp(kSelChanceX, 0.0f);
        }
        // 2.8: LANE: a thin bar at each step of the view of the shown CC lane (height = value)
        {
            const int ls = choiceAt(kLaneSel);
            const CcLane* L = ls > 0 ? &ed().lane[ls - 1] : nullptr;
            for (int j = 0; j < kMaxSteps; ++j)
            {
                const int step = viewStart_ + j;
                const bool show = L && step < L->steps && step < viewStart_ + zoom() - kEps;
                setDisp(kLnBarX0 + j, xPark(pxOfPos((float)step), kVelW, show));
                setDisp(kLnBarY0 + j, show ? L->val[step] / 127.0f : 0.0f);
            }
        }
        setDisp(kLineY, 0.5f);
        updateTexts();
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

    // MIDI thread: only sends the queued messages to ALSA. Real-time priority (if the system
    // allows it), woken by the audio thread, so notes are not delayed by display work.
    void midiLoop()
    {
#if defined(__linux__)
        sched_param sp{};
        sp.sched_priority = 40;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   // ignored if not permitted
#endif
        alsa_.open(clientName_, "MIDI Out");
        // 2.1.1: scheduled output (alsa_.startQueue) is off: measured on the MPC One G2 it was worse
        // (20.9 ms spread, and a pitch bend could arrive after its note-on). Direct output, as 2.0.
        while (!quit_.load())
        {
            wakeMidi_.waitMs(50);
            uint8_t m[3];
            int64_t t;
            int n = 0;
            while (ring_.pop(m, t))
            {
                deliver(m, 0);
                ++n;
            }
            if (n) alsa_.flush();
        }
        uint8_t m[3];
        int64_t t;
        while (ring_.pop(m, t)) deliver(m, 0);
        for (int c = 0; c < 16; ++c) { const uint8_t off[3] = {(uint8_t)(0xB0 | c), 123, 0}; deliver(off, 0); }
        alsa_.flush();
        alsa_.close();
    }

    // Display thread: loads scales and tells the host which values changed, every kDisplayMs at
    // most. 2.4.1: 50 ms (was 16 ms). Measured on the MPC One G2: while the roll is dragged, the MPC's
    // screen thread used 30-56 % of a core and its audio thread twice its normal load, and the
    // audio crackled. Fewer updates mean fewer redraws.
    static constexpr int kDisplayMs = 50;
    void workerLoop()
    {
        auto lastNotify = std::chrono::steady_clock::now();
        while (!quit_.load())
        {
            wake_.waitMs(kDisplayMs);
            if (embedPending_.exchange(false))
            {
                std::string t;
                { std::lock_guard<std::mutex> l(scaleMutex_); t = embedText_; }
                loadScale(-1, &t);
            }
            else if (scaleDirty_.exchange(false))
            {
                const int si = choiceAt(kScale);
                bool skip;
                { std::lock_guard<std::mutex> l(scaleMutex_); skip = (si == appliedScale_); appliedScale_ = -1; }
                if (!skip) loadScale(si, nullptr);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastNotify < std::chrono::milliseconds(kDisplayMs)) continue;   // an early wake (scale load)
            lastNotify = now;
            for (int i = 0; i < kNumParams; ++i)
                if (notify_[i].exchange(false, std::memory_order_acquire)) notifyHostParameterChanged(i);
        }
    }


    // relNs: how long after now the message is due (<= 0: at once)
    void deliver(const uint8_t* m, int64_t relNs)
    {
        alsa_.sendIn(m, relNs);
#ifdef SCALASEQ_TEST
        std::lock_guard<std::mutex> l(testMutex_);
        testLog_.push_back({m[0], m[1], m[2]});
        testRel_.push_back(relNs);
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
    int tIdx_ = 0, edSeq_ = 0, engSeq_ = 0, sel_ = 0, viewStart_ = 0, viewLow_ = 0;
#ifdef SCALASEQ_TRACE
    int testPhase_ = -1;
    long testTick_ = -1;
#endif
    bool pendSwap_ = false, wasPlaying_ = false;
    // 2.5: sequence notes of the current block (playing), the song position where the playing
    // sequence started (0 = the song grid), the accepted Pad 1 Note
    static constexpr int kMaxSeqEv = 16;
    struct SeqEv { int at = 0, seq = 0; };
    SeqEv seqEv_[kMaxSeqEv];
    int seqEvN_ = 0, padNote_ = 37;
    double anchorQ_ = 0;
    // 2.8: CC lanes and locks
    static constexpr long kNoStep = -2147483647L;
    double lnAnchor_ = 0;
    long lnN_[kCcLanes];
    int lnIdx_[kCcLanes];
    int ccSent_[128];
    uint32_t rng_ = 0x9e3779b9u;
    int gLnLast_ = -1;
    float cursor_ = 0;
    double expectPpq_ = 0, playPos_ = 0;
    int playStep_ = -1;

    float lastSeen_[kNumParams];
    float computed_[kNumParams];   // display parameters: the last value the plugin computed
    bool primeOnly_ = false;
    bool trustRestored_ = false;   // after a 2.x load: no repair of restored display values until the first edit
    int stagingView_[4] = {0, 0, 0, 0};
    // value texts (written by the audio thread, read by formatValue)
    std::atomic<int> tSel_{0}, tDeg_{0}, tView0_{0}, tZoom_{16};
    std::atomic<int> tChOp_[kLanes] = {}, tChProb_[kLanes] = {}, tChAmt_[kLanes] = {};
    bool clearArmed_ = false;
    bool seqArmed_[2] = {false, false}, pastePending_ = false;   // 2.8: PASTE, CLEAR armed
    double seqArmMs_[2] = {0, 0};
    int burstSend_ = -1, burstCC_ = 0, burstStep_ = 0;   // SEND zigzag: which SEND is on (-1 = none)
    double burstMs_ = 0;
    bool sendPending_ = false;
    double clearArmMs_ = 0;
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
    int previewCh_ = -1;
    double previewOffMs_ = 0;
    bool previewSent_ = false;

    // touch gestures: 0 roll, 1 ruler, 2 labels, 3 velocity lane, 4 FREE
    Touch touch_[5];
    int gMode_ = 0;           // roll: 0 none, 1 draw, 2 move, 3 length
    float gAnchor_ = 0, gEntryLen_ = 1;
    Note gOrig_;
    bool gDelete_ = false;
    int lastTapNote_ = -1;
    double lastTapMs_ = -1e9;
    int gView0_ = 0, gZoom0_ = 16, gAxis_ = 0, gLow0_ = 0, gVelNote_ = -1;

    std::atomic<int> dispSize_{12}, dispCount_{0};
    std::atomic<bool> dispMask_[kMaxMask];
    std::atomic<int> laneOp_[kLanes] = {};

    rt::TimedRing ring_;
    rt::SampleClock sclock_;
    uint64_t curSample_ = 0;
    rt::Wakeup wake_;
    alsa::Output alsa_;
    char clientName_[48] = {};
    std::atomic<bool> quit_{false};
    std::thread worker_, midiThread_;
    rt::Wakeup wakeMidi_;

#ifdef SCALASEQ_TEST
public:
    std::mutex testMutex_;
    std::vector<std::array<uint8_t, 3>> testLog_;
    std::vector<int64_t> testRel_;
    Model& testModel() { return *model_; }
    void testSaveAll() { savedAll_ = true; }
    int testSel() const { return sel_; }
    float testCursor() const { return cursor_; }
    int testViewLow() const { return viewLow_; }
    int testViewStart() const { return viewStart_; }
    int testChoice(int i) const { return choiceAt(i); }
    struct TestStep { int64_t sample; int step, seq; };
    std::vector<TestStep> testSteps_;   // 2.5: each step change while playing (audio thread, test build only)
    int testEdSeq() const { return edSeq_; }
    int testEngSeq() const { return engSeq_; }
#endif
};

#ifdef SCALASEQ_TRACE
SSQ_TRACE_ENTRY(ScalaSeq)
#else
MPC_PLUGIN_ENTRY(ScalaSeq)
#endif
