# The NV2A Ack Thread

> Part of the [Documentation Index](../INDEX.md)

> **Status: to do.** This page describes the thread as it is and a design to
> replace its polling. Nothing in the design is implemented yet.

The toolkit has no GPU behind the NV2A's registers, so one host thread plays
its part: `nv2a_ack_thread` in `src/kernel/xbox_memory_layout.c`. It answers
what XDK D3D asks the GPU, walks what the title submits, and keeps a few
clocks moving. It works by polling, and it holds a whole host core in every
title.

## What It Does Today

Each pass of the loop:

- clears the PFB flush bit, which every D3D kickoff sets and waits on;
- holds five interrupt status registers at zero (PMC, PBUS, PFIFO, PGRAPH,
  PCRTC). They are write-1-to-clear on the console; the aperture is plain
  memory, where an ISR's acknowledging write sets the bits instead, and
  only the next pass takes them down. The vblank bits belong to the
  vblank-ack thread and PGRAPH's to the software-method trap while those
  are on;
- keeps the two PFIFO idle bits set;
- polls a held flip (`nv2a_pb_exec_flip_poll`);
- ticks the fence, DSP, counter and frame-counter mirrors and the `POKE`
  probes the project registered;
- when `DMA_PUT` moved, walks the new segment (fences, semaphores, flips,
  software-method traps; see [Pushbuffer Executor](pushbuffer-executor.md))
  and then sets `DMA_GET` to `PUT`;
- adds 1 to each MCPX counter (the APU sample counter), unless the emulated
  APU owns those registers;
- writes `KeTickCount` from `GetTickCount()`.

Then it ends with `Sleep(0)`. That gives way to another ready thread, and on a
machine with idle cores there is none, so it returns at once and the loop runs
tens of thousands of passes a second, nearly all of them finding nothing to
do. Halo's cryo bay profiled the thread at 97% of a core: 56% in
`ZwDelayExecution` (the `Sleep(0)`) and 25% reading the clock
(`xbox_VblankAckHeld` → `kernel_clock_ms`, asked once per register in the
table, so up to six times a pass).

The MCPX counters show the cost is not only CPU: they go up once per pass, so
their rate is the loop's speed, not time.

## How It Got Here

- **0483f1a** (upstream, 2026-07-20) created the thread to answer D3D's
  set-a-bit-then-wait handshakes (Halo hung in the pushbuffer kick). The
  `Sleep(0)` is commented "yield; the waiter is spinning on another core":
  it was written for latency, and CPU cost was not the question.
- **91589dc** (2026-11-14) sleeps `RECOMP_ACK_HOLD_SLEEP_US` (500 µs) per pass
  while a flip is held for its vblank. Burnout 3's ack thread went from 93% to
  32% of a core at the same frame rate. Outside a hold it still only yields.
- **663c953** (2026-11-15): a renderer hook in front of D3D's `KickOff`
  (`nv2a_pb_flip_hold_wait`) sleeps the title while a flip is held and then
  announces the kickoff, and the thread spins on the announcement
  (`kick_ack_spin`, `RECOMP_KICK_SPIN_US`) to clear the flush the moment it is
  set. MechAssault went from 80 to 91 fps.

A title that paces itself and never holds a flip (Halo) never reaches the
sleep.

## Design: React Instead of Poll

A GPU does not poll. It reacts to the CPU's register writes, and its clocks
run on their own. Each job above has one of three kinds of trigger, and can
be met the way the hardware meets it.

| Job | Trigger | Handled by |
|---|---|---|
| PFB flush bit | the title writes a GPU register | the write itself |
| interrupt status registers | the title writes a GPU register | the write itself: the bits written clear |
| walk, `DMA_GET`, fences, semaphores, flips | a `PUT` write | a worker woken by it |
| `KeTickCount`, MCPX counters, frame counters | time | a clock thread |
| flip release | time | the timer thread, at the vblank |
| idle bits | nothing | set once, checked by the safety net |
| fence, DSP and counter mirrors, `POKE` | the title writes its own RAM | each walk, and the safety net |

### Register writes

The toolkit already traps writes to register pages: the page is read-only,
and a vectored exception handler sees the write and acts on it (PGRAPH's
write-1-to-clear and software-method trap, AC97, the APU). The same mechanism
covers these pages of the aperture:

| Page | Register | On a write |
|---|---|---|
| `0x100000` | PFB flush (`0x100410`) | the bit is cleared |
| `0x800000` | `DMA_PUT` (`0x800040`) | the worker's event is signalled |
| `0x000000`, `0x001000`, `0x002000`, `0x600000` | PMC, PBUS, PFIFO, PCRTC interrupt status | the bits written clear |
| `0x400000` | PGRAPH interrupt status | as today under `RECOMP_PGRAPH_TRAP`, always on |

- **The flush** is completed in the handler, so D3D's wait loop finds it clear
  on its first read. This replaces `kick_ack_spin` and the kickoff
  announcement.
- **While a flip is held** the flush must stay unanswered: that is how the
  title feels the console's `FLIP_STALL`. The handler then waits for the
  release before it completes the write (up to `XBOX_FLIP_HOLD_MAX_NS`, on an
  event the release sets), holding no lock. The title sleeps in its own
  register write, where today it sleeps in the renderer's hook just before
  it, so the hook goes too and pacing holds for any title and XDK.
- **A `PUT` write** signals an event. The worker sleeps on it, walks the
  segment and advances `GET`. The walk stays off the title's thread, as on
  the console, so the title is never re-entered from its own register write.
- **The interrupt status registers** cannot wait for the safety net: Halo's
  graphics ISR writes its acknowledgement, reads the bit back and re-enters
  until the stack is gone when it is still set. Their writes clear in the
  handler, as PGRAPH's do today. The vblank bits stay with the vblank-ack
  thread, whose one-second backstop (`xbox_VblankAckHeld`) moves to the
  safety net.

### The toolkit's own writes

`DMA_GET` (`0x800044`) is on `PUT`'s page, and the walker writes it at every
command. The executor and the trap raise write PGRAPH's page. On a read-only
page those writes fault too, and opening the page around each one
(`xbox_Nv2aPgraphRaise` does today) costs two `VirtualProtect` calls a write.

So the aperture becomes a section mapped twice, like guest RAM and its
mirrors: the guest's view at the register base, with the trapped pages
read-only, and a host view elsewhere that is always writable.
`xbox_Nv2aRegPtr` and the toolkit's other register writes go through the host
view and never fault. The aperture is private `VirtualAlloc` memory today
(`layout_commit_at`), which has no second view.

### Time

Three clocks move to a clock thread of their own, which sleeps 1 ms on the
high-resolution timer and does nothing else: `KeTickCount`, the MCPX counters
(derived from the clock, not counted; only while the emulated APU does not
own them) and the frame counters' fallback tick.

They do not go on the kernel timer thread. It runs the title's DPCs itself,
and a DPC can hold it: Halo's DSOUND DPC stalls it with
`KeStallExecutionProcessor`. On the console the clock interrupt preempts a
DPC, so a DPC that waits on `KeTickCount` or the sample counter sees them
move; on the timer thread it would wait forever. Without a vblank mode or the
dispatch gate that thread is also a plain `Sleep(10)` loop.

The timer thread does take the release of a held flip: it already wakes at
each vblank edge. It sets the event the worker and a title sleeping in a
flush write wait on. A release delayed by a DPC is bounded by the hold's
limit, as today.

The MCPX counter's rate is open. One per pass was never a rate; the sample
clock (48 kHz) is the candidate, to be checked against what DirectSound's
`SetupVoiceProcessor` waits for.

### Safety net

The worker also wakes every few milliseconds and runs one full pass, as
today. It counts the passes that found work no write announced, by job. A
non-zero count names a handshake the traps miss.

Nothing is logged while the title runs. The counts are printed at a clean
exit and offered through an accessor for the renderer's performance panel:
headless runs end with `taskkill`, which prints nothing.

### Wake latency

A busy-bit handshake is answered sooner than today: during the write, not on
the next pass. The walk is not. Today the spinning thread sees `PUT` move
within a pass; a sleeping worker has to be woken first, tens of microseconds
on Windows, and D3D waits on `GET` and on fences. 663c953 found 25 µs per
kickoff worth a tenth of MechAssault's main thread. So the worker spins for a
short, tunable time after its last work before it sleeps, as `kick_ack_spin`
does now, and the A/B checks set the default (0 if it buys nothing).

## Measurements

What to measure before building, against the loop as it polls today. Each
names the result that would change the design. One short run per title at a
busy spot (Halo, MechAssault, Burnout 3, Splinter Cell, DOAX), the same spot
every time.

### Counters in the loop

One instrument answers most of it: what each pass found. Plain increments
behind one environment variable, printed at a clean exit and when a frame
capture is asked for, never per event.

- passes that found work, by job: flush cleared, each interrupt status
  register cleared (and which bits), idle bits re-set, `PUT` moved, each
  mirror or `POKE` that changed something, the vblank backstop firing;
- total passes and frames, to read the rest per frame and per second;
- latency histograms (powers of two, in µs): kickoff announcement to flush
  cleared, and `PUT` seen to `GET` equal to `PUT`.

| Result | Effect on the design |
|---|---|
| Idle bits re-set more than once | not "set once": trap their page or leave them to the safety net |
| An interrupt status register never cleared in any title | its page is not trapped; the row goes |
| One cleared often | confirms its trap; the bits say who raises it |
| A mirror changes on passes where `PUT` did not move | each walk and the safety net is too slow for it; it needs another trigger |
| The vblank backstop fires in normal play | it cannot move to the safety net as it is |

### Writes per page

The title's writes to each candidate page, per frame, with the writer.
`RECOMP_WATCH` faults on writes to a page already; a count-only mode of it,
one page a run, if it can be pointed at the aperture, and a small counting
handler if not. The writers also show whether `KickOff` writes `PUT` before
or after the flush.

- thousands of writes a frame rule the page out;
- writes to other registers of the flush's or `PUT`'s page change the cost.

### Trap cost

Time one round trip of the PGRAPH handler (fault, single step, two
`VirtualProtect` calls), in a standalone test and in Halo under
`RECOMP_PGRAPH_TRAP=1`, and multiply by the writes per page. Above about 1%
of a frame in any title, emulating the store in the handler is no longer
optional.

### Wake latency

Standalone: one thread sets an event, another sleeps on it; a histogram of
the delay, with and without a short spin before the sleep. Compared with the
`PUT`-to-`GET` histogram above: no worse than a pass today and the spin
leaves the design; worse, and the histogram sets its default.

### What the polling buys

Phase 6, predicted before any trap exists. An experimental setting makes the
loop sleep N µs after a pass that found nothing, the kickoff spin left on: 0,
100, 500 and 2000, in alternating pairs, frame times from the performance
panel and per-thread CPU from `hostprof.py`.

- a title that slows or hangs at 2000 has a handshake that needs fast
  polling, and the counters name it;
- a title that does not change is safe at that safety-net period;
- titles whose renderer has the kickoff hook, against one without, show what
  the flush trap has to replace.

### Clocks and DPCs

- how long DPCs hold the timer thread (maximum and histogram), and the
  `KeStallExecutionProcessor` calls made on it;
- the guest code that waits on `KeTickCount` or the APU sample counter, and
  the thread it runs on. One such wait in a DPC confirms the clock thread;
  none in five titles allows the timer thread after all;
- the value `SetupVoiceProcessor` waits for and how long that takes today,
  for the counter's rate.

### Baseline

Before phase 1, per title at its spot: frame-time percentiles, per-thread CPU
and the counters. Numbers are quoted from runs with the counters off, and not
from the seconds after a movie, when the graphics card is still clocking up.

## Phases

Each phase is its own commit with its own A/B check (switch off and on, in
alternating pairs on a quiet machine: frame times and per-thread CPU). The
thread keeps polling until phase 5, so each earlier phase is checked against
a loop that still catches whatever it misses.

1. **One clock read per pass.** `xbox_VblankAckHeld` is asked once, not per
   register. Independent of the rest, and most of the 25%.
2. **Clocks to the clock thread.** `KeTickCount`, MCPX and frame counters move
   off the loop; the counters follow time.
3. **Measure** (see [Measurements](#measurements)). A page written too often
   is not trapped, and its row goes back to the safety net with the reason.
   The counters and the baseline come before phase 1.
4. **The aperture as a section** with the host view; the toolkit's writes move
   to it. PGRAPH's open-the-page windows go.
5. **Trap the writes, still polling.** Flush, `PUT` and interrupt status are
   handled at the write; the loop runs as before and counts what it still
   found to do. The counts have to be zero in every title before phase 6.
6. **Sleep on the event.** The worker waits on its event with the safety-net
   timeout and the short spin instead of `Sleep(0)`. A switch keeps the old
   loop.
7. **Remove** `kick_ack_spin`, the announcement and the renderer hook's wait.

## Risks

- **Trap cost.** Each trapped write is two exceptions and two protection
  changes, and a kickoff makes at least two such writes (about 30 kickoffs a
  frame in MechAssault). Phase 3 measures it; it is not known yet. It rules
  out any page with a register written thousands of times a frame.
- **Writes slipping past.** The current handlers make the page writable and
  single-step the write. Another title thread writing in that window is not
  seen. Emulating the store in the handler closes it; until then the safety
  net catches it, a few milliseconds late.
- **Handler interplay.** Several vectored handlers already exist (watch, AC97,
  APU, PGRAPH); their order and re-entry need care. The flush handler sleeps
  during a hold, so it must not hold the page's lock while it does.
- **Instant completion.** A real flush takes time. Completing it at once is a
  legal GPU timing, but it can expose a race in a title that never lost it
  before.
- **Idle bits set once.** Nothing is known to clear them, and the loop has
  always re-set them, so nothing would have shown it. The safety net keeps
  re-setting them and counts when it had to.
- **Title-written mirrors.** The fence, DSP and counter mirrors copy values the
  title writes into its own RAM, which cannot be trapped. They tick on each
  walk and on the safety net. A title that waits on one without kicking off
  sees it at the safety net's rate.
- **Every title is affected.** The A/B checks cover Halo, MechAssault,
  Burnout 3, Splinter Cell and DOAX before a game moves its pin.

## Not Covered

Spins in the title's own code are separate. Halo's frame pacer spins on the
vblank counter (it sleeps only when a flag is set), and its DSOUND DPC stalls
the timer thread with `KeStallExecutionProcessor`.
