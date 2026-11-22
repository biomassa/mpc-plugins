// percSeq - an MPC instrument plugin by blackdingus
//
// The percussion version of scalaSeq: a 16-row hit sequencer for MPC drum tracks and chopped
// samples. It makes no sound. It plays its hits as MIDI notes through its own ALSA port
// ("percSeq MIDI Out"), one note per row, on one channel (Out Ch).
//
// Each row has its own playhead with its own Rate, Steps, Range, Direction and Swing (polyrhythms).
// The chance ops of scalaSeq stay; the pitch ops change the sound (SND+, SND-, SND?, ...), picked
// from the POOL rows. New ops: FLAM, SMP REV (a CC for MIDI Learn -> Layer Reverse) and CC.
//
// Signal flow
//   MPC song position -> each row's phase -> row Engine::tick() per sample, half a step AHEAD of the
//     song (so a FLAM grace hit can come before its hit)
//     -> events stamped with their song position -> queue -> sent when the song reaches them
//     -> lock-free ring -> MIDI thread (real-time priority) -> ALSA sequencer port
//   A jump (start, loop, locate), a sequence switch or a Reset restarts every row: the queue is
//   flushed and the rows catch up over their look-ahead in 64 sub-ticks.
//   Pads / keys on this track: step entry at the cursor (Step Entry on) and preview.
//   MIDI notes 60-91 on this track select sequences 1-32.
//
// Threads: as scalaSeq. Audio thread: owns the 32 sequences, edits, engines, display values.
// MIDI thread: sends the queued MIDI to ALSA. Worker: tells the host which display values changed.

#include "mpc_plugin.h"
#ifdef PERCSEQ_TRACE
#include "psq_trace.h"
#endif
#include "perc_engine.h"
#include "../../common/alsa_out.h"
#include "../../common/rt_util.h"

#include <array>
#include <atomic>
#include <chrono>
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

using namespace perc;

constexpr int kShown = 32;          // hits drawn exactly on one screen (1.0.1: was 64)
constexpr int kPieces = 10;         // bar pieces of 1, 2, 4 ... 512 px
constexpr int kGaps = 7;            // ratchet gaps per hit (8 ratchets)
constexpr int kZoomN = 4;
constexpr int kNumLists = 13;        // 1.3: + Ratio, Lane Rate, Lane Ratio       // 1.1: drawn pop-up lists: Entry Len, Op 1-4, PTN 1-4, Rate           // 1.1: Steps/Page 8, 16, 32, 64
constexpr int kStepLines = 65, kSnapLines = 89, kGrpLines = 65, kNums = 64;

enum ParamIndex
{
    // SETUP (parameter 0 is nudged by the host on every prepare: Group only redraws)
    kGroup = 0, kZoom, kPadNote, kOutCh, kPtnMode, kReset, kSnap,
    // HIT
    kHitSel, kHitRow, kMove, kLen, kNudge, kVel, kRatch, kFade, kDuty,
    kCursor, kEntryLen, kStepEntry, kDelete,
    // VIEW
    kViewStart, kSequence, kSelRow,
    // ROW (the selected row)
    kRNote, kRVel, kRRate, kRSteps, kRStart, kREnd, kRDir, kRSwing, kRevCC, kCCNum, kCCRest,
    // CHANCE: 4 lanes x (op, prob, ptn, amount)
    kLane0,
    // POOL
    kPool0 = kLane0 + 4 * kLanes,
    kClearNote = kPool0 + kRows, kClearAll,
    // texts (written by the plugin)
    kLastKey, kViewText, kMoreText, kFreeLit, kChanceText0, kLabText0 = kChanceText0 + kLanes,
    // touch surfaces (ResetOnGestureEnd: 0, 0 = no finger)
    kRollTX = kLabText0 + kRows, kRollTY, kRulTX, kRulTY, kLabTX, kLabTY, kVelTX, kVelTY, kFreeTX, kFreeTY,
    // display markers (written by the plugin)
    kLineY, kCursorX, kCellX0, kEndX0 = kCellX0 + kRows * kZoomN,
    kStepLine0 = kEndX0 + kRows, kSnapLine0 = kStepLine0 + kStepLines, kGrpLine0 = kSnapLine0 + kSnapLines,
    kNumX0 = kGrpLine0 + kGrpLines,
    kHitY0 = kNumX0 + kNums, kPieceX0 = kHitY0 + kShown, kNotchX0 = kPieceX0 + kShown * kPieces,
    kGapX0 = kNotchX0 + kShown, kChanceX0 = kGapX0 + kShown * kGaps, kVelX0 = kChanceX0 + kShown,
    kVelY0 = kVelX0 + kShown,
    kSelY = kVelY0 + kShown, kSelPieceX0, kSelNotchX = kSelPieceX0 + kPieces, kSelGapX0,
    kSelChanceX = kSelGapX0 + kGaps, kSelVelX, kSelVelY,
    // 1.1: row on/off (the edited sequence's rows)
    kOn0,
    // 1.1: page buttons, FOLLOW, which drawn list is open (0 = none)
    kPagePrev = kOn0 + kRows, kPageNext, kFollow, kListOpen,
    // 1.2: momentary < > buttons of the step controls on the HIT page (Hit, Row, Move, Length)
    kHitPrev, kHitNext, kRowPrev, kRowNext, kMovePrev, kMoveNext, kLenPrev, kLenNext,
    // 1.3: Row < > (select the row) on RATE and LOCK; RATE: rate mode of the selected row
    kSelRowPrev, kSelRowNext, kRMode, kRRatio, kRFine,
    // 1.3: LOCK: the selected hit's lock values, the selected row's lock CC numbers
    kLock0, kLockCC0 = kLock0 + 4,
    // 1.3: LANE: which lane is shown (VEL, CC 1, CC 2) and the shown CC lane of the selected row
    kLaneSel = kLockCC0 + 4, kLnCC, kLnSteps, kLnRate, kLnMode, kLnRatio, kLnFine, kLnDir, kLnRest,
    // 1.3: LANE display: the lane's current step (one marker per zoom size) and the bars of the page
    kLnPlayX0, kLnBarY0 = kLnPlayX0 + kZoomN,
    // 1.3: SETUP: copy, paste and clear the edited sequence (PASTE and CLEAR: touch twice within 3 s)
    kSeqCopy = kLnBarY0 + kMaxSteps, kSeqPaste, kSeqClear,
    // 1.3: SEND buttons (one CC at value 64 for MIDI Learn): Lock CC 1-4, the shown lane, Rev CC, CC Num
    kSend0, kNumSends = 7,
    kNumParams = kSend0 + kNumSends
};
constexpr int kFirstEdit = kGroup, kLastEdit = kCCRest;
// MIDI notes 60-91 on this track select sequences 1-32. The 16 pad rows (Pad 1 Note .. +15) must
// not overlap them, so Pad 1 Note cannot be 45-91.
constexpr int kSeqNote0 = 60;
constexpr int kPadLowMax = kSeqNote0 - kRows, kPadHighMin = kSeqNote0 + kSeqs;   // 44 and 92
inline bool padNoteOk(int v) { return v <= kPadLowMax || v >= kPadHighMin; }

// ---- SEQ page geometry in pixels (must match skin/make_skin.py) -------------------------------
constexpr float kGW = 880, kGH = 540;            // the roll (1.3: + the old velocity lane, was 476)
constexpr float kRowH = kGH / kRows;             // 29.75
constexpr float kPARK = 40;                      // masked strip right of the roll (parked lines)
constexpr float kOFF = 1100;                     // x-parked markers: pad starts kOFF px left of the roll
constexpr float kNoteH = 24;
constexpr float kNotchW = 4, kGapW = 2, kBadgeW = 16, kNumW = 40, kVelW = 5, kVelH = 280, kFreeW = 272;   // 1.3: kVelH = the LANE page lane height
constexpr float kRulerPadH = 2628;               // ruler touch pad height (tall, so drags have room)
constexpr float kMinLinePx = 10;                 // snap lines closer than this are hidden
constexpr float kNumMinPx = 30;                  // ruler numbers at least this far apart
constexpr float kDragPx = 6;                     // a touch that moves less is a tap
constexpr double kDoubleTapMs = 300;
constexpr float kSwipePx = 60;                   // 1.1: ruler: a sideways swipe this long flips one page
constexpr float kZoomPx = 24;                    // ruler: px of vertical drag for one zoom size (1.1: 8/16/32/64 steps)
const int kSnapDiv[6] = {1, 2, 3, 4, 6, 8};
const float kEntryLens[9] = {0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
constexpr double kAheadSteps = 0.5;              // look-ahead of every row (FLAM up to 48/96 step)
constexpr int kCatchUp = 64;                     // sub-ticks of a restart's catch-up

struct Model
{
    Sequence seqs[kSeqs];
    bool pool[kRows];
    Model() { for (bool& p : pool) p = true; }
};

struct Touch
{
    bool down = false, dragged = false;
    float x = 0, y = 0;      // last position (0..1)
    float x0 = 0, y0 = 0;    // press position
    double t0 = 0;
};

// A MIDI message waiting for its song position.
struct Ev
{
    double t = 0;            // song position (quarter notes)
    uint8_t type = 0;        // 0 note on, 1 note off, 2 CC
    uint8_t a = 0, b = 0;    // note / velocity, or CC number / value
    int8_t row = -1;         // the row whose note it is (note on / off)
};

std::atomic<int> g_instances{0};
// 1.3: the copied sequence, shared by all percSeq instances (lost when the MPC app restarts)
Sequence g_clip;
bool g_clipHas = false;
std::mutex g_clipMutex;

} // namespace

class PercSeq : public mpc::Plugin
{
public:
    PercSeq() : mpc::Plugin(makeInfo()), model_(new Model), saved_(new Model), staging_(new Model),
                tables_(new Tables[2 * kRows])
    {
        auto num = [](const char* a, int i) { return std::string(a) + std::to_string(i + 1); };
        std::vector<std::string> rates;
        for (int i = 0; i < kNumRates; ++i) rates.push_back(rateNames()[i]);

        // SETUP
        addParam(mpc::intParam   ("group",     "Group",      0, 64, 0));
        addParam(mpc::choiceParam("zoom",      "Steps/Page", {"8 st", "16 st", "32 st", "64 st"}, 0));   // 1.1: default 8 steps
        addParam(mpc::intParam   ("padnote",   "Pad 1 Note", 0, 127, 37));
        addParam(mpc::intParam   ("outch",     "Out Ch",     1, 16, 1));
        addParam(mpc::choiceParam("ptnmode",   "PTN Count",  {"Loops", "Bars"}, 0));
        addParam(mpc::intParam   ("reset",     "Reset",      0, 64, 0));
        addParam(mpc::choiceParam("snap",      "Snap",       {"1", "1/2", "1/3", "1/4", "1/6", "1/8"}, 0));   // 1.1: default 1
        // HIT (knobs with 64 values or fewer: the MPC gives a knob about 128 positions)
        addParam(mpc::intParam   ("hit",       "Hit",        1, kMaxHits, 1));
        addParam(mpc::intParam   ("row",       "Row",        1, kRows, 1));
        addParam(mpc::intParam   ("move",      "Move",       0, 63, 0));   // snap steps
        addParam(mpc::intParam   ("length",    "Length",     1, 64, 1));   // snap steps
        addParam(mpc::intParam   ("nudge",     "Nudge",      -96, 96, 0)); // 1/96 step (1.1: was -32..31)
        addParam(mpc::floatParam ("vel",       "VEL",        0, 100, 100, "%", 0));
        addParam(mpc::intParam   ("ratch",     "RATCH",      1, 8, 1));
        addParam(mpc::intParam   ("fade",      "FADE",       -50, 50, 0));
        addParam(mpc::floatParam ("duty",      "DUTY",       0.05f, 0.95f, 0.5f, "", 2));
        addParam(mpc::floatParam ("cursor",    "Cursor",     0, kMaxSteps, 0, "st", 2));
        addParam(mpc::choiceParam("entrylen",  "Entry Len",  {"1/8", "1/4", "1/2", "3/4", "1", "1.5", "2", "3", "4"}, 4));
        addParam(mpc::boolParam  ("stepentry", "Step Entry", false));
        addParam(mpc::boolParam  ("delete",    "Delete",     false));
        // VIEW
        addParam(mpc::intParam   ("page",      "Page",       1, kMaxSteps / 8, 1));   // 1.1: pages of Steps/Page steps (was View Start)
        addParam(mpc::intParam   ("sequence",  "Sequence",   1, kSeqs, 1));
        addParam(mpc::intParam   ("selrow",    "Sel Row",    1, kRows, 1));
        // ROW
        addParam(mpc::intParam   ("rnote",     "MIDI Note",  0, 127, 36));
        addParam(mpc::floatParam ("rvel",      "Velocity",   0, 100, 100, "%", 0));
        addParam(mpc::choiceParam("rrate",     "Rate",       rates, kDefRate));
        addParam(mpc::intParam   ("rsteps",    "Steps",      1, kMaxSteps, 16));
        addParam(mpc::intParam   ("rstart",    "Range Start", 1, kMaxSteps, 1));
        addParam(mpc::intParam   ("rend",      "Range End",  1, kMaxSteps, 16));
        addParam(mpc::choiceParam("rdir",      "Direction",  {"Fwd", "Rev", "Rnd"}, 0));
        addParam(mpc::floatParam ("rswing",    "Swing",      50, 75, 50, "%", 0));
        addParam(mpc::intParam   ("revcc",     "Rev CC",     0, 127, 102));
        addParam(mpc::intParam   ("ccnum",     "CC Num",     0, 127, 14));
        addParam(mpc::intParam   ("ccrest",    "CC Rest",    0, 127, 64));
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
            addParam(mpc::intParam(("amt" + n).c_str(), ("Amount " + n).c_str(), 0, 127, 0));
        }
        // POOL
        for (int r = 0; r < kRows; ++r) addParam(mpc::boolParam(num("pool", r).c_str(), num("Row ", r).c_str(), true));
        addParam(mpc::boolParam  ("clearnote", "Clear Note", false));   // clears the chance lanes of the selected hit
        addParam(mpc::boolParam  ("clearall",  "Clear All",  false));   // 1st touch arms, 2nd touch within 3 s clears all
        // texts
        addParam(mpc::intParam   ("lastkey",   "Last Note",  0, 127, 0));
        addParam(mpc::floatParam ("viewtext",  "View Text",  0, 1, 0, "", 3));
        addParam(mpc::floatParam ("moretext",  "More Text",  0, 1, 0, "", 3));
        addParam(mpc::floatParam ("freelit",   "Free Lit",   0, 1, 1, "", 4));   // 1 = parked off the right edge
        auto disp = [&](const std::string& id, const std::string& name, float def) { addParam(mpc::floatParam(id.c_str(), name.c_str(), 0, 1, def, "", 4)); };
        for (int l = 0; l < kLanes; ++l) disp(num("chancetext", l), num("Chance ", l), 0);
        for (int r = 0; r < kRows; ++r) disp(num("labtext", r), num("Lab ", r), 0);
        // touch surfaces
        const char* tn[5] = {"roll", "ruler", "labels", "lane", "free"};
        for (int t = 0; t < 5; ++t)
        {
            addParam(mpc::floatParam((std::string(tn[t]) + "tx").c_str(), (std::string("Touch ") + tn[t] + " X").c_str(), 0, 1, 0, "", 4));
            addParam(mpc::floatParam((std::string(tn[t]) + "ty").c_str(), (std::string("Touch ") + tn[t] + " Y").c_str(), 0, 1, 0, "", 4));
        }
        // display markers
        disp("liney", "Line Y", 0.5f);
        disp("cursorx", "Cursor X", 1);
        for (int r = 0; r < kRows; ++r)
            for (int k = 0; k < kZoomN; ++k)
                disp("cellx" + std::to_string(r + 1) + "_" + std::to_string(k), "Cell X " + std::to_string(r + 1) + "." + std::to_string(k), 0);
        for (int r = 0; r < kRows; ++r) disp(num("endx", r), num("End X ", r), 1);
        for (int i = 0; i < kStepLines; ++i) disp(num("stepline", i), num("Step Line ", i), 1);
        for (int i = 0; i < kSnapLines; ++i) disp(num("snapline", i), num("Snap Line ", i), 1);
        for (int i = 0; i < kGrpLines; ++i) disp(num("grpline", i), num("Grp Line ", i), 1);
        for (int i = 0; i < kNums; ++i) disp(num("numx", i), num("Num X ", i), 0);
        for (int j = 0; j < kShown; ++j) disp(num("hity", j), num("Hit Y ", j), 0);
        for (int j = 0; j < kShown; ++j)
            for (int k = 0; k < kPieces; ++k) disp("piece" + std::to_string(j + 1) + "_" + std::to_string(k), "Piece " + std::to_string(j + 1) + "." + std::to_string(k), 0);
        for (int j = 0; j < kShown; ++j) disp(num("notchx", j), num("Notch X ", j), 0);
        for (int j = 0; j < kShown; ++j)
            for (int k = 0; k < kGaps; ++k) disp("gap" + std::to_string(j + 1) + "_" + std::to_string(k + 1), "Gap " + std::to_string(j + 1) + "." + std::to_string(k + 1), 0);
        for (int j = 0; j < kShown; ++j) disp(num("chancex", j), num("Chance X ", j), 0);
        for (int j = 0; j < kShown; ++j) disp(num("velx", j), num("Vel X ", j), 0);
        for (int j = 0; j < kShown; ++j) disp(num("vely", j), num("Vel Y ", j), 0);
        disp("sely", "Sel Y", 0);
        for (int k = 0; k < kPieces; ++k) disp("selpiece" + std::to_string(k), "Sel Piece " + std::to_string(k), 0);
        disp("selnotchx", "Sel Notch X", 0);
        for (int k = 0; k < kGaps; ++k) disp("selgap" + std::to_string(k + 1), "Sel Gap " + std::to_string(k + 1), 0);
        disp("selchancex", "Sel Chance X", 0);
        disp("selvelx", "Sel Vel X", 0); disp("selvely", "Sel Vel Y", 0);
        for (int r = 0; r < kRows; ++r) addParam(mpc::boolParam(num("on", r).c_str(), num("On ", r).c_str(), true));
        addParam(mpc::boolParam  ("pageprev",  "Page Prev",  false));   // momentary: the plugin sets it back to off
        addParam(mpc::boolParam  ("pagenext",  "Page Next",  false));
        addParam(mpc::boolParam  ("follow",    "Follow",     false));
        addParam(mpc::intParam   ("listopen",  "List Open",  0, kNumLists, 0));
        // 1.2: momentary step buttons (the plugin sets them back to off); appended, so old projects keep their indices
        addParam(mpc::boolParam  ("hitprev",   "Hit Prev",   false));
        addParam(mpc::boolParam  ("hitnext",   "Hit Next",   false));
        addParam(mpc::boolParam  ("rowprev",   "Row Prev",   false));
        addParam(mpc::boolParam  ("rownext",   "Row Next",   false));
        addParam(mpc::boolParam  ("moveprev",  "Move Prev",  false));
        addParam(mpc::boolParam  ("movenext",  "Move Next",  false));
        addParam(mpc::boolParam  ("lenprev",   "Len Prev",   false));
        addParam(mpc::boolParam  ("lennext",   "Len Next",   false));
        // 1.3 (appended)
        std::vector<std::string> ratios, ccs{"off"};
        for (int i = 0; i < kNumRatios; ++i) ratios.push_back(ratioNames()[i]);
        addParam(mpc::boolParam  ("selrowprev", "Sel Row Prev", false));
        addParam(mpc::boolParam  ("selrownext", "Sel Row Next", false));
        addParam(mpc::choiceParam("rmode",     "Rate Mode",  {"Musical", "Ratio"}, 0));
        addParam(mpc::choiceParam("rratio",    "Ratio",      ratios, kDefRatio));
        addParam(mpc::intParam   ("rfine",     "Fine",       -10, 10, 0));
        for (int k = 0; k < 4; ++k) addParam(mpc::intParam(num("lock", k).c_str(), num("Lock ", k).c_str(), -1, 127, -1));
        for (int k = 0; k < 4; ++k) addParam(mpc::intParam(num("lockcc", k).c_str(), num("Lock CC ", k).c_str(), 0, numCCChoices() - 1, 0));
        addParam(mpc::choiceParam("lanesel",   "Lane",       {"VEL", "CC 1", "CC 2"}, 0));
        addParam(mpc::intParam   ("lncc",      "Lane CC",    0, numCCChoices() - 1, 0));   // the LANE page shows it as "CC Num"
        addParam(mpc::intParam   ("lnsteps",   "Lane Steps", 1, kMaxSteps, 16));
        addParam(mpc::choiceParam("lnrate",    "Lane Rate",  rates, kDefRate));
        addParam(mpc::choiceParam("lnmode",    "Lane Mode",  {"Musical", "Ratio"}, 0));
        addParam(mpc::choiceParam("lnratio",   "Lane Ratio", ratios, kDefRatio));
        addParam(mpc::intParam   ("lnfine",    "Lane Fine",  -10, 10, 0));
        addParam(mpc::choiceParam("lndir",     "Lane Dir",   {"Fwd", "Rev", "Rnd"}, 0));
        addParam(mpc::intParam   ("lnrest",    "Rest",       0, 127, 64));
        for (int k = 0; k < kZoomN; ++k) disp(num("lnplayx", k), num("Lane Play X ", k), 0);
        for (int j = 0; j < kMaxSteps; ++j) disp(num("lnbary", j), num("Lane Bar Y ", j), 0);
        addParam(mpc::boolParam  ("seqcopy",   "Copy",       false));
        addParam(mpc::boolParam  ("seqpaste",  "Paste",      false));
        addParam(mpc::boolParam  ("seqclear",  "Clear Seq",  false));
        for (int k = 0; k < 4; ++k) addParam(mpc::boolParam(num("sendlock", k).c_str(), num("Send Lock ", k).c_str(), false));
        addParam(mpc::boolParam  ("sendlane",  "Send Lane",  false));
        addParam(mpc::boolParam  ("sendrev",   "Send Rev CC", false));
        addParam(mpc::boolParam  ("sendcc",    "Send CC Num", false));

        if (numParams() != kNumParams) std::abort();   // the index layout and the parameter list must match
        for (auto& f : notify_) f.store(false);
        for (int r = 0; r < kRows; ++r) for (int l = 0; l < kCcLanes; ++l) { lnN_[r][l] = kNoStep; lnIdx_[r][l] = -1; }
        for (int& c : ccSent_) c = -1;
        for (int i = 0; i < kNumParams; ++i) { lastSeen_[i] = getNormalised(i); computed_[i] = -1; }
        for (int& k : keyNote_) k = -1;
        for (int r = 0; r < kRows; ++r) { engines_[r].home = r; engines_[r].reset(); }
        sctx_.pool = &pool_;
        sctx_.nextRow = &PercSeq::nextRowThunk;
        sctx_.user = this;
        pool_.set(model_->pool);
        rebuildAll();
        refreshView();
        const int n = g_instances.fetch_add(1);
        if (n == 0) std::snprintf(clientName_, sizeof clientName_, "percSeq");
        else        std::snprintf(clientName_, sizeof clientName_, "percSeq %d", n + 1);
        midiThread_ = std::thread([this] { midiLoop(); });
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~PercSeq() override
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
        bool loaded = false;
        if (stagingReady_.exchange(false))
        {
            *model_ = *staging_;
            edSeq_ = engSeq_ = std::max(0, std::min(kSeqs - 1, stagingView_[0]));
            selRow_ = std::max(0, std::min(kRows - 1, stagingView_[1]));
            sel_ = stagingView_[2]; viewStart_ = stagingView_[3];
            savedAll_ = true;
            loaded = true;
        }
        allNotesOff();
        for (Engine& e : engines_) { e.reset(); e.restart(); }
        for (int i = 0; i < kNumParams; ++i) lastSeen_[i] = getNormalised(i);
        for (Touch& t : touch_) t = Touch();
        if (edSeq_ != engSeq_) { engSeq_ = edSeq_; pendSwitch_ = false; }
        sel_ = std::max(-1, std::min(row().count - 1, sel_));
        if (sel_ < 0 && row().count > 0) sel_ = 0;
        viewStart_ = std::max(0, std::min(kMaxSteps - 1, viewStart_));
        cursor_ = std::max(0.0f, std::min((float)kMaxSteps, get(kCursor)));
        echo(kListOpen, 0);
        for (int i = kHitPrev; i <= kLenNext; ++i) echo(i, 0);   // momentary buttons start off
        for (int i = kSeqCopy; i <= kSeqClear; ++i) echo(i, 0);
        for (int i = kSend0; i < kSend0 + kNumSends; ++i) echo(i, 0);
        padNote_ = getInt(kPadNote);
        if (!padNoteOk(padNote_)) { padNote_ = kPadHighMin; echo(kPadNote, (float)padNote_); }
        for (int r = 0; r < kRows; ++r) model_->pool[r] = getBool(kPool0 + r);
        pool_.set(model_->pool);
        seqEvN_ = 0;
        for (double& a : anchor_) a = 0;
        rebuildAll();
        syncLaneOps();
        echoRow();
        echoHit();
        if (loaded)
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
        pollEdits();
        if (previewNote_ >= 0 && msNow() >= previewOffMs_) { sendNow(0x80, (uint8_t)previewNote_, 0); previewNote_ = -1; sent = true; }
        if (previewSent_) { previewSent_ = false; sent = true; }

        // MIDI in: sequence notes, step entry and preview
        const mpc::Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        for (int i = 0; i < ctx.numMidiEvents; ++i)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (e.isNoteOn() && isSeqNote(e.note()))
            {
                echo(kLastKey, (float)e.note());
                if (!playing) selectSequence(e.note() - kSeqNote0);
                else if (seqEvN_ < kMaxSeqEv) { seqEv_[seqEvN_].at = std::max(0, std::min(ctx.numFrames - 1, e.sampleOffset)); seqEv_[seqEvN_].seq = e.note() - kSeqNote0; ++seqEvN_; }
            }
            else if (e.isNoteOff() && isSeqNote(e.note())) {}   // ignored
            else if (e.isNoteOn()) { onKey(e.note(), e.velocity()); sent = true; }
            else if (e.isNoteOff()) { offKey(e.note()); sent = true; }
            else if (e.isAllNotesOff()) { for (int k = 0; k < 128; ++k) offKey(k); sent = true; }
        }
        if (dirtyRows_) { rebuildDirty(); savedDirty_ = true; viewDirty_ = true; }

        // transport
        const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
        const double bpm = (tr.tempoValid && tr.bpm > 1) ? tr.bpm : 120.0;
        const double qps = bpm / 60.0 / sr;   // quarter notes per sample
        // The MPC reports the song position rounded to small ticks; the plugin runs its own steady
        // clock and follows the MPC only slowly (forward only); a jump restarts every row (scalaSeq 2.0.1).
        const bool jump = !wasPlaying_ || std::fabs(tr.ppqPos - expectPpq_) > 0.05;
        const double base = jump ? tr.ppqPos : expectPpq_ + std::max(0.0, 0.05 * (tr.ppqPos - expectPpq_));
        if (!playing && wasPlaying_) { flushOutput(); restCCs(); sent = true; }
        barQ_ = tr.timeSigDenominator > 0 ? 4.0 * tr.timeSigNumerator / tr.timeSigDenominator : 4.0;
        const bool ptnBars = choiceAt(kPtnMode) == 1;
        const int barCount = (int)std::floor(tr.ppqPos / barQ_);
        for (int r = 0; r < kRows; ++r)
        {
            engines_[r].dir = model_->seqs[engSeq_].rows[r].dir;
            engines_[r].ptnBars = ptnBars;
            engines_[r].barCount = barCount;
        }
        const int resetBars = getInt(kReset);
        const double resetQ = resetBars > 0 ? resetBars * barQ_ : 0;

        if (playing)
        {
            cacheRows();
            if (jump) { restartAll(base, false); sent = true; }
            else
                // 1.1: a row switched off stops now; a row switched on waits for the next bar line
                for (int r = 0; r < kRows; ++r)
                {
                    const bool on = playRow(r).on;
                    if (!on && (rowLive_[r] || rowPend_[r])) { stopRow(r); sent = true; }
                    else if (on && !rowLive_[r]) rowPend_[r] = true;
                }
            int ev = 0;
            double prevP = jump ? base : lastP_;
            for (int i = 0; i < ctx.numFrames; ++i)
            {
                const double p = base + i * qps;
                bool restarted = false;
                // sequence notes switch at their sample and start the sequence there
                while (ev < seqEvN_ && seqEv_[ev].at <= i)
                {
                    selectSequence(seqEv_[ev].seq);
                    switchNow();
                    restartAll(p, true);
                    restarted = true;
                    ++ev;
                }
                if (!restarted && !(jump && i == 0))
                {
                    // the Sequence knob: the new sequence starts at the next bar line
                    if (pendSwitch_ && std::floor(p / barQ_) > std::floor(prevP / barQ_))
                    {
                        switchNow();
                        restartAll(std::floor(p / barQ_) * barQ_, true);
                        restarted = true;
                    }
                    // Reset: every N bars all rows start again at their Range Start
                    else if (resetQ > 0 && std::floor(p / resetQ) > std::floor(prevP / resetQ))
                    {
                        restartAll(std::floor(p / resetQ) * resetQ, true);
                        restarted = true;
                    }
                    // rows switched on start at their Range Start at the next bar line
                    else if (std::floor(p / barQ_) > std::floor(prevP / barQ_))
                        for (int r = 0; r < kRows; ++r)
                            if (rowPend_[r]) { restartRow(r, std::floor(p / barQ_) * barQ_); rowStarted_[r] = true; }
                }
                if (!restarted)
                    for (int r = 0; r < kRows; ++r)
                    {
                        if (rowStarted_[r]) { rowStarted_[r] = false; continue; }
                        if (rowLive_[r] && !empty_[r]) tickRow(r, p + ahead_[r], true);
                    }
                for (int r = 0; r < kRows; ++r) if (rowLive_[r]) tickLanes(r, p);   // 1.3
                if (emitDue(p)) sent = true;
                prevP = p;
            }
            expectPpq_ = base + ctx.numFrames * qps;
            lastP_ = prevP;
            // rows without hits only move their playhead: one tick per block (a block is shorter
            // than the shortest step, 1/64 at 300 bpm = 551 samples, except 512-frame blocks above 320 bpm)
            for (int r = 0; r < kRows; ++r) if (empty_[r] && rowLive_[r]) tickRow(r, prevP + ahead_[r], true);
            // playheads: the step each row is at now (the engines run ahead); see FOLLOW below
            for (int r = 0; r < kRows; ++r)
            {
                StepFifo& f = stepFifo_[r];
                while (f.n > 0 && f.t[f.head] <= expectPpq_)
                {
                    dispStep_[r] = f.step[f.head];
                    f.head = (f.head + 1) % kFifo; --f.n;
                }
            }
        }
        else
        {
            for (int& s : dispStep_) s = -1;
            if (pendSwitch_) switchNow();
        }
        wasPlaying_ = playing;
        seqEvN_ = 0;
        sampleCount_ += (uint64_t)std::max(0, ctx.numFrames);
        // 1.1: FOLLOW: the page follows the selected row's playhead
        if (playing && getBool(kFollow) && dispStep_[selRow_] >= 0 && dispStep_[selRow_] / (int)zoom() != page())
            setPage(dispStep_[selRow_] / (int)zoom());
        if (viewDirty_) { refreshView(); viewDirty_ = false; }
        // 1.1: each row's current step as a square, one marker per zoom size (the others parked)
        for (int r = 0; r < kRows; ++r)
        {
            const int st = rowLive_[r] ? dispStep_[r] : -1;
            const bool show = st >= 0 && st >= viewStart_ && st < viewStart_ + zoom() - kEps;
            for (int k = 0; k < kZoomN; ++k)
                setDisp(kCellX0 + r * kZoomN + k, xPark(pxOfPos((float)st), kGW / (8 << k), show && k == zoomClass()));
        }
        // 1.3: LANE: the shown CC lane's current step
        {
            const int ls = choiceAt(kLaneSel);
            const int st = (playing && ls > 0) ? lnIdx_[selRow_][ls - 1] : -1;
            const bool show = st >= 0 && st >= viewStart_ && st < viewStart_ + zoom() - kEps;
            for (int k = 0; k < kZoomN; ++k)
                setDisp(kLnPlayX0 + k, xPark(pxOfPos((float)st), kGW / (8 << k), show && k == zoomClass()));
        }
        if (notifyPending_) sent = true;
        // copy for project saving: the edited sequence only (a full copy after a load)
        if ((savedDirty_ || savedAll_) && savedMutex_.try_lock())
        {
            if (savedAll_) *saved_ = *model_;
            else saved_->seqs[edSeq_] = model_->seqs[edSeq_];
            for (int r = 0; r < kRows; ++r) saved_->pool[r] = model_->pool[r];
            savedMutex_.unlock();
            savedDirty_ = savedAll_ = false;
        }
        if (sent) { wakeMidi_.post(); notifyPending_ = false; }
    }

    // the text of a chance lane's Amount (as the Amount knob shows it)
    static std::string amountText(int op, int v)
    {
        char b[32];
        switch (opInfo(op).unit)
        {
            case U_NONE: return "-";
            case U_GATE: std::snprintf(b, sizeof b, "%s st", gateName(v)); break;
            case U_ROWS:
                if (v == 0 && (op == O_SNDRP || op == O_SNDRM || op == O_SNDR)) return "pool";
                std::snprintf(b, sizeof b, "%d rows", v); break;
            case U_X: std::snprintf(b, sizeof b, "x%d", v); break;
            case U_REP: std::snprintf(b, sizeof b, "+%d", v); break;
            case U_RND: std::snprintf(b, sizeof b, "+-%d", v); break;
            case U_PCT: std::snprintf(b, sizeof b, "%d%%", v); break;
            case U_TO: std::snprintf(b, sizeof b, "step %d", v); break;
            case U_FLAM: std::snprintf(b, sizeof b, "%d/96", v); break;
            case U_CC: std::snprintf(b, sizeof b, "val %d", v); break;
            default: std::snprintf(b, sizeof b, "%d", v); break;
        }
        return b;
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[40];
        const int v = (int)std::lround(plain);
        switch (index)
        {
            case kHitSel:
                if (tSel_.load() == 0) out = "none";
                else { std::snprintf(b, sizeof b, "%d/%d", tSel_.load(), tCount_.load()); out = b; }
                return true;
            case kViewText: std::snprintf(b, sizeof b, "%d/%d", tView0_.load() / std::max(1, tZoom_.load()) + 1, tPages_.load()); out = b; return true;
            case kMoreText: out = tMore_.load() ? "more" : ""; return true;
            case kNudge: std::snprintf(b, sizeof b, v == 0 ? "0" : "%+d", v); out = b; return true;
            // 1.3
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
            {
                const int ri = choiceAt(index == kRFine ? kRRatio : kLnRatio);
                std::snprintf(b, sizeof b, "%.2f", ratioOf(ri, v));
                out = b;
                return true;
            }
            case kMove: case kLen:
            {
                const float q = 1.0f / kSnapDiv[std::max(0, std::min(5, choiceAt(kSnap)))];
                std::snprintf(b, sizeof b, "%g", std::round(v * q * 1000) / 1000); out = b; return true;
            }
            case kGroup: if (v == 0) out = "off"; else { std::snprintf(b, sizeof b, "%d st", v); out = b; } return true;
            case kReset: if (v == 0) out = "off"; else { std::snprintf(b, sizeof b, v == 1 ? "%d bar" : "%d bars", v); out = b; } return true;
            default: break;
        }
        if (index >= kChanceText0 && index < kChanceText0 + kLanes)
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
        if (index >= kLabText0 && index < kLabText0 + kRows)
        {
            const int r = index - kLabText0;
            std::snprintf(b, sizeof b, "%d %d", r + 1, tRowNote_[r].load());
            out = b;
            return true;
        }
        if (index >= kLane0 && index < kPool0 && (index - kLane0) % 4 == 3)
        {
            out = amountText(laneOp_[(index - kLane0) / 4].load(std::memory_order_relaxed), v);
            return true;
        }
        return false;
    }

    // Project state (format PSQ3; 1.1 PSQ2: + row on/off; 1.3 PSQ3: + rate mode, lock CCs, CC lanes, hit locks;
    // PSQ1 and PSQ2 still load): the 32 sequences (16 rows each) and the pool.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(x >> (8 * i))); };
        auto putF = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(u); };
        out.insert(out.end(), {'P', 'S', 'Q', '3'});
        put32((uint32_t)edSeq_); put32((uint32_t)selRow_); put32((uint32_t)sel_); put32((uint32_t)viewStart_);
        std::lock_guard<std::mutex> l(savedMutex_);
        for (int r = 0; r < kRows; ++r) out.push_back(saved_->pool[r] ? 1 : 0);
        for (int s = 0; s < kSeqs; ++s)
            for (int r = 0; r < kRows; ++r)
            {
                const Row& q = saved_->seqs[s].rows[r];
                put32((uint32_t)q.steps); put32((uint32_t)q.rStart); put32((uint32_t)q.rEnd); putF(q.swing);
                put32((uint32_t)q.rate); put32((uint32_t)q.dir); put32((uint32_t)q.note); putF(q.vel);
                put32((uint32_t)q.revCC); put32((uint32_t)q.ccNum); put32((uint32_t)q.ccRest);
                put32(q.on ? 1u : 0u);
                put32((uint32_t)q.mode); put32((uint32_t)q.ratio); put32((uint32_t)q.fine);
                for (int k = 0; k < 4; ++k) out.push_back(q.lockCC[k]);
                for (const CcLane& L : q.lane)
                {
                    out.insert(out.end(), {L.cc, L.steps, L.rate, L.mode, L.ratio, L.dir, L.rest, (uint8_t)L.fine});
                    out.insert(out.end(), L.val, L.val + kMaxSteps);
                }
                put32((uint32_t)q.count);
                const uint8_t* p = reinterpret_cast<const uint8_t*>(q.hits);
                out.insert(out.end(), p, p + sizeof(Hit) * (size_t)q.count);
            }
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        size_t pos = 0;
        auto get32 = [&](uint32_t& x) { if (pos + 4 > size) return false; x = 0; for (int i = 0; i < 4; ++i) x |= (uint32_t)data[pos + i] << (8 * i); pos += 4; return true; };
        auto getF = [&](float& f) { uint32_t u; if (!get32(u)) return false; std::memcpy(&f, &u, 4); return true; };
        if (size < 4 || std::memcmp(data, "PSQ", 3) != 0 || data[3] < '1' || data[3] > '3') return;
        const int fmt = data[3] - '0';
        const bool v2 = fmt >= 2, v3 = fmt >= 3;
        pos = 4;
        int view[4] = {0, 0, 0, 0};
        for (int& x : view) { uint32_t u; if (!get32(u)) return; x = (int)u; }
        if (pos + kRows > size) return;
        Model& m = *staging_;
        for (int r = 0; r < kRows; ++r) m.pool[r] = data[pos++] != 0;
        for (int s = 0; s < kSeqs; ++s)
            for (int r = 0; r < kRows; ++r)
            {
                Row& q = m.seqs[s].rows[r];
                uint32_t st, rs, re, rate, dir, note, rc, cn, cr, cnt;
                float sw, vel;
                if (!get32(st) || !get32(rs) || !get32(re) || !getF(sw) || !get32(rate) || !get32(dir) || !get32(note)
                    || !getF(vel) || !get32(rc) || !get32(cn) || !get32(cr)) return;
                uint32_t on = 1;
                if (v2 && !get32(on)) return;
                Row fresh;   // 1.3 settings of older projects: defaults
                q.mode = fresh.mode; q.ratio = fresh.ratio; q.fine = fresh.fine;
                for (int k = 0; k < 4; ++k) q.lockCC[k] = 0;
                for (int l = 0; l < kCcLanes; ++l) q.lane[l] = CcLane();
                if (v3)
                {
                    uint32_t md, ra, fi;
                    if (!get32(md) || !get32(ra) || !get32((fi))) return;
                    q.mode = std::max(0, std::min(1, (int)md)); q.ratio = std::max(0, std::min(kNumRatios - 1, (int)ra));
                    q.fine = std::max(-10, std::min(10, (int)(int32_t)fi));
                    if (pos + 4 + kCcLanes * (8 + kMaxSteps) > size) return;
                    for (int k = 0; k < 4; ++k) { const int c = data[pos++]; q.lockCC[k] = (uint8_t)(ccAllowed(c) ? c : 0); }
                    for (CcLane& L : q.lane)
                    {
                        const uint8_t* b = data + pos;
                        L.cc = (uint8_t)(ccAllowed(b[0]) ? b[0] : 0); L.steps = (uint8_t)std::max(1, std::min(kMaxSteps, (int)b[1]));
                        L.rate = (uint8_t)std::min(kNumRates - 1, (int)b[2]); L.mode = (uint8_t)std::min(1, (int)b[3]);
                        L.ratio = (uint8_t)std::min(kNumRatios - 1, (int)b[4]); L.dir = (uint8_t)std::min(2, (int)b[5]);
                        L.rest = (uint8_t)std::min(127, (int)b[6]); L.fine = (int8_t)std::max(-10, std::min(10, (int)(int8_t)b[7]));
                        for (int k = 0; k < kMaxSteps; ++k) L.val[k] = (uint8_t)std::min(127, (int)b[8 + k]);
                        pos += 8 + kMaxSteps;
                    }
                }
                if (!get32(cnt)) return;
                q.on = on != 0;
                const size_t hsz = v3 ? sizeof(Hit) : sizeof(HitV2);
                if (cnt > (uint32_t)kMaxHits || pos + hsz * cnt > size) return;
                q.steps = std::max(1, std::min(kMaxSteps, (int)st));
                q.rStart = (int)rs; q.rEnd = (int)re; q.swing = std::max(50.0f, std::min(75.0f, sw));
                q.rate = std::max(0, std::min(kNumRates - 1, (int)rate)); q.dir = std::max(0, std::min(2, (int)dir));
                q.note = std::max(0, std::min(127, (int)note)); q.vel = std::max(0.0f, std::min(100.0f, vel));
                q.revCC = std::max(0, std::min(127, (int)rc)); q.ccNum = std::max(0, std::min(127, (int)cn));
                q.ccRest = std::max(0, std::min(127, (int)cr));
                q.count = (int)cnt;
                if (v3) std::memcpy(q.hits, data + pos, sizeof(Hit) * cnt);
                else
                    for (uint32_t i = 0; i < cnt; ++i)   // PSQ1/PSQ2 hits: no locks
                    {
                        HitV2 o;
                        std::memcpy(&o, data + pos + i * sizeof(HitV2), sizeof(HitV2));
                        Hit& h = q.hits[i];
                        h = Hit();
                        h.st = o.st; h.len = o.len; h.v = o.v; h.d = o.d; h.r = o.r; h.fd = o.fd; h.nd = o.nd;
                        std::memcpy(h.ch, o.ch, sizeof h.ch);
                    }
                for (uint32_t i = 0; i < cnt; ++i)
                    for (int8_t& lk : q.hits[i].lock) lk = std::max<int8_t>(-1, lk);
                pos += hsz * cnt;
                q.fitToSteps();
            }
        for (int k = 0; k < 4; ++k) stagingView_[k] = view[k];
        stagingReady_.store(true);
        wake_.post();
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "percSeq";
        info.vendor = "blackdingus";
        info.uniqueId = MPC_FOURCC('B', 'x', 'P', 'q');
        info.version = 1300;
        info.isInstrument = true;
        info.numInputs = 0;
        info.numOutputs = 2;
        info.tailSamples = 1;
        return info;
    }

    // ===== audio thread: model and edits ==========================================================
    Sequence& ed() { return model_->seqs[edSeq_]; }
    Row& row() { return ed().rows[selRow_]; }
    Row& rowOf(int r) { return ed().rows[r]; }
    Hit* selHit() { Row& s = row(); return (sel_ >= 0 && sel_ < s.count) ? &s.hits[sel_] : nullptr; }
    Tables& tab(int set, int r) { return tables_[set * kRows + r]; }
    const Row& playRow(int r) const { return model_->seqs[engSeq_].rows[r]; }
    // A choice parameter as the MPC shows it (rounded down, see scalaSeq 2.3.1).
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
    int zoomClass() const { return std::max(0, std::min(kZoomN - 1, choiceAt(kZoom))); }
    float zoom() const { return (float)(8 << zoomClass()); }
    double stepQ(int r) const { const Row& w = playRow(r); return stepQOf(w.rate, w.mode, w.ratio, w.fine); }
    int maxSteps() const
    {
        int m = 1;
        for (int r = 0; r < kRows; ++r)
        {
            const Row& w = model_->seqs[edSeq_].rows[r];
            m = std::max(m, w.steps);
            for (const CcLane& L : w.lane) m = std::max(m, (int)L.steps);   // 1.3: the LANE page pages too
        }
        return m;
    }

    void rebuildAll()
    {
        for (int r = 0; r < kRows; ++r)
        {
            buildTables(model_->seqs[engSeq_].rows[r], tab(tIdx_, r));
            if (pendSwitch_) buildTables(model_->seqs[edSeq_].rows[r], tab(1 - tIdx_, r));
        }
        dirtyRows_ = 0;
    }
    void rebuildDirty()
    {
        for (int r = 0; r < kRows; ++r)
            if (dirtyRows_ & (1u << r))
                buildTables(ed().rows[r], tab(edSeq_ == engSeq_ ? tIdx_ : 1 - tIdx_, r));
        dirtyRows_ = 0;
    }
    void markRow(int r) { dirtyRows_ |= 1u << r; }

    // the edited sequence (the Sequence knob); while playing the playing one follows at the next bar
    void setEdSeq(int q)
    {
        if (q == edSeq_) return;
        edSeq_ = q;
        sel_ = row().count > 0 ? 0 : -1;
        if (edSeq_ != engSeq_)
        {
            pendSwitch_ = true;
            for (int r = 0; r < kRows; ++r) buildTables(ed().rows[r], tab(1 - tIdx_, r));
        }
        else pendSwitch_ = false;
        echoRow();
        echoHit();
        keepVisible();
    }
    static bool isSeqNote(int n) { return n >= kSeqNote0 && n < kSeqNote0 + kSeqs; }
    void selectSequence(int q) { setEdSeq(q); echo(kSequence, (float)(edSeq_ + 1)); }
    void switchNow()
    {
        if (!pendSwitch_) return;
        tIdx_ = 1 - tIdx_;
        engSeq_ = edSeq_;
        pendSwitch_ = false;
    }

    // ---- playback ---------------------------------------------------------------------------------
    // the unswung position (in steps) where step lo begins (the engine swings pairs of steps)
    static double startPos(const Tables& T)
    {
        const int n = std::max(1, std::min(kMaxSteps, T.steps));
        const int lo = std::max(0, std::min(n - 1, T.rs));
        const double msw = std::max(50.0f, std::min(75.0f, T.swing)) / 100.0;
        return ((lo & 1) && lo < n) ? (lo - 1) + 2 * msw : (double)lo;
    }
    // per-row loop length and look-ahead, refreshed once per block and after a switch
    void cacheRows()
    {
        for (int r = 0; r < kRows; ++r)
        {
            const double sq = stepQ(r);
            invLoop_[r] = 1.0 / (sq * std::max(1, tab(tIdx_, r).steps));
            ahead_[r] = kAheadSteps * sq;
            empty_[r] = playRow(r).count == 0;
        }
    }
    double phaseOf(int r, double e) const
    {
        double ph = (e - anchor_[r]) * invLoop_[r];
        ph -= std::floor(ph);
        if (ph >= 1) ph = 0;
        return ph;
    }

    // Every row starts again at song position p. atStart: each row starts at its Range Start at p
    // (sequence notes, the Sequence knob, Reset). Otherwise (a jump) the rows follow the song grid
    // (with Reset: the last Reset boundary).
    void restartAll(double p, bool atStart)
    {
        flushOutput();
        nowP_ = p;
        cacheRows();
        const int resetBars = getInt(kReset);
        const double resetQ = resetBars > 0 ? resetBars * barQ_ : 0;
        for (int r = 0; r < kRows; ++r)
        {
            const Tables& T = tab(tIdx_, r);
            const double sq = stepQ(r);
            if (atStart) anchor_[r] = p - (startPos(T) + 1e-6) * sq;
            else if (resetQ > 0) anchor_[r] = std::floor(p / resetQ + 1e-9) * resetQ - (startPos(T) + 1e-6) * sq;
            else anchor_[r] = 0;
            engines_[r].restart();
            stepFifo_[r] = StepFifo();
            lastStep_[r] = -1;
            dispStep_[r] = -1;
            rowPend_[r] = false;
            // 1.3: CC lanes start at their step 1 at p (or follow the song / Reset grid after a jump)
            lnAnchor_[r] = atStart ? p : (resetQ > 0 ? std::floor(p / resetQ + 1e-9) * resetQ : 0.0);
            for (int l = 0; l < kCcLanes; ++l) { lnN_[r][l] = kNoStep; lnIdx_[r][l] = -1; }
            rowLive_[r] = playRow(r).on;
            if (!rowLive_[r]) continue;
            // catch up over the look-ahead
            const double L = ahead_[r];
            for (int k = 0; k <= kCatchUp; ++k) tickRow(r, p + L * k / kCatchUp, true);
        }
    }
    // 1.1: one row starts at its Range Start at song position p (switched on while playing)
    void restartRow(int r, double p)
    {
        const Tables& T = tab(tIdx_, r);
        anchor_[r] = p - (startPos(T) + 1e-6) * stepQ(r);
        lnAnchor_[r] = p;
        for (int l = 0; l < kCcLanes; ++l) { lnN_[r][l] = kNoStep; lnIdx_[r][l] = -1; }
        engines_[r].restart();
        stepFifo_[r] = StepFifo();
        lastStep_[r] = -1;
        rowPend_[r] = false;
        rowLive_[r] = true;
        const double L = ahead_[r];
        for (int k = 0; k <= kCatchUp; ++k) tickRow(r, p + L * k / kCatchUp, true);
    }
    // 1.1: a row switched off: its note ends now and its queued notes are dropped
    void stopRow(int r)
    {
        rowLive_[r] = false;
        rowPend_[r] = false;
        int w = 0;
        for (int i = 0; i < evN_; ++i) if (!(evq_[i].row == r && evq_[i].type == 0)) evq_[w++] = evq_[i];
        evN_ = w;
        // the sounding note (its off is not queued yet) ends now; queued offs stay (an off for a
        // dropped note-on only lowers a count that is above 0)
        if (rowNote_[r] >= 0) { Ev off; off.t = nowP_; off.type = 1; off.a = (uint8_t)rowNote_[r]; emit(off); rowNote_[r] = -1; }
        stepFifo_[r] = StepFifo();
        dispStep_[r] = -1;
        for (int l = 0; l < kCcLanes; ++l) lnIdx_[r][l] = -1;
    }

    // 1.3: the CC lanes of row r at song position p: a new step sends its value when it differs from
    // the last value sent on that CC (queued just before p, so a lock at the same time comes after it)
    void tickLanes(int r, double p)
    {
        const Row& w = playRow(r);
        for (int l = 0; l < kCcLanes; ++l)
        {
            const CcLane& L = w.lane[l];
            if (L.cc == 0) { lnIdx_[r][l] = -1; continue; }
            const double sq = stepQOf(L.rate, L.mode, L.ratio, L.fine);
            const long n = (long)std::floor((p - lnAnchor_[r]) / sq + 1e-9);
            if (n == lnN_[r][l]) continue;
            lnN_[r][l] = n;
            const int steps = std::max(1, (int)L.steps);
            const int m = (int)(((n % steps) + steps) % steps);
            int idx = m;
            if (L.dir == 1) idx = steps - 1 - m;
            else if (L.dir == 2) { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; idx = (int)(rng_ % (uint32_t)steps); }
            lnIdx_[r][l] = idx;
            const int v = L.val[idx];
            if (ccSent_[L.cc] != v) { queue(p - 1e-7, 2, L.cc, (uint8_t)v, -1); ccSent_[L.cc] = v; }
        }
    }

    void tickRow(int r, double e, bool running)
    {
        RowOut o = engines_[r].tick(phaseOf(r, e), running, tab(tIdx_, r), sctx_);
        if (o.step != lastStep_[r])
        {
            lastStep_[r] = o.step;
            StepFifo& f = stepFifo_[r];
            if (f.n < kFifo) { const int w = (f.head + f.n) % kFifo; f.t[w] = e; f.step[w] = o.step; ++f.n; }
#ifdef PERCSEQ_TEST
            testSteps_.push_back({e, r, o.step});
#endif
        }
        if (o.off && rowNote_[r] >= 0) { queue(e, 1, (uint8_t)rowNote_[r], 0, r); rowNote_[r] = -1; }
        if (o.start)
        {
            const Fire& f = o.fire;
            const int dest = std::max(0, std::min(kRows - 1, f.sound));
            const Row& home = playRow(r);
            const Row& dr = playRow(dest);
            curSound_[r] = dest;
            curVel_[r] = home.vel / 100.0f;
            sctx_.lastPlayed = dest;
            sctx_.lastHome = r;
            if (curVel_[r] <= 0 || !dr.on) { curVel_[r] = 0; return; }   // Velocity 0 % or a hit on an off row: nothing
            // FLAM: a grace hit before the hit (half its velocity, the same sound); dropped if its
            // time has passed (the first hit after a restart)
            double tMsg = e;
            if (f.flam > 0)
            {
                const double tg = e - f.flam / 96.0 * stepQ(r);
                if (tg >= nowP_ - 1e-9) tMsg = tg;
            }
            // SMP REV and CC of the sound's row, before the hit (only when the state changes)
            if (f.rev && revState_[dest] != 127) { queue(tMsg, 2, (uint8_t)dr.revCC, 127, -1); revState_[dest] = 127; }
            else if (!f.rev && revState_[dest] == 127) { queue(tMsg, 2, (uint8_t)dr.revCC, 0, -1); revState_[dest] = 0; }
            if (f.cc >= 0) { if (ccState_[dest] != f.cc) { queue(tMsg, 2, (uint8_t)dr.ccNum, (uint8_t)f.cc, -1); ccState_[dest] = f.cc; } }
            else if (ccState_[dest] >= 0) { queue(tMsg, 2, (uint8_t)dr.ccNum, (uint8_t)dr.ccRest, -1); ccState_[dest] = -1; }
            // 1.3: the hit's locks, on the Lock CCs of the sound's row (always sent, they win over a lane)
            if (f.hit >= 0 && f.hit < home.count)
                for (int k = 0; k < 4; ++k)
                {
                    const int lv = home.hits[f.hit].lock[k], cc = dr.lockCC[k];
                    if (lv >= 0 && cc > 0) { queue(tMsg, 2, (uint8_t)cc, (uint8_t)lv, -1); ccSent_[cc] = lv; }
                }
            if (tMsg < e)
            {
                const int gv = velByte(o.vel * 0.5f * curVel_[r]);
                queue(tMsg, 0, (uint8_t)dr.note, (uint8_t)gv, -1);
                queue(e - 1e-7, 1, (uint8_t)dr.note, 0, -1);
            }
        }
        if (o.on && curVel_[r] > 0)
        {
            const int dest = curSound_[r];
            const int note = playRow(dest).note;
            queue(e, 0, (uint8_t)note, (uint8_t)velByte(o.vel * curVel_[r]), r);
            rowNote_[r] = note;
        }
    }
    static int velByte(float v01) { const int v = (int)std::lround(v01 * 127.0f); return v < 1 ? 1 : (v > 127 ? 127 : v); }

    // queue of timed messages (sorted by time; equal times keep their order)
    void queue(double t, uint8_t type, uint8_t a, uint8_t b, int row)
    {
        if (evN_ >= kMaxEv) return;
        int i = evN_;
        while (i > 0 && evq_[i - 1].t > t) { evq_[i] = evq_[i - 1]; --i; }
        evq_[i].t = t; evq_[i].type = type; evq_[i].a = a; evq_[i].b = b; evq_[i].row = (int8_t)row;
        ++evN_;
    }
    bool emitDue(double p)
    {
        nowP_ = p;
        int k = 0;
        while (k < evN_ && evq_[k].t <= p + 1e-9) { emit(evq_[k]); ++k; }
        if (k == 0) return false;
        for (int i = k; i < evN_; ++i) evq_[i - k] = evq_[i];
        evN_ -= k;
        return true;
    }
    void emit(const Ev& e)
    {
        const uint8_t ch = (uint8_t)(std::max(1, std::min(16, getInt(kOutCh))) - 1);
        if (e.type == 0)
        {
            // the same note from two rows: the second note-on comes after a note-off
            if (noteCnt_[e.a] > 0) sendNow((uint8_t)(0x80 | ch), e.a, 0);
            ++noteCnt_[e.a];
            sendNow((uint8_t)(0x90 | ch), e.a, e.b);
#ifdef PERCSEQ_TEST
            testEmits_.push_back({e.t, nowP_, 0, e.a, e.b});
#endif
        }
        else if (e.type == 1)
        {
            if (noteCnt_[e.a] > 0 && --noteCnt_[e.a] == 0) sendNow((uint8_t)(0x80 | ch), e.a, 0);
#ifdef PERCSEQ_TEST
            testEmits_.push_back({e.t, nowP_, 1, e.a, 0});
#endif
        }
        else
        {
            ccSent_[e.a & 127] = e.b;
            sendNow((uint8_t)(0xB0 | ch), e.a, e.b);
#ifdef PERCSEQ_TEST
            testEmits_.push_back({e.t, nowP_, 2, e.a, e.b});
#endif
        }
    }
    // all sounding notes off and the queue emptied (stop, jump, switch)
    void flushOutput()
    {
        const uint8_t ch = (uint8_t)(std::max(1, std::min(16, getInt(kOutCh))) - 1);
        for (int n = 0; n < 128; ++n)
            if (noteCnt_[n] > 0) { sendNow((uint8_t)(0x80 | ch), (uint8_t)n, 0); noteCnt_[n] = 0; }
        evN_ = 0;
        for (int& n : rowNote_) n = -1;
    }
    void sendNow(uint8_t a, uint8_t b, uint8_t c) { ring_.push(a, b, c, 0); }
    // on stop: reversed pads back to forward, CCs back to their rest value
    void restCCs()
    {
        const uint8_t ch = (uint8_t)(std::max(1, std::min(16, getInt(kOutCh))) - 1);
        for (int r = 0; r < kRows; ++r)
        {
            const Row& w = playRow(r);
            if (revState_[r] == 127) { sendNow((uint8_t)(0xB0 | ch), (uint8_t)w.revCC, 0); revState_[r] = 0; }
            if (ccState_[r] >= 0) { sendNow((uint8_t)(0xB0 | ch), (uint8_t)w.ccNum, (uint8_t)w.ccRest); ccState_[r] = -1; }
            for (const CcLane& L : w.lane)   // 1.3: lanes to their Rest value
                if (L.cc > 0) sendNow((uint8_t)(0xB0 | ch), L.cc, L.rest);
            for (int l = 0; l < kCcLanes; ++l) lnIdx_[r][l] = -1;
        }
        for (int& c : ccSent_) c = -1;   // the first lane step after a stop always sends
    }

    // SND NEXT: the row whose next hit starts first from now (forward, inside its play range;
    // directions, jumps and chance are not looked at)
    static int nextRowThunk(void* u) { return static_cast<PercSeq*>(u)->nextRow(); }
    int nextRow() const
    {
        int best = -1;
        double bestQ = 1e18;
        for (int r = 0; r < kRows; ++r)
        {
            const Row& w = playRow(r);
            if (w.count == 0) continue;
            const double pos = engines_[r].posSteps;
            const int lo = std::max(0, std::min(w.steps - 1, w.rStart)), hi = std::max(lo, std::min(w.steps - 1, w.rEnd));
            const double span = hi - lo + 1;
            double bestD = 1e18;
            for (int i = 0; i < w.count; ++i)
            {
                const double st = w.hits[i].st;
                if (st < lo - kEps || st >= hi + 1 - kEps) continue;
                double d = st - pos;
                if (d <= 1e-4) d += span;
                bestD = std::min(bestD, d);
            }
            if (bestD < 1e17 && bestD * stepQ(r) < bestQ) { bestQ = bestD * stepQ(r); best = r; }
        }
        return best;
    }

    // ---- edits ------------------------------------------------------------------------------------
    void pollEdits()
    {
        for (int i = kFirstEdit; i <= kLastEdit; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
#ifdef PERCSEQ_TRACE
            psqtrace::put(psqtrace::K_MARK, 1, i, psqtrace::nowNs());
#endif
            onEdit(i, plainOf(i, v));
            snapChoice(i, v);
            closeList(i);
#ifdef PERCSEQ_TRACE
            { float after = getNormalised(i); uint32_t bits; std::memcpy(&bits, &after, 4);
              const uint64_t t = psqtrace::nowNs(); psqtrace::put(psqtrace::K_MARK, 2, i, t);
              (void)bits; }
#endif
        }
        for (int i = kLane0; i < kClearAll + 1; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            onEdit(i, plainOf(i, v));
            snapChoice(i, v);
            closeList(i);
        }
        for (int i = kPagePrev; i <= kSelRowNext; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            onEdit(i, plainOf(i, v));
        }
        for (int i = kRMode; i <= kLnRest; ++i)   // 1.3: RATE, LOCK and LANE settings
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            onEdit(i, plainOf(i, v));
            snapChoice(i, v);
            closeList(i);
        }
        for (int i = kOn0; i < kOn0 + kRows; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            ed().rows[i - kOn0].on = v >= 0.5f;   // the playing rows follow in process()
            savedDirty_ = true;
            viewDirty_ = true;
        }
        if (clearArmed_ && msNow() - clearArmMs_ > 3000) { clearArmed_ = false; echo(kClearAll, 0); }   // not confirmed: disarm
        // 1.3: COPY, PASTE, CLEAR of the edited sequence
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
        // 1.3: SEND: on a touch the CC goes out at 64; the button stays lit for 150 ms
        for (int i = kSend0; i < kSend0 + kNumSends; ++i)
        {
            const float v = getNormalised(i);
            if (v == lastSeen_[i]) continue;
            lastSeen_[i] = v;
            if (v >= 0.5f) sendLearn(i - kSend0);                       // tap: start (lit)
            else if (burstSend_ == i - kSend0) stopBurst(false);         // tap again: stop
        }
        tickBurst();
        pollTouch(0, kRollTX); pollTouch(1, kRulTX); pollTouch(2, kLabTX); pollTouch(3, kVelTX); pollTouch(4, kFreeTX);
    }

    // 1.1: a Q-Link writes values between two choices (measured on the MPC One G2: steps of 1/128 for
    // Op 1, which has 32 choices), and the MPC then showed a name one choice away from the one
    // percSeq used. After the edit, write the exact value of the choice percSeq took, so both show
    // the same name. (Only if the edit itself did not write the parameter.)
    void snapChoice(int i, float before)
    {
        const int n = (int)param(i).choices.size();
        if (n < 2 || getNormalised(i) != before) return;
        echo(i, (float)choiceAt(i));
    }

    // 1.1: a pick in a drawn list (or a Q-Link turn of its parameter) closes the open list
    static bool isListParam(int i)
    {
        if (i == kEntryLen || i == kRRate || i == kRRatio || i == kLnRate || i == kLnRatio) return true;
        return i >= kLane0 && i < kPool0 && ((i - kLane0) % 4 == 0 || (i - kLane0) % 4 == 2);
    }
    void closeList(int i) { if (isListParam(i) && getInt(kListOpen) != 0) echo(kListOpen, 0); }

    void textNotify(int i) { notify_[i].store(true, std::memory_order_release); notifyPending_ = true; }

    // Value texts: report a text field only when what it shows changed (1.1: each redraw reported
    // all 7, measured offline: about 7 of the 10 reports per velocity-lane drag update).
    void updateTexts()
    {
        Row& s = row();
        Hit* h = selHit();
        auto put = [&](std::atomic<int>& a, int v) { return a.exchange(v) != v ? 1 : 0; };   // int: every field is updated
        const bool selCh = put(tSel_, h ? sel_ + 1 : 0) | put(tCount_, s.count);
        if (selCh) textNotify(kHitSel);
        for (int l = 0; l < kLanes; ++l)
        {
            int eff = 0;
            if (h)
            {
                const Cell& c = h->ch[l];
                const Unit u = opInfo(c.op).unit;
                if (u == U_GATE) eff = c.amt;
                else if (u != U_NONE) eff = (int)std::lround(opValue(c, s));
            }
            const bool ch = put(tChOp_[l], h ? h->ch[l].op : 0) | put(tChProb_[l], h ? (int)std::lround(h->ch[l].prob * 100) : 0)
                          | put(tChAmt_[l], eff);
            if (ch || selCh) textNotify(kChanceText0 + l);
        }
        if (put(tView0_, viewStart_) | put(tZoom_, (int)zoom()) | put(tPages_, pages())) textNotify(kViewText);
        for (int r = 0; r < kRows; ++r) if (put(tRowNote_[r], ed().rows[r].note)) textNotify(kLabText0 + r);
    }

    void setRT(int i, float norm)
    {
        if (getNormalised(i) == norm) return;
        setNormalised(i, norm);
        lastSeen_[i] = norm;
        notify_[i].store(true, std::memory_order_release);
        notifyPending_ = true;
    }
    void echo(int i, float plain) { setRT(i, param(i).toNormalised(plain)); }
    // Display parameters: written when the computed value changes (see scalaSeq).
    void setDisp(int i, float norm)
    {
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
        Hit* h = selHit();
        for (int l = 0; l < kLanes; ++l) laneOp_[l].store(h ? h->ch[l].op : 0);
    }

    // the knobs show the selected hit
    void echoHit()
    {
        viewDirty_ = true;
        Hit* h = selHit();
        if (!h) { for (int l = 0; l < kLanes; ++l) laneOp_[l].store(0); for (int k = 0; k < 4; ++k) echo(kLock0 + k, -1); echoKnobs(); return; }
        for (int k = 0; k < 4; ++k) echo(kLock0 + k, (float)h->lock[k]);   // 1.3
        echo(kVel, h->v * 100); echo(kRatch, h->r); echo(kFade, h->fd); echo(kDuty, h->d);
        for (int l = 0; l < kLanes; ++l)
        {
            const Cell& c = h->ch[l];
            laneOp_[l].store(c.op);
            echo(kLane0 + l * 4, choiceOfOp(c.op)); echo(kLane0 + l * 4 + 1, c.prob * 100);
            echo(kLane0 + l * 4 + 2, c.ptn); echo(kLane0 + l * 4 + 3, c.amt);
        }
        echoKnobs();
    }
    void echoKnobs()
    {
        echo(kSequence, (float)(edSeq_ + 1));
        echo(kViewStart, (float)(page() + 1));
        echo(kSelRow, (float)(selRow_ + 1));
        echo(kHitRow, (float)(selRow_ + 1));
        Hit* h = selHit();
        if (!h) return;
        const float q = snapQ();
        const float base = h->st - h->nd / 96.0f;   // the Move position
        echo(kHitSel, (float)(sel_ + 1));
        echo(kMove, (float)std::lround(base / q));
        echo(kLen, (float)std::max(1L, std::lround(h->len / q)));
        echo(kNudge, (float)h->nd);
    }
    // the ROW page shows the selected row
    void echoRow()
    {
        const Row& w = row();
        echo(kRNote, (float)w.note); echo(kRVel, w.vel); echo(kRRate, (float)w.rate); echo(kRSteps, (float)w.steps);
        echo(kRStart, (float)(w.rStart + 1)); echo(kREnd, (float)(w.rEnd + 1)); echo(kRDir, (float)w.dir);
        echo(kRSwing, w.swing); echo(kRevCC, (float)w.revCC); echo(kCCNum, (float)w.ccNum); echo(kCCRest, (float)w.ccRest);
        echo(kSelRow, (float)(selRow_ + 1));
        echo(kRMode, (float)w.mode); echo(kRRatio, (float)w.ratio); echo(kRFine, (float)w.fine); textNotify(kRFine);   // 1.3
        for (int k = 0; k < 4; ++k) echo(kLockCC0 + k, (float)indexOfCC(w.lockCC[k]));
        echoLane();
        echo(kCursor, cursor_);
        for (int r = 0; r < kRows; ++r) echo(kOn0 + r, ed().rows[r].on ? 1.0f : 0.0f);   // 1.1: the edited sequence's rows
        viewDirty_ = true;
    }
    // 1.3: the LANE settings show the shown CC lane of the selected row
    void echoLane()
    {
        const int ls = choiceAt(kLaneSel);
        if (ls <= 0) return;
        const CcLane& L = row().lane[ls - 1];
        echo(kLnCC, (float)indexOfCC(L.cc)); echo(kLnSteps, (float)L.steps); echo(kLnRate, (float)L.rate);
        echo(kLnMode, (float)L.mode); echo(kLnRatio, (float)L.ratio); echo(kLnFine, (float)L.fine);
        echo(kLnDir, (float)L.dir); echo(kLnRest, (float)L.rest);
        textNotify(kLnFine);
    }
    void selectRow(int r)
    {
        r = std::max(0, std::min(kRows - 1, r));
        if (r != selRow_) { selRow_ = r; sel_ = row().count > 0 ? 0 : -1; }
        echoRow();
        echoHit();
    }

    // keep the selected hit on screen
    void keepVisible()
    {
        Hit* h = selHit();
        if (!h) return;
        setPage((int)std::floor((h->st + kEps) / zoom()));
    }
    // 1.1: the view shows whole pages of Steps/Page steps, up to the longest row
    int pages() const { return std::max(1, (maxSteps() + (int)zoom() - 1) / (int)zoom()); }
    void setPage(int pg)
    {
        pg = std::max(0, std::min(pages() - 1, pg));
        const int vs = pg * (int)zoom();
        if (vs != viewStart_) { viewStart_ = vs; viewDirty_ = true; }
        echo(kViewStart, (float)(pg + 1));
    }
    int page() const { return viewStart_ / std::max(1, (int)zoom()); }

    // move hit i of the selected row to start st (keeps the list sorted); returns its new index
    int moveHit(int i, float st)
    {
        Row& s = row();
        Hit moved = s.hits[i];
        moved.st = st;
        s.remove(i);
        return s.insert(moved);
    }
    // move the selected hit to row r (same start; length clipped to that row); false if it does not fit
    // 1.3: COPY takes the edited sequence (all its data); PASTE (armed, then a 2nd touch within 3 s)
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
    // 1.3: SEND k: 0-3 Lock CC 1-4 of the selected row, 4 the shown lane, 5 Rev CC, 6 CC Num (nothing while off)
    void sendLearn(int k)
    {
        const Row& w = row();
        int cc = 0;
        if (k < 4) cc = w.lockCC[k];
        else if (k == 4) { const int ls = choiceAt(kLaneSel); cc = ls > 0 ? w.lane[ls - 1].cc : 0; }
        else cc = k == 5 ? w.revCC : w.ccNum;
        if (k < 5 && cc <= 0) { echo(kSend0 + k, 0); return; }   // the CC is off: nothing
        // measured 2026-11-22: MIDI Learn ignores a single CC, learns a moving CC, and turns off when its
        // page is left; so SEND keeps sending until it is touched again (or 15 s), then sends 64
        if (burstSend_ >= 0) stopBurst(true);
        burstSend_ = k; burstCC_ = cc & 127; burstStep_ = 0; burstMs_ = msNow();
        tickBurst();
    }
    void burstOut(int v)
    {
        const uint8_t ch = (uint8_t)(std::max(1, std::min(16, getInt(kOutCh))) - 1);
        sendNow((uint8_t)(0xB0 | ch), (uint8_t)burstCC_, (uint8_t)v);
        ccSent_[burstCC_] = v;
        previewSent_ = true;   // wake the MIDI thread
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
        for (int r = 0; r < kRows; ++r) markRow(r);
        sel_ = row().count > 0 ? 0 : -1;
        echoRow();
        echoHit();
        setPage(page());
        savedDirty_ = true;
        viewDirty_ = true;
    }

    // 1.2: the < > step buttons. Each stops at the ends. Move and Row skip places where the hit does not fit
    // and go to the next free one in that direction.
    void stepButton(int i)
    {
        Row& s = row();
        Hit* h = selHit();
        const int dir = (i == kHitNext || i == kRowNext || i == kMoveNext || i == kLenNext || i == kSelRowNext) ? 1 : -1;
        switch (i)
        {
            case kHitPrev: case kHitNext:
                if (s.count > 0) { sel_ = std::max(0, std::min(s.count - 1, sel_ + dir)); echoHit(); keepVisible(); }
                break;
            case kRowPrev: case kRowNext:
                if (!h) break;
                for (int r = selRow_ + dir; r >= 0 && r < kRows; r += dir)
                    if (moveToRow(r, h->st)) { echoRow(); echoHit(); preview(); break; }
                break;
            case kMovePrev: case kMoveNext:
            {
                if (!h) break;
                const float q = snapQ();
                const float nd = h->nd / 96.0f;
                const long b = std::lround((h->st - nd) / q);
                for (long c = b + dir; ; c += dir)
                {
                    const float st = c * q + nd;
                    if (st < -kEps || st + h->len > s.steps + kEps) break;   // the row start or end
                    if (s.fits(std::max(0.0f, st), h->len, sel_)) { onEdit(kMove, (float)c); break; }
                }
                break;
            }
            case kLenPrev: case kLenNext:
                if (h) onEdit(kLen, (float)(std::max(1L, std::lround(h->len / snapQ())) + dir));
                break;
            case kSelRowPrev: case kSelRowNext:   // 1.3: RATE and LOCK: select the row (stops at rows 1 and 16)
                selectRow(selRow_ + dir); keepVisible();
                break;
            default: break;
        }
    }

    bool moveToRow(int r, float st)
    {
        if (r == selRow_) return false;
        Hit* h = selHit();
        if (!h) return false;
        Row& to = rowOf(r);
        if (st >= to.steps - kEps) return false;
        const float len = std::min(h->len, to.steps - st);
        if (len < kMinLen || !to.fits(st, len, -1) || to.count >= kMaxHits) return false;
        Hit moved = *h;
        moved.st = st; moved.len = len;
        row().remove(sel_);
        markRow(selRow_);
        selRow_ = r;
        sel_ = to.insert(moved);
        markRow(r);
        return true;
    }

    void onEdit(int i, float v)
    {
        trustRestored_ = false;
        Row& s = row();
        Hit* h = selHit();
        const int iv = (int)std::lround(v);
        switch (i)
        {
            case kSequence: setEdSeq(std::max(0, std::min(kSeqs - 1, iv - 1))); break;
            case kSelRow: selectRow(iv - 1); keepVisible(); break;
            case kHitRow:
                if (h && iv - 1 != selRow_ && moveToRow(std::max(0, std::min(kRows - 1, iv - 1)), h->st)) { echoRow(); echoHit(); preview(); }
                else echo(kHitRow, (float)(selRow_ + 1));   // no hit, or it does not fit there
                break;
            case kRNote: s.note = std::max(0, std::min(127, iv)); viewDirty_ = true; savedDirty_ = true; break;
            case kRVel: s.vel = std::max(0.0f, std::min(100.0f, v)); savedDirty_ = true; break;
            // choices: the parameter is written back on the exact choice, so it always equals the row's value
            case kRRate: s.rate = std::max(0, std::min(kNumRates - 1, iv)); echo(kRRate, (float)s.rate); savedDirty_ = true; break;
            case kRDir: s.dir = std::max(0, std::min(2, iv)); echo(kRDir, (float)s.dir); savedDirty_ = true; break;
            case kRevCC: s.revCC = std::max(0, std::min(127, iv)); savedDirty_ = true; break;
            case kCCNum: s.ccNum = std::max(0, std::min(127, iv)); savedDirty_ = true; break;
            case kCCRest: s.ccRest = std::max(0, std::min(127, iv)); savedDirty_ = true; break;
            case kRSteps:
                s.steps = std::max(1, std::min(kMaxSteps, iv));
                s.fitToSteps();
                sel_ = std::min(sel_, s.count - 1);
                echo(kRStart, s.rStart + 1); echo(kREnd, s.rEnd + 1);
                markRow(selRow_); echoHit();
                break;
            case kRStart: s.rStart = std::max(0, std::min(s.steps - 1, iv - 1)); s.rEnd = std::max(s.rEnd, s.rStart);
                echo(kREnd, s.rEnd + 1); markRow(selRow_); break;
            case kREnd: s.rEnd = std::max(0, std::min(s.steps - 1, iv - 1)); s.rStart = std::min(s.rStart, s.rEnd);
                echo(kRStart, s.rStart + 1); markRow(selRow_); break;
            case kRSwing: s.swing = std::max(50.0f, std::min(75.0f, v)); markRow(selRow_); break;
            case kSnap: echoHit(); break;
            case kPadNote:
                if (!padNoteOk(iv)) { const int nv = iv > padNote_ ? kPadHighMin : kPadLowMax; echo(kPadNote, (float)nv); padNote_ = nv; }
                else padNote_ = iv;
                viewDirty_ = true;
                break;
            case kOutCh: flushOutput(); break;   // notes on the old channel end
            case kGroup: case kStepEntry: viewDirty_ = true; break;
            case kZoom: setPage(viewStart_ / (int)zoom()); keepVisible(); viewDirty_ = true; break;
            case kHitSel:
                if (s.count > 0) { sel_ = std::max(0, std::min(s.count - 1, iv - 1)); echoHit(); keepVisible(); }
                break;
            case kMove:
                if (h)
                {
                    // 1.1: the Move position is on a snap line; Nudge stays as it is
                    const float q = snapQ();
                    const float st = std::max(0.0f, std::min((float)s.steps - h->len, iv * q + h->nd / 96.0f));   // end stops at the row end
                    if (std::fabs(st - h->st) > kEps && s.fits(st, h->len, sel_)) { sel_ = moveHit(sel_, st); markRow(selRow_); }
                    echoHit();
                    keepVisible();
                }
                break;
            case kLen:
                if (h)
                {
                    const float q = snapQ();
                    const float len = std::max(kMinLen, std::min(iv * q, s.maxLen(h->st, sel_)));
                    if (std::fabs(len - h->len) > kEps) { h->len = len; markRow(selRow_); }
                    echo(kLen, (float)std::max(1L, std::lround(h->len / q)));
                }
                break;
            case kNudge:
                if (h)
                {
                    // 1.1: -96..96 (one step either way) from the hit's Move position, whatever Snap is
                    const float base = h->st - h->nd / 96.0f;
                    const int nd = std::max(-96, std::min(96, iv));
                    const float st = base + nd / 96.0f;
                    if (st >= -kEps && st + h->len <= s.steps + kEps && s.fits(std::max(0.0f, st), h->len, sel_))
                    {
                        if (std::fabs(st - h->st) > kEps) sel_ = moveHit(sel_, std::max(0.0f, st));
                        row().hits[sel_].nd = (int16_t)nd;
                        markRow(selRow_);
                    }
                    echo(kNudge, (float)row().hits[sel_].nd);
                }
                break;
            case kViewStart: setPage(iv - 1); break;
            case kPagePrev: case kPageNext:
                if (v >= 0.5f) { setPage(page() + (i == kPageNext ? 1 : -1)); echo(i, 0); }
                break;
            case kFollow: case kListOpen: break;
            // 1.3: rate mode of the selected row
            case kRMode: s.mode = std::max(0, std::min(1, iv)); echo(kRMode, (float)s.mode); savedDirty_ = true; break;
            case kRRatio: s.ratio = std::max(0, std::min(kNumRatios - 1, iv)); echo(kRRatio, (float)s.ratio); savedDirty_ = true; textNotify(kRFine); break;
            case kRFine: s.fine = std::max(-10, std::min(10, iv)); savedDirty_ = true; textNotify(kRFine); break;
            // 1.3: locks of the selected hit, lock CCs of the selected row
            case kLock0: case kLock0 + 1: case kLock0 + 2: case kLock0 + 3:
                if (h) { h->lock[i - kLock0] = (int8_t)std::max(-1, std::min(127, iv)); markRow(selRow_); }
                else echo(i, -1);
                break;
            case kLockCC0: case kLockCC0 + 1: case kLockCC0 + 2: case kLockCC0 + 3:
                s.lockCC[i - kLockCC0] = (uint8_t)ccOfIndex(iv); savedDirty_ = true;
                break;
            // 1.3: LANE
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
                    case kLnRate: L.rate = (uint8_t)std::max(0, std::min(kNumRates - 1, iv)); break;
                    case kLnMode: L.mode = (uint8_t)std::max(0, std::min(1, iv)); break;
                    case kLnRatio: L.ratio = (uint8_t)std::max(0, std::min(kNumRatios - 1, iv)); textNotify(kLnFine); break;
                    case kLnFine: L.fine = (int8_t)std::max(-10, std::min(10, iv)); textNotify(kLnFine); break;
                    case kLnDir: L.dir = (uint8_t)std::max(0, std::min(2, iv)); break;
                    case kLnRest: L.rest = (uint8_t)std::max(0, std::min(127, iv)); break;
                }
                savedDirty_ = true;
                break;
            }
            case kHitPrev: case kHitNext: case kRowPrev: case kRowNext:
            case kMovePrev: case kMoveNext: case kLenPrev: case kLenNext: case kSelRowPrev: case kSelRowNext:
                if (v >= 0.5f) { stepButton(i); echo(i, 0); }
                break;
            case kVel: if (h) { h->v = v / 100; markRow(selRow_); } break;
            case kRatch: if (h) { h->r = (int8_t)iv; markRow(selRow_); } break;
            case kFade: if (h) { h->fd = (int8_t)iv; markRow(selRow_); } break;
            case kDuty: if (h) { h->d = v; markRow(selRow_); } break;
            case kCursor: cursor_ = std::max(0.0f, std::min((float)maxSteps() - snapQ(), snapTo(v))); viewDirty_ = true; break;
            case kClearNote:
                if (v >= 0.5f)
                {
                    if (h) { for (Cell& c : h->ch) c = Cell(); markRow(selRow_); }
                    echo(kClearNote, 0);
                    echoHit();
                }
                break;
            case kClearAll:
                if (v >= 0.5f) { clearArmed_ = true; clearArmMs_ = msNow(); }   // armed: the switch stays lit
                else if (clearArmed_)
                {
                    clearArmed_ = false;   // second touch within 3 s: clear every chance op of the sequence
                    for (int r = 0; r < kRows; ++r)
                    {
                        Row& w = rowOf(r);
                        for (int k = 0; k < w.count; ++k) for (Cell& c : w.hits[k].ch) c = Cell();
                        markRow(r);
                    }
                    echoHit();
                }
                break;
            case kDelete:
                if (v >= 0.5f) { deleteSelected(); echo(kDelete, 0); }
                break;
            default:
                if (i >= kLane0 && i < kPool0 && h)
                {
                    const int l = (i - kLane0) / 4, f = (i - kLane0) % 4;
                    Cell& c = h->ch[l];
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
                        const bool poolZero = c.op == O_SNDRP || c.op == O_SNDRM || c.op == O_SNDR;
                        const int lo = poolZero ? 0 : o.lo;
                        const int a = std::max(lo, std::min(o.hi, iv));
                        c.amt = (uint8_t)a;
                        if (a != iv) echo(i, a);
                    }
                    markRow(selRow_);
                }
                else if (i >= kPool0 && i < kPool0 + kRows)
                {
                    model_->pool[i - kPool0] = v >= 0.5f;
                    pool_.set(model_->pool);
                    savedDirty_ = true;
                }
                break;
        }
    }

    void deleteSelected()
    {
        Row& s = row();
        if (sel_ < 0 || sel_ >= s.count) return;
        s.remove(sel_);
        sel_ = std::min(sel_, s.count - 1);
        markRow(selRow_);
        echoHit();
    }

    // a short preview of the selected hit's row (touch and knob edits)
    void preview()
    {
        if (!selHit()) return;
        const uint8_t ch = (uint8_t)(std::max(1, std::min(16, getInt(kOutCh))) - 1);
        if (previewNote_ >= 0) sendNow((uint8_t)(0x80 | ch), (uint8_t)previewNote_, 0);
        previewNote_ = row().note;
        sendNow((uint8_t)(0x90 | ch), (uint8_t)previewNote_, (uint8_t)velByte(selHit()->v * row().vel / 100.0f));
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
    float touchQ() const { const float q = snapQ(); return q * (kGW / zoom()) >= kMinLinePx ? q : 1.0f; }
    float snapOrFree(float pos) const { const float q = touchQ(); return freeHeld() ? std::round(pos * 96.0f) / 96.0f : std::round(pos / q) * q; }
    float minLen() const { return freeHeld() ? kMinLen : touchQ(); }
    static int rowAtY(float yUp) { return std::max(0, std::min(kRows - 1, (int)std::floor(yUp / kRowH))); }

    void onPress(int t, Touch& tc)
    {
        trustRestored_ = false;
        if (t == 4) { setDisp(kFreeLit, xPark(0, kFreeW, true)); return; }
        if (t == 1) { gView0_ = viewStart_; gZoom0_ = (int)zoom(); gAxis_ = 0; gFlipped_ = false; return; }
        if (t == 2)   // 1.1: a row label switches the row on or off (the playing rows follow in process())
        {
            const int r = rowAtY(padPx(tc.y, kGH));
            ed().rows[r].on = !ed().rows[r].on;
            echo(kOn0 + r, ed().rows[r].on ? 1.0f : 0.0f);
            savedDirty_ = true;
            viewDirty_ = true;
            return;
        }
        if (t == 3 && choiceAt(kLaneSel) > 0) { gLnLast_ = -1; drawLane(tc); return; }   // 1.3: CC lane
        if (t == 3)
        {
            // the velocity bar of the selected row nearest to the finger (within 14 px)
            Row& s = row();
            const float fx = padPx(tc.x, kGW);
            int best = -1;
            float bd = 14;
            for (int i = 0; i < s.count; ++i)
            {
                const float d = std::fabs(pxOfPos(s.hits[i].st) + kVelW * 0.5f - fx);
                if (s.hits[i].st >= viewStart_ - kEps && s.hits[i].st < viewStart_ + zoom() - kEps && d < bd) { bd = d; best = i; }
            }
            gVelHit_ = best;
            if (best >= 0) { sel_ = best; setVelFromTouch(tc); echoHit(); }
            return;
        }
        // the roll
        const float fx = rollX(tc), pos = posOfPx(fx);
        const int r = rowAtY(rollYUp(tc));
        Row& s = rowOf(r);
        int hit = -1;
        for (int i = s.count - 1; i >= 0; --i)
        {
            const Hit& h = s.hits[i];
            const float a = pxOfPos(h.st), b = pxOfPos(h.st + h.len);
            if (fx >= a - 4 && fx < std::max(b, a + 12)) { hit = i; break; }
        }
        gDelete_ = false;
        if (hit >= 0)
        {
            const Hit& h = s.hits[hit];
            const float a = pxOfPos(h.st), b = pxOfPos(h.st + h.len);
            const float edge = std::max(12.0f, std::min(24.0f, (b - a) / 3));
            gMode_ = (fx >= b - edge && b - a >= 12) ? 3 : 2;   // 3 length, 2 move
            if (hit == lastTapHit_ && r == lastTapRow_ && msNow() - lastTapMs_ <= kDoubleTapMs) gDelete_ = true;
            if (r != selRow_) { selRow_ = r; echoRow(); }
            sel_ = hit;
            gOrig_ = h;
            gOrigRow_ = r;
            echoHit();
            preview();
            return;
        }
        // empty: a new hit on the nearest snap line (or exactly, while FREE is held)
        float st = std::max(0.0f, snapOrFree(pos));
        st = std::max(st, s.minStart(st + kEps, -1));   // not inside a hit
        if (st >= s.steps - kEps || s.maxLen(st, -1) < kMinLen) { gMode_ = 0; return; }
        const float elen = kEntryLens[std::max(0, std::min(8, choiceAt(kEntryLen)))];
        Hit nh;
        const float room = s.maxLen(st, -1);
        nh.st = st; nh.len = std::min(minLen(), room);   // a tap gets Entry Len at release
        gEntryLen_ = std::min(elen, room);
        const int idx = s.insert(nh);
        if (idx < 0) { gMode_ = 0; return; }
        if (r != selRow_) { selRow_ = r; echoRow(); }
        sel_ = idx;
        gMode_ = 1;
        gAnchor_ = st;
        gOrig_ = nh;
        gOrigRow_ = r;
        markRow(r);
        echoHit();
        preview();
    }

    void onDrag(int t, Touch& tc)
    {
        if (t == 4 || t == 2) return;
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
                // 1.1: a swipe flips one page (finger to the left = the next page), once per swipe
                if (!gFlipped_ && std::fabs(dx) >= kSwipePx) { setPage(gView0_ / (int)zoom() + (dx < 0 ? 1 : -1)); gFlipped_ = true; }
            }
            else
            {
                int c0 = 0;
                while (c0 < kZoomN - 1 && (8 << c0) < gZoom0_) ++c0;
                const int cls = std::max(0, std::min(kZoomN - 1, c0 + (int)std::lround(dyDown / kZoomPx)));
                const int z = 8 << cls;
                if (z != (int)zoom())
                {
                    const float fx0 = padPx(tc.x0, kGW);
                    const float anchor = gView0_ + fx0 / kGW * gZoom0_;   // the step under the finger stays
                    echo(kZoom, (float)cls);
                    setPage((int)std::floor(anchor / z));   // the page that holds the step under the finger
                    viewDirty_ = true;
                }
            }
            return;
        }
        if (t == 3) { if (choiceAt(kLaneSel) > 0) drawLane(tc); else if (gVelHit_ >= 0) setVelFromTouch(tc); return; }
        // the roll
        Row& s = row();
        if (gMode_ == 0 || sel_ < 0 || sel_ >= s.count) return;
        if (!tc.dragged)
        {
            const float dx = (tc.x - tc.x0) * (kGW - 2), dy = (tc.y - tc.y0) * (kGH - 2);
            if (std::fabs(dx) < kDragPx && std::fabs(dy) < kDragPx) return;
            tc.dragged = true;
            gDelete_ = false;
        }
        const float pos = posOfPx(rollX(tc));
        Hit& h = s.hits[sel_];
        if (gMode_ == 1)   // drawing: the press point stays one end
        {
            const float cur = std::max(0.0f, std::min((float)s.steps, snapOrFree(pos)));
            const float ml = minLen();
            float st, len;
            if (cur >= gAnchor_) { st = gAnchor_; len = std::max(ml, cur - gAnchor_); }
            else
            {
                st = std::max(cur, s.minStart(gAnchor_, sel_));   // leftwards: stop at the hit before
                len = std::max(std::min(ml, s.maxLen(st, sel_)), gAnchor_ - st);
            }
            len = std::min(len, s.maxLen(st, sel_));   // rightwards: stop at the next hit
            const float oldSt = h.st;
            if (std::fabs(st - oldSt) > kEps) sel_ = moveHit(sel_, st);
            Hit& m = s.hits[sel_];
            if (std::fabs(len - m.len) > kEps || std::fabs(st - oldSt) > kEps) { m.len = len; markRow(selRow_); echoHit(); }
        }
        else if (gMode_ == 2)   // move in time and to other rows
        {
            const float d = (rollX(tc) - rollX(tc, true)) / kGW * zoom();
            const int toRow = std::max(0, std::min(kRows - 1, gOrigRow_ + rowAtY(rollYUp(tc)) - rowAtY(rollYUp(tc, true))));
            Row& to = rowOf(toRow);
            float st = freeHeld() ? std::round((gOrig_.st + d) * 96.0f) / 96.0f : snapOrFree(gOrig_.st + d);
            st = std::max(0.0f, std::min((float)to.steps - h.len, st));   // its end stops at the row end
            if (toRow != selRow_)
            {
                const float len = std::min(h.len, to.steps - st);
                if (st < 0 || len < kMinLen || !to.fits(st, len, -1)) return;   // stays where it last fitted
                if (moveToRow(toRow, st)) { row().hits[sel_].nd = 0; echoRow(); echoHit(); preview(); }   // a touch move: no Nudge
                return;
            }
            if (!s.fits(st, std::min(h.len, s.steps - st), sel_)) return;
            if (std::fabs(st - h.st) > kEps) { sel_ = moveHit(sel_, st); row().hits[sel_].nd = 0; markRow(selRow_); echoHit(); }
        }
        else if (gMode_ == 3)   // length from the right end
        {
            const float end = std::max(h.st + std::min(minLen(), s.maxLen(h.st, sel_)),
                                       std::min(h.st + s.maxLen(h.st, sel_), snapOrFree(pos)));
            const float len = end - h.st;
            if (std::fabs(len - h.len) > kEps) { h.len = len; markRow(selRow_); echoHit(); }
        }
    }

    void onRelease(int t, Touch& tc)
    {
        if (t == 4) { setDisp(kFreeLit, 1.0f); return; }   // parked off the right edge of the screen
        if (t != 0) return;
        if (!tc.dragged && gMode_ == 1 && sel_ >= 0 && sel_ < row().count)
        {
            row().hits[sel_].len = gEntryLen_;   // a tap on an empty place: a hit of Entry Len
            markRow(selRow_);
            echoHit();
        }
        if (!tc.dragged && gMode_ >= 2)
        {
            if (gDelete_) { deleteSelected(); lastTapHit_ = -1; }
            else { lastTapHit_ = sel_; lastTapRow_ = selRow_; lastTapMs_ = msNow(); }
        }
        else lastTapHit_ = -1;
        gMode_ = 0;
        gDelete_ = false;
    }

    // 1.3: a finger on a CC lane sets the bar under it to the finger height; a sweep fills the bars between
    void drawLane(const Touch& tc)
    {
        const int ls = choiceAt(kLaneSel);
        if (ls <= 0) return;
        CcLane& L = row().lane[ls - 1];
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
        Row& s = row();
        if (gVelHit_ < 0 || gVelHit_ >= s.count) return;
        const float v = std::max(0.01f, std::min(1.0f, padPx(tc.y, kVelH) / kVelH));
        if (std::fabs(v - s.hits[gVelHit_].v) > 1e-4f) { s.hits[gVelHit_].v = v; markRow(selRow_); echo(kVel, v * 100); }
    }

    // ===== audio thread: keys (step entry, preview) ================================================
    // Pads 1-16 (Pad 1 Note .. +15) are rows 1-16.
    void onKey(int key, int vel)
    {
        trustRestored_ = false;
        echo(kLastKey, (float)key);
        const int r = key - padNote_;
        if (r < 0 || r >= kRows) return;
        const uint8_t ch = (uint8_t)(std::max(1, std::min(16, getInt(kOutCh))) - 1);
        const int note = rowOf(r).note;
        if (keyNote_[key] >= 0) offKey(key);
        sendNow((uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)std::max(1, std::min(127, vel)));
        keyNote_[key] = note;
        if (!getBool(kStepEntry)) return;
        if (r != selRow_) { selRow_ = r; echoRow(); }
        Row& s = row();
        const float elen = kEntryLens[std::max(0, std::min(8, choiceAt(kEntryLen)))];
        if (cursor_ < s.steps - kEps)
        {
            int at = -1;
            for (int i = 0; i < s.count; ++i) if (std::fabs(s.hits[i].st - cursor_) < kEps) at = i;
            if (at >= 0) sel_ = at;
            else
            {
                const float room = s.maxLen(cursor_, -1);
                if (room >= kMinLen && s.minStart(cursor_ + kEps, -1) <= cursor_ + kEps)
                {
                    Hit nh;
                    nh.st = cursor_; nh.len = std::max(kMinLen, std::min(elen, room)); nh.v = vel / 127.0f;
                    const int idx = s.insert(nh);
                    if (idx >= 0) sel_ = idx;
                }
            }
            markRow(selRow_);
        }
        cursor_ += elen;
        if (cursor_ >= maxSteps() - kEps) cursor_ = 0;
        echo(kCursor, cursor_);
        echoHit();
        keepVisible();
    }

    void offKey(int key)
    {
        if (key < 0 || key > 127 || keyNote_[key] < 0) return;
        const uint8_t ch = (uint8_t)(std::max(1, std::min(16, getInt(kOutCh))) - 1);
        sendNow((uint8_t)(0x80 | ch), (uint8_t)keyNote_[key], 0);
        keyNote_[key] = -1;
    }

    void allNotesOff()
    {
        flushOutput();
        for (int& k : keyNote_) k = -1;
        for (int r = 0; r < kRows; ++r) { revState_[r] = 0; ccState_[r] = -1; }
    }

    // ===== display geometry ========================================================================
    static float rowCentre(int r) { return (r + 0.5f) * kRowH; }   // from the bottom
    static float padPos(float centre, float size, float pad)
    {
        return std::max(0.0f, std::min(1.0f, (centre - size * 0.5f) / (pad - size)));
    }
    static float xPark(float left, float, bool show)
    {
        return show ? std::max(0.0f, std::min(1.0f, (left + kOFF) / (kOFF + kGW))) : 0.0f;
    }

    // marks of hit h drawn from a to b (px): ratchet gaps and the chance ring (x-park values).
    void hitMarks(const Hit& h, float a, float b, bool startShown, float* out) const
    {
        for (int k = 0; k < kGaps; ++k)
        {
            const bool show = k < h.r - 1;
            const float gx = pxOfPos(h.st + h.len * (k + 1) / std::max(1, (int)h.r)) - kGapW * 0.5f;
            out[k] = xPark(gx, kGapW, show && gx > a && gx < b - kGapW);
        }
        bool chance = false;
        for (int l = 0; l < kLanes; ++l) chance = chance || h.ch[l].op != 0;
        out[kGaps] = xPark(a + 6, kBadgeW, chance && startShown && b - (a + 6) >= kBadgeW);
    }

    void refreshView()
    {
        Sequence& sq = ed();
        const float q = snapQ(), z = zoom(), v0 = (float)viewStart_, stepPx = kGW / z;
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
            int every = 1;
            if (stepPx < kNumMinPx)
            {
                if (g > 0 && g * stepPx >= kNumMinPx) every = g;
                else { every = 2; while (every * stepPx < kNumMinPx && every < 64) every *= 2; }
            }
            for (int m = 0; m < kNums; ++m)
            {
                const bool show = m >= viewStart_ && m < v0 + z - kEps && m % every == 0;
                setDisp(kNumX0 + m, xPark(pxOfPos((float)m) + 3, kNumW, show));
            }
        }
        for (int r = 0; r < kRows; ++r)
        {
            const int st = sq.rows[r].steps;
            setDisp(kEndX0 + r, st < v0 + z - kEps && st >= v0 - kEps ? lineX((float)st, 3) : 1.0f);
        }
        setDisp(kCursorX, getBool(kStepEntry) && cursor_ >= v0 - kEps && cursor_ < v0 + z - kEps
                              ? padPos(pxOfPos(cursor_) + 6.5f, 13, wide) : 1.0f);

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
        bool more = false;
        for (int r = 0; r < kRows; ++r)
        {
            const Row& w = sq.rows[r];
            for (int i = 0; i < w.count; ++i)
            {
                const Hit& h = w.hits[i];
                const float a = std::max(0.0f, pxOfPos(h.st)), b = std::min(kGW, pxOfPos(h.st + h.len));
                if (b <= 0 || a >= kGW) continue;
                if (j >= kShown) { more = true; continue; }
                const bool startShown = h.st >= v0 - kEps;
                setDisp(kHitY0 + j, padPos(rowCentre(r), kNoteH, kGH));
                bodyPieces(kPieceX0 + j * kPieces, a, b, true);
                setDisp(kNotchX0 + j, xPark(a, kNotchW, startShown));
                float marks[kGaps + 1];
                hitMarks(h, a, b, startShown, marks);
                for (int k = 0; k < kGaps; ++k) setDisp(kGapX0 + j * kGaps + k, marks[k]);
                setDisp(kChanceX0 + j, marks[kGaps]);
                setDisp(kVelX0 + j, xPark(a, kVelW, startShown && r == selRow_));
                setDisp(kVelY0 + j, h.v);
                if (r == selRow_ && i == sel_) selJ = j;
                ++j;
            }
        }
        if (tMore_.exchange(more) != more) textNotify(kMoreText);
        for (; j < kShown; ++j)
        {
            bodyPieces(kPieceX0 + j * kPieces, 0, 0, false);
            setDisp(kNotchX0 + j, 0.0f);
            for (int k = 0; k < kGaps; ++k) setDisp(kGapX0 + j * kGaps + k, 0.0f);
            setDisp(kChanceX0 + j, 0.0f); setDisp(kVelX0 + j, 0.0f);
        }
        // the selected hit again, in its own colour, on top
        Hit* sh = selHit();
        if (selJ >= 0 && sh)
        {
            const float a = std::max(0.0f, pxOfPos(sh->st)), b = std::min(kGW, pxOfPos(sh->st + sh->len));
            const bool startShown = sh->st >= v0 - kEps;
            setDisp(kSelY, computed_[kHitY0 + selJ]);
            bodyPieces(kSelPieceX0, a, b, true);
            setDisp(kSelNotchX, xPark(a, kNotchW, startShown));
            setDisp(kSelVelX, computed_[kVelX0 + selJ]);
            setDisp(kSelVelY, sh->v);
            float marks[kGaps + 1];
            hitMarks(*sh, a, b, startShown, marks);
            for (int k = 0; k < kGaps; ++k) setDisp(kSelGapX0 + k, marks[k]);
            setDisp(kSelChanceX, marks[kGaps]);
        }
        else
        {
            bodyPieces(kSelPieceX0, 0, 0, false);
            setDisp(kSelNotchX, 0.0f);
            setDisp(kSelVelX, 0.0f);
            for (int k = 0; k < kGaps; ++k) setDisp(kSelGapX0 + k, 0.0f);
            setDisp(kSelChanceX, 0.0f);
        }
        // 1.3: LANE: the bars of the shown CC lane on this page (0 = no bar)
        {
            const int ls = choiceAt(kLaneSel);
            const CcLane* L = ls > 0 ? &row().lane[ls - 1] : nullptr;
            for (int j = 0; j < kMaxSteps; ++j)
            {
                const int step = viewStart_ + j;
                setDisp(kLnBarY0 + j, (L && j < (int)z && step < L->steps) ? L->val[step] / 127.0f : 0.0f);
            }
        }
        setDisp(kLineY, 0.5f);
        updateTexts();
    }

    // ===== MIDI thread and worker ==================================================================
    void midiLoop()
    {
#if defined(__linux__)
        sched_param sp{};
        sp.sched_priority = 40;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   // ignored if not permitted
#endif
        alsa_.open(clientName_, "MIDI Out");
        while (!quit_.load())
        {
            wakeMidi_.waitMs(50);
            uint8_t m[3];
            int64_t t;
            int n = 0;
            while (ring_.pop(m, t)) { deliver(m); ++n; }
            if (n) alsa_.flush();
        }
        uint8_t m[3];
        int64_t t;
        while (ring_.pop(m, t)) deliver(m);
        for (int c = 0; c < 16; ++c) { const uint8_t off[3] = {(uint8_t)(0xB0 | c), 123, 0}; deliver(off); }
        alsa_.flush();
        alsa_.close();
    }

    static constexpr int kDisplayMs = 50;   // as scalaSeq 2.4.1
    void workerLoop()
    {
        auto lastNotify = std::chrono::steady_clock::now();
        while (!quit_.load())
        {
            wake_.waitMs(kDisplayMs);
            const auto now = std::chrono::steady_clock::now();
            if (now - lastNotify < std::chrono::milliseconds(kDisplayMs)) continue;
            lastNotify = now;
            notifyPass();
        }
    }
    // At most kMaxNotify reports to the host for each pass, round-robin from where the last pass
    // stopped; the rest go out in the next passes. Every report is an audioMasterAutomate, which the
    // MPC answers with a setParameter (Open MPC NOTES 2026-10-08); about 1,000 of them per second
    // made audio blocks late on the MPC One G2 (scalaSeq trace, 2026-11-21). Open MPC plugins also
    // bound their reports per pass (docs/SDK-PORTING.md).
    static constexpr int kMaxNotify = 30;
    int notifyPass()
    {
        int sent = 0, i = notifyCursor_;
        for (int k = 0; k < kNumParams && sent < kMaxNotify; ++k, i = (i + 1) % kNumParams)
            if (notify_[i].exchange(false, std::memory_order_acquire)) { notifyHostParameterChanged(i); ++sent; }
        notifyCursor_ = i;
        return sent;
    }
    int notifyCursor_ = 0;

    void deliver(const uint8_t* m)
    {
        alsa_.send(m);
#ifdef PERCSEQ_TEST
        std::lock_guard<std::mutex> l(testMutex_);
        testLog_.push_back({m[0], m[1], m[2]});
#endif
    }

    // ----- state ----------------------------------------------------------------------------------
    std::unique_ptr<Model> model_, saved_, staging_;
    std::unique_ptr<Tables[]> tables_;   // [2][kRows]: the playing set and the next sequence
    std::atomic<bool> stagingReady_{false};
    std::mutex savedMutex_;
    bool savedDirty_ = false, savedAll_ = true, viewDirty_ = true;
    uint32_t dirtyRows_ = 0;

    Pool pool_;
    SoundCtx sctx_;
    Engine engines_[kRows];
    double anchor_[kRows] = {}, invLoop_[kRows] = {}, ahead_[kRows] = {};
    bool empty_[kRows] = {};
    bool rowLive_[kRows] = {}, rowPend_[kRows] = {}, rowStarted_[kRows] = {};   // 1.1: row on/off while playing
    int tIdx_ = 0, edSeq_ = 0, engSeq_ = 0, selRow_ = 0, sel_ = -1, viewStart_ = 0;
    bool pendSwitch_ = false, wasPlaying_ = false;
    static constexpr int kMaxSeqEv = 16;
    struct SeqEv { int at = 0, seq = 0; };
    SeqEv seqEv_[kMaxSeqEv];
    int seqEvN_ = 0, padNote_ = 37;
    float cursor_ = 0;
    double expectPpq_ = 0, barQ_ = 4.0, nowP_ = 0, lastP_ = 0;

    // output: timed queue, notes per row, CC states per sound row, note counts
    static constexpr int kMaxEv = 1024;
    Ev evq_[kMaxEv];
    int evN_ = 0;
    int rowNote_[kRows] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int curSound_[kRows] = {};
    float curVel_[kRows] = {};
    int revState_[kRows] = {};
    // 1.3: CC lanes and locks
    static constexpr long kNoStep = -2147483647L;
    double lnAnchor_[kRows] = {};
    long lnN_[kRows][kCcLanes];
    int lnIdx_[kRows][kCcLanes];
    int ccSent_[128];
    uint32_t rng_ = 0x9e3779b9u;
    int gLnLast_ = -1;
    int ccState_[kRows] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int noteCnt_[128] = {};
    // playheads: step changes wait for the song to reach them (the engines run ahead)
    static constexpr int kFifo = 16;
    struct StepFifo { double t[kFifo] = {}; int step[kFifo] = {}; int head = 0, n = 0; };
    StepFifo stepFifo_[kRows];
    int lastStep_[kRows] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int dispStep_[kRows] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    float lastSeen_[kNumParams];
    float computed_[kNumParams];
    bool primeOnly_ = false;
    bool trustRestored_ = false;
    int stagingView_[4] = {0, 0, 0, 0};
    std::atomic<int> tSel_{0}, tCount_{0}, tView0_{0}, tZoom_{16}, tPages_{1};
    std::atomic<bool> tMore_{false};
    std::atomic<int> tChOp_[kLanes] = {}, tChProb_[kLanes] = {}, tChAmt_[kLanes] = {};
    std::atomic<int> tRowNote_[kRows] = {};
    bool clearArmed_ = false;
    bool seqArmed_[2] = {false, false}, pastePending_ = false;   // 1.3: PASTE, CLEAR armed
    double seqArmMs_[2] = {0, 0};
    int burstSend_ = -1, burstCC_ = 0, burstStep_ = 0;   // SEND zigzag: which SEND is on (-1 = none)
    double burstMs_ = 0;
    double clearArmMs_ = 0;
    std::atomic<bool> notify_[kNumParams];
    bool notifyPending_ = false;

    int keyNote_[128];
    uint64_t sampleCount_ = 0;
    int previewNote_ = -1;
    double previewOffMs_ = 0;
    bool previewSent_ = false;

    // touch gestures: 0 roll, 1 ruler, 2 labels, 3 velocity lane, 4 FREE
    Touch touch_[5];
    int gMode_ = 0;           // roll: 0 none, 1 draw, 2 move, 3 length
    float gAnchor_ = 0, gEntryLen_ = 1;
    Hit gOrig_;
    int gOrigRow_ = 0;
    bool gDelete_ = false;
    int lastTapHit_ = -1, lastTapRow_ = -1;
    double lastTapMs_ = -1e9;
    int gView0_ = 0, gZoom0_ = 16, gAxis_ = 0, gVelHit_ = -1;
    bool gFlipped_ = false;

    std::atomic<int> laneOp_[kLanes] = {};

    rt::TimedRing ring_;
    rt::Wakeup wake_;
    alsa::Output alsa_;
    char clientName_[48] = {};
    std::atomic<bool> quit_{false};
    std::thread worker_, midiThread_;
    rt::Wakeup wakeMidi_;

#ifdef PERCSEQ_TEST
public:
    std::mutex testMutex_;
    std::vector<std::array<uint8_t, 3>> testLog_;
    struct TestEmit { double t, at; int type, a, b; };
    std::vector<TestEmit> testEmits_;   // emitted queue events (audio thread, test build only)
    struct TestStep { double t; int row, step; };
    std::vector<TestStep> testSteps_;
    Model& testModel() { return *model_; }
    int testSel() const { return sel_; }
    int testSelRow() const { return selRow_; }
    float testCursor() const { return cursor_; }
    int testViewStart() const { return viewStart_; }
    int testEdSeq() const { return edSeq_; }
    int testEngSeq() const { return engSeq_; }
    int testDispStep(int r) const { return dispStep_[r]; }
    float testComputed(int i) const { return computed_[i]; }
    void testSaveAll() { savedAll_ = true; }
    int testChoice(int i) const { return choiceAt(i); }
#endif
};

#ifdef PERCSEQ_TRACE
PSQ_TRACE_ENTRY(PercSeq)
#else
MPC_PLUGIN_ENTRY(PercSeq)
#endif
