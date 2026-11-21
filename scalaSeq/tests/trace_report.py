#!/usr/bin/env python3
"""Report for a scalaSeq trace (build with --define SCALASEQ_TRACE=1, see src/trace.h).

Usage: trace_report.py ssq_trace.bin [thread_names.txt]
thread_names.txt: lines "<tid> <name>" (from /proc/<pid>/task/*/comm on the MPC).

Prints, per second: audio blocks, the largest gap between blocks (a gap well over 2.9 ms means the
audio was late), the longest process() call, touch updates, getParameter / setParameter / dispatcher
calls and the plugin's calls into the host (count and time spent in them), plus totals per thread.
"""
import struct
import sys
from collections import defaultdict

REC = struct.Struct("<QIiHhi")
KIND = {0: "process", 1: "dispatch", 2: "setParam", 3: "getParam", 4: "hostCall", 5: "mark", 99: "LOST"}
TOUCH = set(range(89, 99))   # Touch roll/ruler/labels/lane/free X and Y


def main():
    data = open(sys.argv[1], "rb").read()
    names = {}
    if len(sys.argv) > 2:
        for line in open(sys.argv[2]):
            p = line.split(None, 1)
            if len(p) == 2 and p[0].isdigit():
                names[int(p[0])] = p[1].strip()
    recs = [REC.unpack_from(data, i) for i in range(0, len(data) - REC.size + 1, REC.size)]
    if not recs:
        print("no records")
        return
    t0 = min(r[0] for r in recs if r[3] != 99)
    lost = sum(r[1] for r in recs if r[3] == 99)
    print(f"{len(recs)} records over {(max(r[0] for r in recs) - t0) / 1e9:.1f} s, lost {lost}")

    # per thread
    per = defaultdict(lambda: defaultdict(lambda: [0, 0, 0]))
    for t, dur, tid, kind, op, idx in recs:
        if kind == 99:
            continue
        key = KIND[kind] + (f" op{op}" if kind in (1, 4) else "")
        e = per[tid][key]
        e[0] += 1; e[1] += dur; e[2] = max(e[2], dur)
    print("\nper thread: calls, total ms, longest us")
    for tid, d in sorted(per.items(), key=lambda x: -sum(v[1] for v in x[1].values())):
        print(f"  {tid} {names.get(tid, '?')}")
        for k, (n, tot, mx) in sorted(d.items(), key=lambda x: -x[1][1])[:8]:
            print(f"     {k:16s} {n:8d} {tot / 1e6:9.1f} {mx / 1e3:9.1f}")

    # per second
    procs = sorted((r[0], r[1]) for r in recs if r[3] == 0)
    sec = defaultdict(lambda: dict(mark="", blocks=0, gap=0, late=0, pmax=0, touch=0, get=0, set=0, disp=0, host=0, hostms=0.0))
    for (a, d), (b, _) in zip(procs, procs[1:]):
        s = sec[int((b - t0) / 1e9)]
        g = (b - a) / 1e6
        s["gap"] = max(s["gap"], g)
        if g > 4.0:
            s["late"] += 1
    for t, dur, tid, kind, op, idx in recs:
        if kind == 99:
            continue
        s = sec[int((t - t0) / 1e9)]
        if kind == 0:
            s["blocks"] += 1; s["pmax"] = max(s["pmax"], dur / 1e3)
        elif kind == 2:
            s["set"] += 1
            if idx in TOUCH:
                s["touch"] += 1
        elif kind == 3:
            s["get"] += 1
        elif kind == 1:
            s["disp"] += 1
        elif kind == 4:
            s["host"] += 1; s["hostms"] += dur / 1e6
        elif kind == 5:
            s["mark"] = op
    print("\n  s  phase blocks  maxgap_ms  gaps>4ms  maxproc_us  touch  setP   getP  disp  hostCalls hostCall_ms")
    for k in sorted(sec):
        s = sec[k]
        print(f"{k:3d}  {str(s['mark']):5s} {s['blocks']:6d}  {s['gap']:9.2f}  {s['late']:8d}  {s['pmax']:10.1f}  {s['touch']:5d} "
              f"{s['set']:5d} {s['get']:6d} {s['disp']:5d}  {s['host']:9d} {s['hostms']:11.2f}")


if __name__ == "__main__":
    main()
