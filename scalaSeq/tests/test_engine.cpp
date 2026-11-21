// Engine tests for scalaSeq (no MPC, no ALSA). Run: sh tests/run.sh
#include "../src/seq_engine.h"

#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace seq;

struct Hit { double t; int step; float pitch, vel; };

// run `loops` loops of the sequence; returns gate rises
static std::vector<Hit> run(Engine& e, const Tables& T, int loops, double sr = 1000.0, double loopSec = 1.0)
{
    std::vector<Hit> h;
    float prev = 0;
    const long N = (long)(loops * loopSec * sr);
    for (long i = 0; i < N; ++i)
    {
        const double ph = std::fmod(i / (loopSec * sr), 1.0);
        EngineOut o = e.tick(ph, true, T, sr);
        if (o.gate > 0 && prev <= 0) h.push_back({i / sr, o.step, o.pitch, o.vel});
        prev = o.gate;
    }
    return h;
}

static scala::Scale g_scale12;
static Tuning tuning12()
{
    scala::parseScl("! x\n12\n12\n100.\n200.\n300.\n400.\n500.\n600.\n700.\n800.\n900.\n1000.\n1100.\n2/1\n", g_scale12);
    Tuning t;
    t.sc = &g_scale12;
    t.rootPitch = 60;
    return t;
}

static Note note(float st, float len, int deg)
{
    Note n; n.st = st; n.len = len; n.pitch = (int16_t)(kCenter + deg); return n;
}

int main()
{
    static Sequence s;
    static Tables T;
    Tuning t = tuning12();

    // 1. four notes, 8 steps, forward: hits at their starts with their pitches
    s = Sequence(); s.steps = 8; s.rStart = 0; s.rEnd = 7;
    s.insert(note(0, 1, 0)); s.insert(note(2, 1, 4)); s.insert(note(4.5f, 1, 7)); s.insert(note(6, 2, 12));
    buildTables(s, t, T);
    Engine e;
    auto h = run(e, T, 2, 8000.0, 1.0);   // one step = 1/8 s
    CHECK(h.size() == 8, "8 hits in 2 loops: %zu", h.size());
    if (h.size() >= 4)
    {
        CHECK(std::fabs(h[1].t - 2.0 / 8) < 0.002 && std::fabs(h[1].pitch - 64) < 1e-3, "note 2 at step 2, E4: %f %f", h[1].t, h[1].pitch);
        CHECK(std::fabs(h[2].t - 4.5 / 8) < 0.002 && std::fabs(h[2].pitch - 67) < 1e-3, "note 3 at 4.5 steps: %f", h[2].t);
        CHECK(std::fabs(h[3].pitch - 72) < 1e-3, "octave up: %f", h[3].pitch);
    }

    // 2. ratchets: 4 hits in one note; FADE > 0 falls
    s = Sequence(); s.steps = 4; s.rEnd = 3;
    { Note n = note(0, 2, 0); n.r = 4; n.fd = 50; n.d = 0.5f; s.insert(n); }
    buildTables(s, t, T);
    e = Engine();
    h = run(e, T, 1, 8000.0, 1.0);
    CHECK(h.size() == 4, "4 ratchet hits: %zu", h.size());
    if (h.size() == 4) CHECK(h[0].vel > h[3].vel && std::fabs(h[3].vel - 0.01f) < 1e-3, "fade to 1%%: %f %f", h[0].vel, h[3].vel);

    // 3. reverse direction plays the notes in the other order
    s = Sequence(); s.steps = 4; s.rEnd = 3;
    s.insert(note(0, 1, 0)); s.insert(note(1, 1, 2)); s.insert(note(2, 1, 4)); s.insert(note(3, 1, 5));
    buildTables(s, t, T);
    e = Engine(); e.dir = 1;
    h = run(e, T, 1, 8000.0, 1.0);
    CHECK(h.size() == 4 && std::fabs(h[0].pitch - 65) < 1e-3 && std::fabs(h[3].pitch - 60) < 1e-3, "reverse order");

    // 4. play range: only steps 1..2
    s.rStart = 1; s.rEnd = 2;
    buildTables(s, t, T);
    e = Engine();
    h = run(e, T, 1, 8000.0, 1.0);
    bool onlyRange = true;
    for (auto& x : h) if (x.step < 1 || x.step > 2) onlyRange = false;
    CHECK(onlyRange && h.size() == 4, "range 1..2 is a sub-loop (twice per loop): %zu hits", h.size());

    // 5. chance: SKIP 100% on the 2nd note: it never plays; MUTE 0% never mutes
    s = Sequence(); s.steps = 4; s.rEnd = 3;
    s.insert(note(0, 1, 0)); s.insert(note(1, 1, 2)); s.insert(note(2, 1, 4)); s.insert(note(3, 1, 5));
    s.notes[1].ch[0].op = 3; s.notes[1].ch[0].prob = 1.0f;
    s.notes[2].ch[0].op = 1; s.notes[2].ch[0].prob = 0.0f;
    buildTables(s, t, T);
    e = Engine();
    h = run(e, T, 3, 8000.0, 1.0);
    bool sawSkipped = false;
    for (auto& x : h) if (std::fabs(x.pitch - 62) < 1e-3) sawSkipped = true;
    CHECK(!sawSkipped && h.size() == 12, "SKIP 100%%: the next step takes the slot: %zu hits", h.size());

    // 6. P+ 2 degrees on a 12-tone scale with a mask (only even degrees): 0 -> 4 (two scale steps)
    for (int i = 0; i < 12; ++i) t.mask[i] = (i % 2 == 0);
    s = Sequence(); s.steps = 2; s.rEnd = 1;
    s.insert(note(0, 1, 0));
    s.notes[0].ch[0].op = 16; s.notes[0].ch[0].prob = 1.0f; s.notes[0].ch[0].amt = 2;
    buildTables(s, t, T);
    e = Engine();
    h = run(e, T, 1, 8000.0, 1.0);
    CHECK(!h.empty() && std::fabs(h[0].pitch - 64) < 1e-3, "P+2 in the mask: %f", h.empty() ? 0.f : h[0].pitch);
    for (int i = 0; i < 12; ++i) t.mask[i] = true;

    // 7. glide: the second note slides from the first over 0.5 steps
    s = Sequence(); s.steps = 4; s.rEnd = 3;
    s.insert(note(0, 2, 0)); { Note n = note(2, 2, 12); n.gl = 0.5f; s.insert(n); }
    buildTables(s, t, T);
    e = Engine();
    {
        float mid = 0; float prev = 0; int rises = 0;
        const double sr = 8000;
        for (long i = 0; i < 8000; ++i)
        {
            EngineOut o = e.tick(i / sr, true, T, sr);
            if (o.gate > 0 && prev <= 0) rises++;
            if (i == (long)(sr * (2.25 / 4))) mid = o.pitch;   // a quarter step into the glide
            prev = o.gate;
        }
        CHECK(rises == 2 && mid > 62 && mid < 70, "glide midway: %f", mid);
    }

    // 7b. GLIDE op (2.4): 100 %, amount 50 % of the 2-step note = a 1-step glide; NO GLIDE cancels the note's glide
    {
        auto pitchAt = [&](double atStep) {
            buildTables(s, t, T);
            Engine g;
            const double sr = 8000;
            float p = 0;
            for (long i = 0; i < 8000; ++i) { EngineOut o = g.tick(i / sr, true, T, sr); if (i == (long)(sr * atStep / 4)) p = o.pitch; }
            return p;
        };
        s = Sequence(); s.steps = 4; s.rEnd = 3;
        s.insert(note(0, 2, 0));
        { Note n = note(2, 2, 12); n.ch[0].op = 28; n.ch[0].prob = 1; n.ch[0].amt = 50; s.insert(n); }
        const float half = pitchAt(2.5), done = pitchAt(3.2);
        CHECK(std::fabs(half - 66) < 0.3 && std::fabs(done - 72) < 1e-3, "GLIDE 50%% of 2 steps: mid %f (want 66), after %f", half, done);
        s = Sequence(); s.steps = 4; s.rEnd = 3;
        s.insert(note(0, 2, 0));
        { Note n = note(2, 2, 12); n.gl = 0.5f; n.ch[0].op = 29; n.ch[0].prob = 1; s.insert(n); }
        const float now = pitchAt(2.1);
        CHECK(std::fabs(now - 72) < 1e-3, "NO GLIDE: the note starts at its pitch: %f", now);
    }

    // 7c. DELAY % (2.4): 50 % of a step -> the note at step 1 starts at 1.5
    s = Sequence(); s.steps = 4; s.rEnd = 3;
    { Note n = note(1, 1, 0); n.ch[0].op = 30; n.ch[0].prob = 1; n.ch[0].amt = 50; s.insert(n); }
    buildTables(s, t, T);
    e = Engine();
    h = run(e, T, 1, 8000.0, 1.0);   // one step = 1/4 s
    CHECK(h.size() == 1 && std::fabs(h[0].t - 1.5 / 4) < 0.002, "DELAY 50%%: starts at 1.5 steps: %f", h.empty() ? -1 : h[0].t * 4);

    // 8. swing 75%: the 2nd step of a pair starts at 3/4 of the pair
    s = Sequence(); s.steps = 4; s.rEnd = 3; s.swing = 75;
    s.insert(note(0, 1, 0)); s.insert(note(1, 1, 2));
    buildTables(s, t, T);
    e = Engine();
    h = run(e, T, 1, 8000.0, 1.0);
    CHECK(h.size() == 2 && std::fabs(h[1].t - 0.75 * 2 / 4) < 0.004, "swing: %f", h.size() > 1 ? h[1].t : -1);

    std::printf(g_fail ? "%d FAILED\n" : "engine tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
