# The Flip Interval

> Part of the [Documentation Index](../INDEX.md)

> **Status: the forced cap has run in one game; the title's calls in none.**
> `RECOMP_FLIP_INTERVAL_MAX=1` was run in MechAssault (menus at 60, menu
> movies fast). The cap and policy calls are unit-tested (`tests/flip_hold`);
> no title calls them yet.

## What the Title Asks For

The console's video output always refreshes at 60 Hz (a vblank). With each
frame, XDK D3D's Swap tells the GPU how many vblanks must pass since the last
flip before this one may be shown: the presentation interval, 1 (ONE, at most
60 frames a second), 2 (TWO, at most 30) or 3, or IMMEDIATE (no wait). It
travels in the swap's software-method NOP, just before `FLIP_STALL`. A title
chooses it per frame, and many choose differently per screen: Splinter Cell
asks for ONE in its menus and TWO in a level.

With `RECOMP_FLIP_PACING` on (and `RECOMP_VBLANK=paced`), the flip hold
(`nv2a_flip_hold.c`) obeys it: the flip is held until the guest's vblank
count reaches the previous flip's vblank plus the interval. See
[Pushbuffer Executor](pushbuffer-executor.md).

## Why Change It

A title that asks for TWO runs at 30 frames a second, even on a PC that could
draw twice as many. Holding its flips for one vblank instead shows its frames
at 60. Everything the title waits on is still produced: vblanks, the flip
completion (`FLIP_STALL` released, the PFB flush acknowledged, frame counters
bumped). Only the wait is shorter.

What the title does with a shorter frame is its own business:

- Logic timed on a clock (the vblank count, the performance counter) adapts:
  each frame covers half the time, and the title plays at its normal speed
  with twice the frames.
- Logic stepped once per frame (a fixed 1/30 s, or a movie frame per flip)
  runs twice as fast. MechAssault's menu movies do, with the cap on all the
  time.

No rule in the toolkit can tell the two apart from the pushbuffer. So the
toolkit gives the decision to the game's own code, and keeps a switch that
overrides it.

## The Controls

Three layers, the first that is set wins:

| Layer | Set by | Lasts |
|---|---|---|
| Forced cap | `RECOMP_FLIP_INTERVAL_MAX=0..3`, or `xbox_FlipIntervalMaxForce(max)` | until released (`xbox_FlipIntervalMaxForce(-1)`); the title's calls are kept, not used |
| Policy | `xbox_FlipIntervalPolicySet(fn)` | until cleared (NULL) |
| Cap | `xbox_FlipIntervalMaxSet(max)` | until set again |

- **Cap** (`max`, 0-3): a swap asking for more than `max` vblanks is held for
  `max`; 0 holds each frame as asked (the default). A game calls it when it
  knows its state changed: after a level has loaded, 1; when a movie or menu
  starts, 0.
- **Policy**: `unsigned fn(unsigned asked)` is called at every `FLIP_STALL`
  with the interval the swap asked for (0 for IMMEDIATE, at most 3) and
  returns the one to hold for (0 flips at once, above 3 reads as 3). It may
  also lengthen an interval; a lengthened IMMEDIATE or ONE_OR_IMMEDIATE is
  held as a plain interval. Answering what it was asked keeps the swap's own
  interval, so it cannot cut a 4-7 (which XDK D3D does not write) to 3. It runs on the GPU thread, between pushbuffer methods: it may
  read guest memory (a game-state variable) but must not call guest code,
  take a guest lock or block. One call may still run just after the policy
  is cleared or replaced, so the function and what it reads stay valid for
  the whole run.
- **Forced cap**: set in the environment, it wins over both, for A/B runs:
  `1` shows every TWO frame at 60 (the cap on all the time), `0` holds every
  frame as asked whatever the game's code says. Unset or empty, the game
  decides. Any other value is reported and ignored.
  `xbox_FlipIntervalMaxForce` is the same switch while running, for a
  settings panel: 0-3 forces the cap, -1 releases it, and the title's last
  cap and policy decide again. `xbox_FlipIntervalMaxForced` reads it back
  (-1 when the title decides, -2 with flip pacing off).

All three need `RECOMP_FLIP_PACING` (and so `RECOMP_VBLANK`): without the
hold, nothing waits for the interval anyway. With pacing off, the two calls
and `xbox_FlipIntervalMaxGet` return 0, and a set
`RECOMP_FLIP_INTERVAL_MAX` is reported as ignored.

The declarations are in `src/kernel/nv2a_backend.h`. The pure rule
(`xbox_FlipHoldSetCap`, `xbox_FlipHoldSetPolicy`, `xbox_FlipHoldForceCap`,
`xbox_FlipHoldReleaseCap`) is
in `nv2a_flip_hold.c`.

## What the Log Shows

- `[GPU] flip interval cap forced to N (RECOMP_FLIP_INTERVAL_MAX)`: the
  environment override is in force.
- `[GPU] flip interval cap forced to N` / `flip interval cap no longer
  forced: the title decides`: the switch changed while running.
- `[GPU] RECOMP_FLIP_INTERVAL_MAX ignored: flip pacing is off`: it is set
  without `RECOMP_FLIP_PACING`.
- `[GPU] title sets flip interval cap N` / `title sets|clears a flip
  interval policy`: only when the cap or policy changes, so a game may call
  every frame.
- `[GPU] the title's flip interval cap and policy are ignored
  (RECOMP_FLIP_INTERVAL_MAX)`: once, at the title's first call under a
  forced cap.
- The periodic `[GPU] flip hold:` line: `ivN` still counts what the title
  asked for; `changed N (forced|policy|title, cap M)` counts frames held for
  another interval than asked, and which layer was deciding at the report.

## Where a Game Gets Its Signal

Game code, in the game's repository (the toolkit stays game-neutral):

1. **A state variable read in the policy**: find the guest variable for the
   screen (menu, mission, movie) by comparing memory between screens. No
   function hook and no regen.
2. **An indirect call**: when the movie player or the level loader is reached
   through a function pointer, `recomp_lookup_manual` returns a wrapper that
   sets the cap and calls the generated function.
3. **A direct call**: replacing a directly called function means defining it
   in `recomp_manual.c`, and calling the original then needs the generated
   body under another name. The last resort.

A game still wants a switch of its own (an environment variable read in its
`main.c`) so that 60 stays opt-in per game.

## Not Covered

- A title stepped once per frame in gameplay too gets nothing from this:
  it needs its step halved (a patch in the game's code), not a shorter hold.
- A title that paces itself on the CPU without asking for TWO (Halo's
  limiter in its own repository) never reaches the hold with an interval
  above 1.
- Under `RECOMP_PGRAPH_TRAP=1` the title's own graphics interrupt handler
  still gets the swap with the interval the title asked for; only the
  toolkit's hold is shortened. Whether such a title then reaches 60 has not
  been checked.
- A live toggle in the renderer (an F-key calling `xbox_FlipIntervalMaxSet`)
  would help find which screens hold up at 60. Not built.
