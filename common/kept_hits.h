// Kept hits (percSeq) and kept notes (scalaSeq), 2026-11-22 (user): Steps no longer deletes them.
//
// Layout: items[0 .. count) are the active items, exactly as before: sorted by start, inside Steps,
// lengths clipped at Steps. Every reader (engine, drawing, selection, edits) uses only these.
// items[count .. count + kept) are kept entries:
//   - an entry that is NOT visible under the current Steps is a hit outside Steps, at full length;
//   - an entry that IS visible is the full-length copy of an active hit that crosses the end
//     (that active hit is its clipped twin).
// "Visible" is the old drop rule of fitToSteps: steps - st >= kMinLen.
//
// The logical list is all items at full length: active items (a twin gets its copy's length back),
// plus the kept entries outside Steps. A copy whose twin was edited or deleted is dropped: any edit of
// a cut item counts, and its old length is forgotten. Matching is field by field (no memcmp: padding).
// A Steps change = logical list (old Steps) -> split (new Steps). Project state saves the logical list
// (formats PSQ3 / SSQ5 unchanged) and the load splits it again.
#pragma once

#include <algorithm>
#include <cmath>

namespace keephits {

template <class H> inline bool visible(const H& h, int steps, float minLen) { return steps - h.st >= minLen; }

// sameButLen(a, b): every field except len equal
template <class H, class Same>
inline int logical(const H* items, int count, int kept, int steps, float minLen, float eps, Same sameButLen, H* out)
{
    int n = 0;
    for (int i = 0; i < count; ++i) out[n++] = items[i];
    // copies: give the matching twin its full length back
    for (int k = count; k < count + kept; ++k)
    {
        const H& c = items[k];
        if (!visible(c, steps, minLen)) continue;
        for (int i = 0; i < count; ++i)
        {
            H& a = out[i];
            if (std::fabs(a.st - c.st) < eps && sameButLen(a, c)
                && std::fabs(a.len - std::min(c.len, (float)steps - c.st)) < eps)
            {
                a.len = c.len;
                break;
            }
        }
    }
    // hits outside Steps, merged in start order (both parts are sorted)
    for (int k = count; k < count + kept; ++k)
    {
        const H& c = items[k];
        if (visible(c, steps, minLen)) continue;
        int i = n;
        while (i > 0 && out[i - 1].st > c.st) { out[i] = out[i - 1]; --i; }
        out[i] = c;
        ++n;
    }
    return n;
}

// list (sorted, full lengths, n items) -> items/count/kept for Steps `steps`. max = capacity of items.
// A copy that does not fit is left out (that hit then stays cut).
template <class H>
inline void split(const H* list, int n, int steps, float minLen, int max, H* items, int& count, int& kept)
{
    int c = 0;
    for (int i = 0; i < n; ++i)
        if (visible(list[i], steps, minLen))
        {
            items[c] = list[i];
            items[c].len = std::min(list[i].len, (float)steps - list[i].st);
            ++c;
        }
    int k = c;
    for (int i = 0; i < n && k < max; ++i)
        if (!visible(list[i], steps, minLen)) items[k++] = list[i];
    for (int i = 0; i < n && k < max; ++i)
        if (visible(list[i], steps, minLen) && list[i].len > (float)steps - list[i].st) items[k++] = list[i];
    count = c;
    kept = k - c;
}

// kept entries outside Steps (the "+N kept" text)
template <class H>
inline int outside(const H* items, int count, int kept, int steps, float minLen)
{
    int n = 0;
    for (int k = count; k < count + kept; ++k) n += visible(items[k], steps, minLen) ? 0 : 1;
    return n;
}

} // namespace keephits
