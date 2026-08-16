
## 2026-08-16 — drag baseline v2 LOCKED: 25/50 + interpolated origin

Supersedes the morning baseline (70/140 quantised, tag `drag-baseline-20260816`). User-approved:
"good enough, we can stop here and take it as baseline".

**How it got here.** The morning pair was chosen against a dom0 apply lag nobody had measured. The
new `QGAPROTO,msg=MOTION` trace measured it - `L < 18 ms` (median 0, p75 17, 9 ms sampling) - which
made the 70 ms adopt 4-8x too large and the pacing bounded below by it. Ladder, scored on a
path-independent metric (injected-path reversals MINUS the reversals the hand actually made):

| rung | announce rate | excess reversals | deviation p90/max | backward jumps >=20px |
|---|---|---|---|---|
| 35/70 | 6.4/s | 208 (+38%) | 82 / 800 px | 7.5% of events |
| **25/50** | **13.7/s** | **118 (+23%)** | 67 / 261 px | 9.5% |
| 20/40 | 13.4/s | 180 (+36%) | 45 / 215 px | 8.9% |

25/50 is a floor in BOTH directions: below it adopt drops under dom0's real apply lag, the adopted
origin has not been applied, the error takes the wrong sign and the loop reopens (user: "jumps back
a bit"); and pacing under 50 returns nothing (13.4 vs 13.7/s) because the announce rate is already
saturated by the window's own movement and `CFG_POS_MIN_INTERVAL_MS`.

**Interpolated origin** then removed the residual: the origin now ramps between bracketing announces
evaluated at `now - InputDragLagMs` (10 ms) instead of stepping to a whole announce delta at once.
Chosen over the servo the owner floated, deliberately: a damper smooths the step by WITHHOLDING
motion, handing back the latency the ladder had just bought (~33 px of trail by the old servo's own
comment, the same order as the artefact). Interpolation adds none.

Net vs this morning: **dom0-visible update rate ~7/s -> 13.7/s, excess reversals 38% -> 23%.**

**Two-window overlay/z-order check** (owner-requested acceptance): three windows mapped
simultaneously - a green-on-black console overlapping a white Notepad - and every capture came back
complete, with correct chrome and **zero cross-window bleed** (0.0% green pixels inside either
Notepad capture despite real overlap). The grant slab pool is not leaking foreign pixels, the defect
it had when introduced.

LIMIT OF THIS CHECK, stated plainly: `qtest shot` returns PER-WINDOW captures, so it proves capture
isolation, NOT the composited z-order on screen. Which window dom0 actually draws on top is not
verified here and needs eyes on the display.

**Unproven / open**
- `L` is from ONE drag; the ladder rungs are one hand-drag each. Path-normalised, so comparable, but
  a repeat of 25/50 would firm up the +23%.
- Interpolation is deliberately NOT an exact model: dom0's origin genuinely steps (it applies each
  configure at one instant). Ramping is a hedge against L's variance (tail at 82, 398 ms). Guessing
  each step edge exactly is what 20/40 tried, and mis-timing one is what reopened the loop.
