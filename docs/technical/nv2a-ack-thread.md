# The NV2A Ack Thread

> Part of the [Documentation Index](../INDEX.md)

> **Status: phase 3 built and measured; opt-in per title.**
> Phase 1 (one clock read a pass) is always on. Phase 3, the sleeping
> thread, is behind `RECOMP_ACK_SLEEP=1`: paced, it costs nothing in any of
> the five titles and takes the ack thread from 19-96% of a core to 4-12%;
> uncapped, MechAssault loses 12%. The switch depends on no other setting.
> Each of the five games defaults it to 1 in its `main.c`;
> `RECOMP_ACK_SLEEP=0` keeps the polling loop. No trap was needed. See
> [Phases](#phases) for what is done, open and dropped. The counters, the
> write count and the bench stay (`RECOMP_ACK_STATS`, `RECOMP_ACK_WRITES`,
> `tools/bench/nv2a_trap_cost.c`).

The toolkit has no GPU behind the NV2A's registers, so one host thread plays
its part: `nv2a_ack_thread` in `src/kernel/xbox_memory_layout.c`. It answers
what XDK D3D asks the GPU, walks what the title submits, and keeps a few
clocks moving. It works by polling, and it holds a whole host core in every
title.

Where this work lives: the counters in `src/kernel/nv2a_ack_stats.c`, the
sleep in `src/kernel/nv2a_ack_sleep.c`. The loop calls into them at a few
marked places and keeps everything else; with `RECOMP_ACK_SLEEP` off it
yields as before.

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

This section is the design as first written, with a trap on each register
page. The measurements since keep its split of the jobs and its clock thread,
and replace most of the traps with the renderer's kickoff call: the
[Phases](#phases) say what is built and in what order.

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
short, tunable time after its last work before it sleeps, as the kickoff spin
does, and the A/B checks set the default (0 if it buys nothing).

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

The sleeping worker, predicted before it exists. An experimental setting makes the
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

### Results so far

Halo, cryo bay, 2026-11-18 (`RECOMP_ACK_STATS`, `tools/bench/nv2a_trap_cost.c`):

- 30 fps, counters on or off. The ack thread holds 97% of a core: half in
  `Sleep(0)`, 38% reading the clock. The main thread spends 71% in the
  title's own frame pacer.
- 2.2 million passes a second, 99.8% under 1 µs; one in 300 finds work.
- About 7,550 `PUT` moves and 7,750 flushes a second: 250 kickoffs a frame,
  not the 30 assumed above. The kickoff spin answers 96% of the flushes,
  99.5% of them within 1 µs.
- No interrupt status register, idle bit, mirror or vblank backstop fired
  during play; the interrupt registers and idle bits only at boot.
- One trapped write, standalone with one handler: 5.2 µs today
  (single step, two `VirtualProtect`), 1.5 µs with the store emulated
  through a second view. At 15,000 trapped writes a second that is 8% and
  2.3% of Halo's main thread: both above the 1% line, before the other
  vectored handlers the game registers.
- Event wake, gaps of 0-266 µs: median 3.9 µs, 99th percentile about 45 µs.
  A 20-50 µs spin first halves the 99th percentile; 200 µs catches nearly
  every wake, and at these gaps is polling again.

- Writes to the PFB and PFIFO USER pages (`RECOMP_ACK_WRITES=1`, headless
  boot to the menu): the title writes two registers there and nothing
  else, both from `CDevice_KickOff`, flush first, then `PUT`, once each per
  kickoff. Every other write on those pages is the toolkit's own (`DMA_GET`).

So trapping both kickoff writes does not pay in Halo. Since the flush comes
first and `KickOff` waits on it before writing `PUT`, the flush alone can
carry a kickoff: answered in its handler, it can also wake the worker, which
finds `PUT` written a few microseconds later. That is one trap per kickoff,
about 1.1% of Halo's main thread at 1.5 µs. Cheaper still, the renderer
already calls `nv2a_pb_flip_hold_wait` just before `KickOff` in titles it
hooks: a plain call that can wake the worker with no trap at all.

The other four titles, 30 s headless boots with `RECOMP_ACK_WRITES=1`
(menus and intro movies, not busy spots):

| Title | Kickoffs a second (write count on) | Flush written by | `PUT` written by |
|---|---|---|---|
| Splinter Cell | 77 | `CDevice_KickOff` +0x2A0 | `CDevice_KickOff` +0x2F9 |
| MechAssault | 3,580 | `CDevice_KickOff` +0x2A0 | `CDevice_KickOff` +0x2F9 |
| Burnout 3 | 120 | a function `KickOff` calls (LTCG) | `CDevice_KickOff` (LTCG) |
| DOAX | 92 | `CDevice_KickOff` +0x2A0 | `CDevice_KickOff` +0x2F9 |

- Every title writes the flush, then `PUT`, once each per kickoff, and no
  other register on those pages. A few flushes come with no `PUT` after
  them (MechAssault: 1 kickoff in 80).
- In every title each flush was announced first through the renderer's
  call: the kickoff-latency count equals the flush count. The renderer's
  hook covers every kickoff in all five, Burnout 3's LTCG build included.
- No interrupt status register, idle bit, mirror or backstop fired after
  boot in any of them.

So the renderer's call reaches every kickoff in these five titles, and no
trap is needed to know of one. What it costs to answer a kickoff from a
worker that was asleep is a separate question, not measured yet: see the
review below.

### Review of the results

A review of the results above, the counters' code, the bench and the saved
Halo logs (2026-11-18). No counting error was found that changes a number
above.

What holds:

- The loop is nearly all waste: 2.2 million passes a second, one in 300
  with work, and the kickoff spin answers 99.7% of the announced flushes
  within 1 µs.
- Trapping both kickoff writes costs too much (8% or 2.3% of Halo's main
  thread).
- All five titles write the flush, then `PUT`, and the renderer's call
  announces every flush. A kickoff-latency sample is taken only when a
  cleared flush had an announcement pending, so equal counts mean no flush
  came unannounced.
- Halo's interrupt status clears are boot-time only. The bits ever cleared
  are `FFFFFFFF` on PFIFO and PGRAPH, which reads as the title's own
  clear-everything write at start-up, not an ISR acknowledging.

What is weaker than written:

- **A wake replaces the trap.** With no trap the title writes the flush
  and spins until the worker answers. A sleeping worker takes 3.9 µs
  (median) to 45 µs (99th percentile) to wake, by the bench: more than
  the 1.5 µs emulated trap, which answers in the handler. The renderer's
  call is the cheaper way only while the worker is still spinning when the
  kickoff comes.
- **The bench's gaps are not Halo's.** It draws gaps evenly from 0-266 µs.
  Halo's main thread spends 71% of the frame in its pacer, so its 250
  kickoffs fall in about 10 ms, some 40 µs apart, and then nothing for
  23 ms. A short spin probably covers the whole burst and leaves one wake
  a frame. The real gaps were not measured.
- **The cryo bay cannot show a regression.** Halo is held at 30 fps there
  with 71% of the main thread to spare, so even 8% would not move a frame
  time. The 1% line matters in a title bound by its main thread:
  MechAssault, where 25 µs a kickoff cost a tenth of the frame rate.
- **The four titles' kickoff rates were taken with the write count on.** It
  single-steps every `DMA_GET` write the walker makes, one per command, so
  the walker and the rates were slowed. They are not baseline numbers.
- **Flushes with no `PUT` after them may be the instrument.** While a page
  is open for one single-stepped write, another thread's write is not
  counted, and the walker opens `PUT`'s page all the time. Halo's own
  counters cannot settle it either: one pass sees `PUT` move once however
  many kickoffs came during a walk.
- **"No mirror fired" tested nothing.** Each title registers its fence
  mirror only without `RECOMP_PB_EXEC`, and these runs had it on. Burnout
  3's frame counter is the one mirror that was live.
- **The other four titles were measured in menus and intro movies**, so
  "nothing fired after boot" does not cover a level load or play.
- The kickoff-latency histogram starts at the announcement, not at the
  title's flush write. With the spin watching, a flush answered by the spin
  is answered within a pass of its write, so for those the histogram is the
  time from announcement to the flush write: under 1 µs for 99.5% in Halo.
  `KickOff` writes the flush right after the hook returns, and a worker
  woken by the announcement has no head start: the title waits out the
  whole wake.

A second review, of the design against the loop (2026-11-18):

- **A sleeping worker stops the per-pass clocks.** The MCPX counters go up
  once a pass and `KeTickCount` is written once a pass. Asleep between
  bursts, the worker would slow the counter a thousandfold and leave the
  tick count up to a safety-net period stale. The counter has to follow
  time before the worker sleeps, not in a later phase; and "what the
  polling buys" changes the counter's rate with each sleep it tries, so it
  cannot tell a missing handshake from a slow counter until then.
- **The spin should end on `PUT`, not on a time.** The flush comes first and
  `PUT` a few microseconds later; a worker that stops spinning between them
  is woken late for the walk. It spins until `PUT` moves after an answered
  flush, with a limit for the flushes that have no `PUT` after them.
- **The safety net needs a fine timer.** A wait with a timeout of a few
  milliseconds rounds to the 15.6 ms system tick unless the timer
  resolution is raised: a high-resolution waitable timer, as the clock
  thread plans.
- **Jobs the table above leaves out**: `frame_counters_tick`,
  `framebuffer_probe_tick`, the walk repeated every 2 s while `PUT` does
  not move, and the kickoff spin, which starts only at the top of a pass.
  Each needs a place once the loop sleeps.
- **The announcement should signal only a sleeping worker.** Otherwise each
  of Halo's 7,500 kickoffs a second pays a system call while the worker is
  already spinning. A flag the worker sets before it waits, or
  `WaitOnAddress` on the announcement itself, keeps the title's side to a
  memory read.
- **Phase 1 speeds the counter up for now.** Fewer clock reads make passes
  faster (2.5 to 3.3 million a second at Halo's menu), and the MCPX counter
  with them. Baselines are taken after phase 1.

### Still to measure

What each item decides, in order:

1. **The gaps between kickoffs** (`gap` in `RECOMP_ACK_STATS`, from
   `nv2a_pb_flip_hold_wait`), with the four titles' kickoff rates and
   flushes against `PUT` moves from the same runs (write count off), at
   each title's busy spot. Bursts mean a short spin covers each and leaves
   one wake a frame: phase 3 stands. Gaps spread wider than the spin mean
   every kickoff pays a wake (4-45 µs), and phase 3 gives way to an
   adaptive spin or the flush trap. The one result that can overturn it.
   Halo's menu, at boot: one kickoff every 32-65 ms, and while loading,
   gaps of 256 µs to 1 ms that no short spin covers.
2. **What the freed core is worth**: the old loop against the new with the
   process held to 2 and to 4 cores. No gain beyond the CPU figure keeps
   the plan to phases 1 and 3.
3. **The MCPX counter's rate** (from Clocks and DPCs above): the value
   `SetupVoiceProcessor` waits for. Blocks phase 3.
4. **What the polling buys** (above), once the counter follows time. A
   title that slows at 2000 µs names a handshake that needs a faster
   trigger.
5. **MechAssault in a level**: the baseline for the A/B checks, not a
   question.
6. **The rest of Clocks and DPCs**, before phase 5 (clock thread or timer
   thread).

Item 1 at the busy spots, 2026-11-18 (`gap`, steady 5 s intervals):

| Gap | Halo, cryo bay | MechAssault, in a level |
|---|---|---|
| Kickoffs a second | 7,700 | 3,950 |
| < 16 µs | 95.5% (mostly 4-8 µs) | 50% (mostly < 2 µs) |
| 16-128 µs | 0.3% | 0.4% |
| 128 µs - 1 ms | 3.6% | 39% |
| 1-8 ms | 0.5% | 10.5% |
| longer | 0.3% (the pacer, one a frame) | < 0.1% |

- **Halo bursts.** About 250 kickoffs a frame in some 2 ms, 4-8 µs apart.
  A 16-32 µs spin covers 95.5% and leaves about 10 wakes a frame (320 a
  second); nothing between 32 and 256 µs adds coverage. The thread would
  spin about 2 ms a frame: some 6% of a core, down from 97%.
- **MechAssault is the spread case.** Kickoffs come in pairs under 2 µs
  apart (the second finds the worker still walking the first: half the
  flushes are answered by the walker), and the pairs are 256 µs to 1 ms
  apart, evenly. A short spin covers only the second of each pair: about
  2,000 wakes a second, each one waited out by the main thread. At the
  bench's 4-45 µs that is 1-9% of a main-thread-bound title. A spin long
  enough to cover the pairs (1 ms) never sleeps at this spot.
- In both, the flush is answered within 1 µs of the announcement in over
  99.5%: the spin is not the slow part today.

So a fixed spin fits Halo and not MechAssault. The bench's wake times are
not MechAssault's, though: the cost of 2,000 wakes a second is settled by
phase 3 itself, as an A/B check in MechAssault's level, not by more
counting. If it costs more than about 1%, MechAssault needs the flush trap
(one emulated trap a kickoff, about 0.6% at 4,000 a second) or keeps
polling while kickoffs come faster than a threshold.

`SetEvent`'s cost on the title's thread needs no run of its own: the
announcement signals only a sleeping worker, and phase 3's A/B check
includes it.

## Phases

Reordered after the review. Each phase is its own commit with its own A/B
check (switch off and on, in alternating pairs on a quiet machine: frame
times and per-thread CPU), in Halo and in MechAssault in a level.

1. **One clock read per pass.** Done: `xbox_VblankAckHeld` is asked only
   when a vblank bit is up, once a pass (`RECOMP_ACK_CLOCK_EACH=1` asks per
   register, as before).
2. **Measure** what is [still to measure](#still-to-measure), items 1-5.
   Mostly done: the kickoff gaps (item 1) and MechAssault in a level
   (item 5). Direct A/B runs of phase 3 replaced what the polling buys
   (item 4). The freed core (item 2) and the clocks and DPCs (item 6) were
   not measured: with the sleep opt-in per title they no longer decide
   anything. The MCPX counter's rate (item 3) is moot in the five titles,
   whose counter belongs to the emulated APU.
3. **Sleep on the event.** First the MCPX counter follows time (in the
   loop, behind a switch) and `KeTickCount` is written by whoever wakes
   next. Then the worker waits with the safety-net timeout on a
   high-resolution timer instead of `Sleep(0)`, and after an announced
   flush spins until `PUT` moves, with a limit, then for a short, tunable
   time before it sleeps. `nv2a_pb_flip_hold_wait` wakes it when it
   announces a kickoff, only if it is asleep, and the timer thread when it
   releases a held flip. No trap and no second view. A switch keeps the old
   loop.
   Built behind `RECOMP_ACK_SLEEP=1` (7bc8309; `RECOMP_ACK_SPIN_US`,
   `RECOMP_ACK_NET_US`, `RECOMP_MCPX_COUNTER_HZ`); the timer thread's wake
   is in the same commit (below). MechAssault's menu, first boot: passes fell from 3.5 million a
   second to 160,000, no safety-net wake found work, and the first
   kickoff of each pair waits 4-16 µs for its flush (under 1 µs before),
   about 540 times a second.

   MechAssault in a level, its busiest spot, 2026-11-18 (stats off,
   `hostprof` 10 s, alternating):

   | Run | fps | Frame time median / 90th | Ack thread |
   |---|---|---|---|
   | off | 141.3 | 8.2 / 10.0 ms | 98% of a core |
   | on | 126.7 | 9.0 / 11.3 ms | 19% |
   | off | 133.2 | 8.1 / 10.2 ms | 97% |
   | on | 124.2 | 9.4 / 11.4 ms | 19% |

   The sleep costs MechAssault about 12% of its frame time: some 1 ms a
   frame, at about 15 woken kickoffs a frame some 65 µs each, far above
   the bench's 4-45 µs. As built, phase 3 does not hold for a title that
   spreads its kickoffs.

   What the cost is not, from more runs at the same spot:

   | Run | Median frame time | Ack thread |
   |---|---|---|
   | off, third run | 8.4 ms | 97% |
   | on, spin 5 ms (almost never sleeps) | 8.5 ms | 98% |
   | on, spin 32 µs, another process spinning a core | 8.8 ms | 18% |
   | on, spin 2 ms | 9.2 ms | 85% |

   - Not the wakes: with stats on in the level (2 ms spin), a kickoff woke
     the thread about 110 times a second and waited 4-16 µs for its
     flush, about 0.1% of the main thread. No safety-net wake found work.
   - Not the spin loop itself: at 5 ms the thread runs the new loop and
     the frame time is the baseline's.
   - Not the MCPX counter: MechAssault's belongs to the emulated APU.
   - Nothing waits on a pass: no mirror, `POKE`, frame tick or interrupt
     clear fired in the level.

   So the main thread runs slower whenever the ack thread's core idles,
   with nothing waiting on that thread: host power management or
   scheduling (Ryzen 7 5800X, Balanced plan). A process spinning another
   core brought back part of it. The 10-second profiles are too coarse to
   place 1 ms a frame.

   Paced at the 30 fps MechAssault asks for (`RECOMP_VBLANK=paced`,
   `RECOMP_FLIP_PACING=1`; present interval 2), same spot, alternating:

   | Run | fps | Frame time 90th / worst | Main thread | Ack thread |
   |---|---|---|---|---|
   | off | 30.0 | 39.9 / 58.1 ms | 27% | 19% |
   | on | 29.9 | 37.6 / 59.7 ms | 25% | 6% |
   | off | 29.9 | 37.9 / 60.7 ms | 25% | 18% |
   | on | 30.0 | 37.1 / 59.4 ms | 25% | 5% |

   No cost: the main thread's work per frame and the frame times are the
   same, and every flip was held on time. The hold's own sleep
   (`RECOMP_ACK_HOLD_SLEEP_US`) already brings the ack thread to about
   19% at 30 fps; phase 3 takes it to 5-6%. The 12% loss shows only
   uncapped, where the main thread never rests and anything that changes
   the host's clocks or scheduling for the busy cores reaches the frame
   time directly.

   Halo, cryo bay (a light spot, chosen for easy A/B), its own 30 fps:
   it never holds a flip, so the hold's sleep never helped its ack thread.
   Main-thread work is its CPU less the title's frame pacer
   (`sub_000BB060`, a spin):

   | Run | fps | Frame time median / 90th / worst | Main thread work | Ack thread |
   |---|---|---|---|---|
   | off | 29.9 | 33.7 / 41.1 / 64.9 ms | ~25% | 96% |
   | on | 30.0 | 33.2 / 42.0 / 60.8 ms | ~21% | 12% |
   | off | 30.0 | 33.2 / 41.8 / 68.3 ms | ~25% | 96% |
   | on | 30.0 | 33.2 / 42.0 / 69.7 ms | ~23% | 12% |

   The ack thread frees 84% of a core at no cost to the frame. A heavier
   spot is still to check.

   DOAX, Burnout 3 and Splinter Cell, a busy spot each, paced (DOAX and
   Burnout 3 by their own defaults at 60 fps; Splinter Cell with
   `RECOMP_VBLANK=paced RECOMP_FLIP_PACING=1`, 30 fps), two alternating
   pairs each (ack thread from `hostprof`):

   | Title | Run | fps | Frame time median / 90th / worst | Main thread | Ack thread |
   |---|---|---|---|---|---|
   | DOAX | off | 60.0 | 15.5 / 24.8 / 30.9 ms | 25% | 23% |
   | | on | 60.0 | 15.5 / 22.5 / 29.6 ms | 23% | 7% |
   | | off | 60.0 | 15.9 / 23.5 / 30.6 ms | 24% | 22% |
   | | on | 60.0 | 15.7 / 22.5 / 29.0 ms | 24% | 7% |
   | Burnout 3 | off | 60.0 | 16.6 / 23.4 / 30.3 ms | 54% | 23% |
   | | on | 60.0 | 16.1 / 22.7 / 34.1 ms | 52% | 7% |
   | | off | 60.1 | 14.7 / 23.8 / 32.8 ms | 55% | 25% |
   | | on | 60.0 | 15.3 / 22.9 / 35.1 ms | 55% | 8% |
   | Splinter Cell | off | 30.0 | 32.9 / 41.2 / 62.7 ms | 49% | 45% |
   | | on | 30.0 | 33.2 / 37.9 / 59.1 ms | 46% | 5% |
   | | off | 30.0 | 33.4 / 39.9 / 60.6 ms | 50% | 44% |
   | | on | 30.1 | 33.2 / 39.1 / 60.6 ms | 47% | 4% |

   No cost in any of them: the same frame rates, medians, 90th
   percentiles and main-thread work. The ack thread falls to 4-8%.
   Burnout 3's worst frame was 34-35 ms in both sleep-on runs against
   30-33 ms off: one frame in 600 each time, within what the off runs
   vary by, but in the same direction twice, so it stays open. All three
   use the emulated APU, so phase 1's faster passes do not move their
   sample counter.

   The timer thread's wake (7bc8309): under `RECOMP_ACK_SLEEP` a held flip
   sleeps on the worker's event, with the safety net's timeout, and the
   timer thread signals it after each vblank it counts while a flip is
   held (`xbox_Nv2aAckVblank`). `RECOMP_ACK_HOLD_WAKE=0` keeps the fixed
   sleep. Headless, DOAX's holds averaged 15.9 ms, under a vblank: no
   release waits for the timeout. DOAX at the same spot, 2026-11-18
   (`hostprof` 10 s, alternating):

   | Run | fps | Frame time median / 90th / worst | Ack thread |
   |---|---|---|---|
   | fixed sleep | 59.9 | 15.6 / 23.7 / 31.4 ms | 6% |
   | vblank wake | 60.1 | 15.5 / 23.9 / 33.4 ms | 7% |
   | fixed sleep | 60.0 | 16.0 / 23.8 / 33.5 ms | 7% |
   | vblank wake | 60.0 | 15.5 / 23.0 / 29.9 ms | 8% |

   No difference: a release found one 500 µs sleep late changed nothing a
   frame shows. The wake stays, as the design's shape (the release is an
   event, not a poll), at no cost.

   **Where phase 3 stands.** Built and measured in all five titles, paced,
   two A/B pairs each: no cost. Opted in per title (Halo, MechAssault,
   DOAX, Burnout 3, Splinter Cell), paced or not: an uncapped run that
   wants MechAssault's last 12% sets `RECOMP_ACK_SLEEP=0`. Open:
   - Halo at a busy spot (the cryo bay is light);
   - Burnout 3's worst frame, 2-3 ms higher in both sleep-on runs;
   - live only once each title's `TOOLKIT_COMMIT` carries this branch.
4. **Count what the safety net found**, by job. A title whose flushes come
   unannounced (a renderer that does not hook `KickOff`) goes back to the
   polling loop on its own once the count passes a threshold, and says so
   once.
   The count is in (`found work` in `RECOMP_ACK_STATS`) and stayed at 0 in
   every run. The fallback is not built: a title opts in only after its
   A/B check, so it is not needed while that holds.
5. **Clocks to the clock thread**, after the clock measurements.
   `KeTickCount`, MCPX and frame counters move off the loop; the counters
   already follow time since phase 3.
   Dropped for now: under `RECOMP_ACK_SLEEP` the MCPX counter follows time
   in the loop, `KeTickCount` and the frame counters move at least every
   safety-net period (1 ms), and nothing waited on a pass in any run.
6. **Fold `kick_ack_spin` into the worker's spin.** The announcement stays:
   it is now what wakes the worker. Done (60a6d34): the loop itself spins
   on an announced flush, skipping the rest of the pass, with the same
   limit.

Built only if a title needs them:

- **A flush trap**, with the store emulated through a second view of the
  aperture, for a title the renderer cannot hook and that is too slow on the
  polling loop. One trap a kickoff, about 1.5 µs.
- **Interrupt status traps**, if a title hangs or overflows its stack with
  the safety net at 2 ms. PGRAPH keeps the trap it has under
  `RECOMP_PGRAPH_TRAP`. The pages' write counts come first: only the PFB
  and PFIFO USER pages have been counted.

Neither has been needed: the renderer's call announces every kickoff in
all five titles, and no interrupt status register was cleared after boot.

The spin is the design's one real setting: the ack thread's CPU against a
few percent of the main thread in a title that spreads its kickoffs over the
frame. In Halo the thread should fall to about the burst's share of the
frame, not to nothing. Measured: 32 µs covers Halo's bursts (96% -> 12%);
MechAssault's spread kickoffs cost nothing paced, and its uncapped loss
came from the thread being asleep at all, not from the spin's length.

Three faults in the instrument, found in review and fixed (2026-11-18);
none changes a number above:

- The `kick` histogram now and then recorded one sample of "4-8 s". That
  is the last bucket, which has no upper bound (now printed `>=`). The
  sample was taken after the flush was cleared, when the title may already
  have stamped its next kickoff: the difference went negative and wrapped,
  and the new stamp was zeroed, losing that kickoff's sample. The stamp is
  now read before the clear.
- A mirror's change counted as work for the sleep only with the stats on.
  No mirror fired in any measured run.
- "Woken by kickoff" counted a sleep that ended at once on a `SetEvent`
  left over from the sleep before. The thread now takes that wake before
  it goes on.

## Risks

- **Wake cost.** A kickoff that finds the worker asleep waits for it to
  wake, on the title's thread. The spin hides that inside a burst; a title
  with even gaps longer than the spin pays it at every kickoff. The gap
  histogram and MechAssault's A/B check measure it.
- **Trap cost.** For the fallback traps only. One trapped write is 5.2 µs
  single-stepped and 1.5 µs emulated, measured with one handler; the games
  register several. It rules out any page written thousands of times a
  second (Halo kicks off 7,500 times a second).
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
