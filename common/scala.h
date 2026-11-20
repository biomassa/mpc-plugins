// scala.h - Scala (.scl) and keyboard mapping (.kbm) parsing, and the 128-key retuning table.
//
// SPDX-License-Identifier: MIT
//
// Everything here allocates and uses log/pow: call it from the worker thread, the constructor
// or loadExtraState(), NEVER from process(). process() only reads the finished RetuneTable.
//
// Format reference: https://www.huygens-fokker.org/scala/scl_format.html and help on .kbm files.

#ifndef SCALA_RETUNER_SCALA_H
#define SCALA_RETUNER_SCALA_H

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace scala {

// A parsed .scl file. cents[0] = 0 (the unison, implicit in the file); cents[N] = the period.
struct Scale
{
    std::string description;
    std::vector<double> cents;   // size N + 1
    int size() const { return (int)cents.size() - 1; }
    bool valid() const { return cents.size() >= 2 && cents.back() > 0.0; }
};

// A parsed .kbm file. mapSize 0 = linear mapping (every scale degree in order).
struct Mapping
{
    int mapSize = 0;
    int firstNote = 0, lastNote = 127;
    int middleNote = 60;          // key where map entry 0 (scale degree 0) sits
    int refNote = 69;             // key whose frequency is given
    double refFreq = 440.0;
    int octaveDegree = 0;         // degree that counts as the formal octave (0 = scale size)
    std::vector<int> map;         // -1 = unmapped ("x")
};

namespace detail {

inline std::vector<std::string> contentLines(const std::string& text)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= text.size())
    {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string line = text.substr(i, j - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != '!') out.push_back(line);
        i = j + 1;
    }
    return out;
}

inline std::string firstToken(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_first_of(" \t", a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// One pitch line: "702.0" (cents, has a dot), "3/2" (ratio) or "2" (integer ratio).
inline bool parsePitch(const std::string& line, double& cents)
{
    std::string t = firstToken(line);
    if (t.empty()) return false;
    char* end = nullptr;
    if (t.find('.') != std::string::npos)
    {
        cents = std::strtod(t.c_str(), &end);
        return end != t.c_str();
    }
    size_t slash = t.find('/');
    double num = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    double den = 1.0;
    if (slash != std::string::npos) den = std::strtod(t.c_str() + slash + 1, nullptr);
    if (num <= 0.0 || den <= 0.0) return false;
    cents = 1200.0 * std::log2(num / den);
    return true;
}

inline bool parseInt(const std::string& line, int& v)
{
    std::string t = firstToken(line);
    if (t.empty()) return false;
    char* end = nullptr;
    long x = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str()) return false;
    v = (int)x;
    return true;
}

inline int floorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }

} // namespace detail

inline bool parseScl(const std::string& text, Scale& out, std::string* error = nullptr)
{
    std::vector<std::string> lines = detail::contentLines(text);
    if (lines.size() < 2) { if (error) *error = "too short"; return false; }
    Scale s;
    s.description = lines[0];
    int n = 0;
    if (!detail::parseInt(lines[1], n) || n < 1 || n > 1024) { if (error) *error = "bad note count"; return false; }
    s.cents.push_back(0.0);
    for (size_t i = 2; i < lines.size() && (int)s.cents.size() <= n; ++i)
    {
        double c;
        if (detail::firstToken(lines[i]).empty()) continue;
        if (!detail::parsePitch(lines[i], c)) { if (error) *error = "bad pitch line: " + lines[i]; return false; }
        s.cents.push_back(c);
    }
    if ((int)s.cents.size() != n + 1) { if (error) *error = "fewer pitches than the count"; return false; }
    if (!s.valid()) { if (error) *error = "period must be above the unison"; return false; }
    out = s;
    return true;
}

inline bool parseKbm(const std::string& text, Mapping& out, std::string* error = nullptr)
{
    std::vector<std::string> lines = detail::contentLines(text);
    std::vector<std::string> ne;   // non-empty lines
    for (const std::string& l : lines) if (!detail::firstToken(l).empty()) ne.push_back(l);
    if (ne.size() < 7) { if (error) *error = "header needs 7 lines"; return false; }
    Mapping m;
    char* end = nullptr;
    if (!detail::parseInt(ne[0], m.mapSize) || !detail::parseInt(ne[1], m.firstNote) ||
        !detail::parseInt(ne[2], m.lastNote) || !detail::parseInt(ne[3], m.middleNote) ||
        !detail::parseInt(ne[4], m.refNote) || !detail::parseInt(ne[6], m.octaveDegree))
    { if (error) *error = "bad header"; return false; }
    std::string f = detail::firstToken(ne[5]);
    m.refFreq = std::strtod(f.c_str(), &end);
    if (end == f.c_str() || m.refFreq <= 0.0) { if (error) *error = "bad reference frequency"; return false; }
    if (m.mapSize < 0 || m.mapSize > 1024) { if (error) *error = "bad map size"; return false; }
    for (int i = 0; i < m.mapSize; ++i)
    {
        int deg = -1;
        if (7 + (size_t)i < ne.size())
        {
            std::string t = detail::firstToken(ne[7 + (size_t)i]);
            if (t != "x" && t != "X" && !detail::parseInt(t, deg)) deg = -1;
        }
        m.map.push_back(deg);
    }
    out = m;
    return true;
}

// The linear mapping used when no .kbm is chosen: degree 0 on `root`, which sounds at its
// 12-TET pitch (A4 = 440 Hz) shifted by `detuneCents`.
inline Mapping linearMapping(int root, double detuneCents)
{
    Mapping m;
    m.mapSize = 0;
    m.middleNote = root;
    m.refNote = root;
    m.refFreq = 440.0 * std::pow(2.0, (root - 69) / 12.0 + detuneCents / 1200.0);
    return m;
}

// Cents of a key relative to the mapping's middle note; false if the key is unmapped.
inline bool keyCents(const Scale& s, const Mapping& m, int key, double& cents, bool ignoreUnmapped = false,
                     int* degreeOut = nullptr, int* octaveOut = nullptr)
{
    const int n = s.size();
    auto degreeCents = [&](int d) {
        int q = detail::floorDiv(d, n);
        int r = d - q * n;
        return q * s.cents[(size_t)n] + s.cents[(size_t)r];
    };
    const int offset = key - m.middleNote;
    if (m.mapSize == 0)
    {
        cents = degreeCents(offset);
        const int q0 = detail::floorDiv(offset, n);
        if (degreeOut) *degreeOut = offset - q0 * n;
        if (octaveOut) *octaveOut = q0;
        return true;
    }
    if (!ignoreUnmapped && (key < m.firstNote || key > m.lastNote)) return false;
    int q = detail::floorDiv(offset, m.mapSize);
    int r = offset - q * m.mapSize;
    int deg = m.map[(size_t)r];
    if (deg < 0)
    {
        if (!ignoreUnmapped) return false;
        deg = r;
    }
    int octDeg = m.octaveDegree > 0 ? m.octaveDegree : n;
    cents = q * degreeCents(octDeg) + degreeCents(deg);
    if (degreeOut) *degreeOut = deg - detail::floorDiv(deg, n) * n;
    if (octaveOut) *octaveOut = q + detail::floorDiv(deg, n);
    return true;
}

// MIDI pitch (69 = A4 = 440 Hz, fractional) of any key, even outside 0..127; NaN if unmapped.
inline double keyPitch(const Scale& s, const Mapping& m, int key)
{
    double refCents = 0.0, c = 0.0;
    if (!s.valid() || !keyCents(s, m, key, c)) return NAN;
    keyCents(s, m, m.refNote, refCents, true);
    double f = m.refFreq * std::pow(2.0, (c - refCents) / 1200.0);
    return 69.0 + 12.0 * std::log2(f / 440.0);
}

enum class BendCurve { Symmetric = 0, Opx4 = 1 };

// What process() reads. Everything is precomputed on the worker thread; process() does a few
// multiply-adds per note or per bend-wheel move (bendFor below), no transcendental functions.
struct RetuneTable
{
    int8_t note[128];     // 12-TET note to play; -1 = do not play (unmapped or outside 0..127)
    float  resid[128];    // retuning offset from that note, semitones
    float  up[128];       // wheel fully up: extra semitones (pitch of the key In Bend steps above)
    float  down[128];     // wheel fully down: extra semitones (<= 0)
    float  perSemiUp;     // bend units per semitone, upward (curve and Bend Up range)
    float  perSemiDown;   // bend units per semitone, downward (Bend Down range)

    // Display data (ROLL page). Read by the worker thread only.
    static constexpr int kMaxGrid = 31;
    float   pc[128];          // position of the key in the period, 0..1 (pitch class)
    int16_t degree[128];      // scale degree 0..size-1
    int8_t  octave[128];      // period number relative to degree 0 of the mapping
    float   periodSemis;      // period in semitones
    int     size;             // number of scale degrees
    int     gridCount;        // lines in grid[] (min(size, kMaxGrid))
    float   grid[kMaxGrid];   // position of each scale degree in the period, 0..1
};

// 14-bit bend for an offset in cents, for a target with the given ranges and curve.
inline int bendValue(double cents, int rangeUp, int rangeDown, BendCurve curve)
{
    double v;
    if (cents >= 0.0) v = cents / (rangeUp * 100.0) * (curve == BendCurve::Opx4 ? 8064.0 : 8192.0);
    else              v = cents / (rangeDown * 100.0) * 8192.0;
    long r = std::lround(v);
    return (int)(r > 8191 ? 8191 : (r < -8192 ? -8192 : r));
}

// 14-bit bend for `key` with the wheel at w (-1..1). Symmetric curve: value/8192 of the range
// both ways (keygroups). OPx-4: up uses value/8064 (measured on OPx-4, MPC 3.10.1), down /8192.
inline int bendFor(const RetuneTable& t, int key, float w)
{
    const float semis = t.resid[key] + (w >= 0.0f ? w * t.up[key] : -w * t.down[key]);
    const float v = semis >= 0.0f ? semis * t.perSemiUp : semis * t.perSemiDown;
    int r = (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
    return r > 8191 ? 8191 : (r < -8192 ? -8192 : r);
}

// steps: In Bend range in scale steps (keys of the mapping). 0 = the wheel does nothing.
inline void buildTable(const Scale& s, const Mapping& m, int rangeUp, int rangeDown, BendCurve curve,
                       int steps, RetuneTable& t)
{
    t.perSemiUp = (curve == BendCurve::Opx4 ? 8064.0f : 8192.0f) / (float)rangeUp;
    t.perSemiDown = 8192.0f / (float)rangeDown;
    // fallback step size where a neighbour key is unmapped: the average step of the mapping
    double avgStep = 1.0;
    if (s.valid())
    {
        const int n = s.size();
        if (m.mapSize == 0) avgStep = s.cents[(size_t)n] / n / 100.0;
        else
        {
            const int oct = m.octaveDegree > 0 ? m.octaveDegree : n;
            const double octCents = detail::floorDiv(oct, n) * s.cents[(size_t)n] + s.cents[(size_t)(oct - detail::floorDiv(oct, n) * n)];
            avgStep = octCents / m.mapSize / 100.0;
        }
    }
    const double period = s.valid() ? s.cents[(size_t)s.size()] : 1200.0;
    t.periodSemis = (float)(period / 100.0);
    t.size = s.valid() ? s.size() : 12;
    t.gridCount = t.size < RetuneTable::kMaxGrid ? t.size : RetuneTable::kMaxGrid;
    for (int g = 0; g < RetuneTable::kMaxGrid; ++g)
        t.grid[g] = (g < t.gridCount && s.valid()) ? (float)(s.cents[(size_t)g] / period) : 0.0f;
    for (int k = 0; k < 128; ++k)
    {
        t.note[k] = -1; t.resid[k] = 0; t.up[k] = 0; t.down[k] = 0;
        t.pc[k] = 0; t.degree[k] = 0; t.octave[k] = 0;
        const double p = keyPitch(s, m, k);
        if (std::isnan(p)) continue;
        {
            double c = 0; int d = 0, o = 0;
            keyCents(s, m, k, c, false, &d, &o);
            double f = c / period - std::floor(c / period);
            t.pc[k] = (float)f; t.degree[k] = (int16_t)d; t.octave[k] = (int8_t)o;
        }
        const long n = std::lround(p);
        if (n < 0 || n > 127) continue;
        t.note[k] = (int8_t)n;
        t.resid[k] = (float)(p - (double)n);
        if (steps > 0)
        {
            const double pu = keyPitch(s, m, k + steps), pd = keyPitch(s, m, k - steps);
            t.up[k] = (float)(std::isnan(pu) ? steps * avgStep : pu - p);
            t.down[k] = (float)(std::isnan(pd) ? -steps * avgStep : pd - p);
        }
    }
}

} // namespace scala

#endif
