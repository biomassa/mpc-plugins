# scalaSeq chance operations

Each note has 4 lanes. A lane has an operation (Op), a probability (Prob), a pattern (PTN) and an Amount. The lanes act from lane 1 to lane 4. The list is for scalaSeq 2.4.0, in the order of the Op knob. Id is the number the project stores.

| Id | Op | Amount (range, default) | What it does |
|---|---|---|---|
| 0 | - | none | No operation. |
| 1 | MUTE | none | Silences the hit. |
| 2 | UNMUTE | none | Cancels a MUTE from an earlier lane. |
| 3 | SKIP | none | Skips the step: the playhead goes past it. |
| 4 | UNSKIP | none | Cancels a SKIP from an earlier lane. |
| 5 | JMP< | steps, 1-16, 1 | Next step jumps back by Amount steps. |
| 6 | JMP? | none | Next step jumps to a random step. |
| 7 | JMP> | steps, 1-16, 1 | Next step jumps forward by Amount steps. |
| 8 | JMP TO | step, 1-64, 1 | Next step jumps to step number Amount. |
| 9 | VEL- | points, 1-100, 30 | Lowers the velocity. |
| 10 | VEL+ | points, 1-100, 30 | Raises the velocity. |
| 11 | VEL MAX | none | Full velocity. |
| 12 | GATE- | 1/16-4 steps, 1/4 | Shortens the note. |
| 13 | GATE+ | 1/16-4 steps, 1/4 | Lengthens the note (up to the next note). |
| 14 | TIE | none | Lengthens the note to the next note and ties into it. |
| 15 | RATCH | hits, 2-8, 2 | Plays the note as Amount ratchet hits. |
| 16 | P+ | degrees, 1-64, 1 | Pitch up by Amount scale degrees. |
| 17 | P- | degrees, 1-64, 1 | Pitch down by Amount scale degrees. |
| 18 | P?+ | degrees, 1-64, 0 = one period | Random pitch up, 1 to Amount degrees. |
| 19 | P?- | degrees, 1-64, 0 = one period | Random pitch down, 1 to Amount degrees. |
| 20 | P? | degrees, 1-64, 0 = one period | Random pitch up or down, up to Amount degrees. |
| 21 | DELAY | 1/16-1 step, 1/4 | Starts the note later; it keeps its length. |
| 30 | DELAY % | % of a step, 1-100, 25 | Starts the note later by Amount % of a step. |
| 22 | REPEAT | plays, 1-8, 1 | Plays the step again Amount extra times. |
| 23 | REVERSE | none | Reverses the play direction. |
| 24 | HOLD | none | Keeps the previous pitch. |
| 25 | P PREV | none | Takes the pitch of the previous note. |
| 26 | P NEXT | none | Takes the pitch of the next note. |
| 27 | VEL? | points, 1-100, 20 | Random velocity change, up to ± Amount. |
| 28 | GLIDE | % of the note, 10-100, 50 | Glides into the pitch over Amount % of the note. |
| 29 | NO GLIDE | none | Plays the hit without the note's GLIDE. |

Ops 0-27 come from the Max original (`dingus.pitchGateSeq`). Ops 28-30 are new in 2.4.0.
