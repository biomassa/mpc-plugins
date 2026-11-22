# Design: ratio rates, p-locks and CC lanes (percSeq 1.3, scalaSeq 2.8, both in one round)

Status: decisions from the user, 2026-11-22. Not built. Open points are at the end.

## 1. Ratio rates

- Each row (scalaSeq: the sequence) has a Rate Mode: Musical or Ratio.
- Musical: as now (Rate list, 1/64 ... 1 bar).
- Ratio: the step is the row's musical Rate divided by (Ratio + Fine). Ratio 1.5 = 1.5 times faster.
  Example: Rate 1/16, Ratio 1.33 -> a step every 1/16 / 1.33.
- Ratio list (15): 0.25 0.33 0.5 0.67 0.75 0.8 1 1.25 1.33 1.5 1.67 2 2.5 3 4.
- Fine: -0.10 to +0.10 in 0.01 steps, added to the ratio. The value shows the sum (for example 1.38).
- A ratio row lines up again at the song start, at MPC jumps and at each Reset (as the rows now).
- New RATE page (percSeq): Rate, Mode pads (Musical / Ratio), Ratio (field + pop-up list), Fine (fader),
  Swing, Direction, Steps, Range Start, Range End. In Musical mode Ratio and Fine are hidden.
- ROW page keeps: Row, MIDI Note, Velocity, Rev CC, CC Num, CC Rest, the row pads.

## 2. P-locks (per-hit values)

- Each hit has 4 lock values: off or 0-127.
- Each row has 4 CC numbers: Lock CC 1-4 (LOCK page).
- A locked value is sent just before the hit's note. An unlocked slot sends nothing.
- Same CC on a lane and a lock: the lock is sent at its hit; the lane continues at its next step.
- LOCK page: Lock 1-4 of the selected hit (see 5).

## 3. CC lanes

- 2 lanes per row (32 in total; scalaSeq: 2 lanes).
- Each lane: CC number, Steps (1-64), Rate, Mode, Ratio, Fine, Direction, Rest. One CC value (0-127) per step.
  It runs with its own playhead, independent of the hits.
- New LANE page:
  - Top 2/3: the bars of the selected lane of the selected row; touch-draw the values.
  - Bottom: lane pads VEL / CC 1 / CC 2, Row < >, and the selected lane's CC Num, Steps, Rate, Mode, Ratio, Fine.
  - VEL shows the velocities of the row's hits (moved here from the SEQ page).
- SEQ page: the velocity lane is removed; the roll gets its height.

## 4. Recording from the MPC

- Depends on the MPC. The user tests it on the device (test steps below). No build is needed.
- Test: put percSeq on a track, open the HIT page, arm automation recording (Read/Write on the track),
  play, turn the VEL Q-Link, stop, play again. Does the VEL fader move by itself?
  Repeat with a SETUP fader (for example Swing).

## 5. Further decisions (user, 2026-11-22)

- LANE page: pages like SEQ (the SEQ Steps/Page 8/16/32/64, < > page buttons, FOLLOW).
- Draw: a drag across the bars sets each bar to the finger height; a tap sets one bar.
- Look: amber bars; the current step is the white 25 % square behind the bar (as the SEQ playhead).
- Each lane has Rest (0-127, default 64), sent when the MPC stops.
- Each lane has Direction: Fwd / Rev / Rnd (as the rows).
- Lock faders: positions off, 0 ... 127; the left end shows "off".
- Mode pads: Musical lit amber, Ratio lit teal.
- Locks go on a new LOCK page (both plugins), not on HIT / NOTE (they have no 4 free slots).
- scalaSeq gets the same pages as percSeq: RATE, LANE, LOCK. Lock CC 1-4 are on LOCK.
- Tabs stay flat on the tab bar (percSeq 9 tabs, scalaSeq 10). Not known: if 9-10 tabs fit on the G2 tab bar.
  (My earlier claim "the MPC has 6 function keys" was not measured and was wrong: scalaSeq has 7 working tabs.)

## 6. Page layouts (approved by the user, 2026-11-22)

Columns are Q-Link groups.

```
RATE (scalaSeq: no Row; its SETUP loses Rate, Direction, Steps, Swing, Range)
| Row < >    | Fine       | Range Start | -          |
| Rate field | Swing      | Range End   | -          |
| Mode pads  | Direction  | -           | -          |
| Ratio fld  | Steps      | -           | -          |
(Ratio and Fine hidden in Musical mode)

ROW
| Row        | Rev CC     | -           | row pads   |
| MIDI Note  | CC Num     | -           | 4 x 4      |
| Velocity   | CC Rest    | -           |            |

LOCK (scalaSeq: Note < > in place of Hit and Row)
| Lock 1     | Lock CC 1  | Hit < >     | -          |
| Lock 2     | Lock CC 2  | Row < >     | -          |
| Lock 3     | Lock CC 3  | -           | -          |
| Lock 4     | Lock CC 4  | -           | -          |

LANE
header: [VEL][CC 1][CC 2]  Row < n >   < page >  FOLLOW
lane bars (middle of the page)
| CC Num     | Steps      | Rate field  | Mode pads  |
| Ratio fld  | Fine       | Direction   | Rest       |
(settings hidden while VEL is selected)
```

## 7. Open points

- LOCK and RATE "Row < >": select the row (as Sel Row), not move the hit (as on HIT). To confirm at build start.
- Parameter count: percSeq has 1,221 parameters now. Estimated new: about 100. Measure the display traffic
  of a lane redraw before release.

## 8. Defaults and CC numbers (user, 2026-11-22)

- Lane CC Num and Lock CC 1-4 default to off (nothing is sent until a number is picked).
- New lane bars: 64. A lane sends its CC only when the value changes (the first step after a stop always sends).
- Lane CC Num and Lock CC skip: 0-5, 7, 10, 11, 32, 64-68, 98-101, 120-127 (101 numbers left, plus off).
  The MPC guide documents no preset CCs for sound parameters (filter, decay): use MIDI Learn (guide p319).
- Locks: the lock values come from the hit; the CC numbers from the row of the sound that plays (as the CC op).
