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

- acknowledges busy bits D3D set and waits on (the PFB flush every kickoff
  sets) and keeps the idle bits set;
- polls a held flip (`nv2a_pb_exec_flip_poll`);
- ticks the fence, DSP, counter and frame-counter mirrors and the `POKE`
  probes the project registered;
- when `DMA_PUT` moved, walks the new segment (fences, semaphores, flips,
  software-method traps; see [Pushbuffer Executor](pushbuffer-executor.md))
  and then sets `DMA_GET` to `PUT`;
- adds 1 to each MCPX counter (the APU sample counter);
- writes `KeTickCount` from `GetTickCount()`.

Then it ends with `Sleep(0)`. That gives way to another ready thread, and on a
machine with idle cores there is none, so it returns at once and the loop runs
tens of thousands of passes a second, nearly all of them finding nothing to
do. Halo's cryo bay profiled the thread at 97% of a core: 56% in
`ZwDelayExecution` (the `Sleep(0)`) and 25% reading the clock
(`xbox_VblankAckHeld` → `kernel_clock_ms`).

The MCPX counters show the cost is not only CPU: they go up once per pass, so
their rate is the loop's speed, not time.

## How It Got Here

- **0483f1a** (upstream, 2026-07-20) created the thread to answer D3D's
  set-a-bit-then-wait handshakes (Halo hung in the pushbuffer kick). The
  `Sleep(0)` is commented "yield; the waiter is spinning on another core":
  it was written for latency, and CPU cost was not the question.
- **76f9a7d** (2026-11-14) sleeps `RECOMP_ACK_HOLD_SLEEP_US` (500 µs) per pass
  while a flip is held for its vblank. Burnout 3's ack thread went from 93% to
  32% of a core at the same frame rate. Outside a hold it still only yields.
- **53c47c8** (2026-11-15): a renderer hook in front of D3D's `KickOff`
  (`nv2a_pb_flip_hold_wait`) announces each kickoff, and the thread spins on it
  (`kick_ack_spin`, `RECOMP_KICK_SPIN_US`) to clear the flush the moment it is
  set. MechAssault went from 80 to 91 fps.

A title that paces itself and never holds a flip (Halo) never reaches the
sleep.

## Design: React Instead of Poll

A GPU does not poll. It reacts to the CPU's register writes, and its clocks
run on their own. The jobs above have three kinds of trigger, and each can be
met the way the hardware meets it.

| Job | Trigger | Handled by |
|---|---|---|
| PFB flush bit, `DMA_PUT` moved | the title writes a GPU register | the write itself |
| walk, `DMA_GET`, fences, semaphores, flips | a `PUT` write | a worker woken by it |
| `KeTickCount`, MCPX counters, frame counters, flip release | time | the timer thread, on deadlines |
| idle bits | nothing | set once |
| fence, DSP and counter mirrors, `POKE` | the title writes its own RAM | each walk, and the safety net |

### Register writes

The toolkit already traps writes to register pages: the page is read-only,
and a vectored exception handler sees the write and acts on it (PGRAPH's
write-1-to-clear and software-method trap, AC97, the APU). The same mechanism
covers the page holding the PFB flush and `DMA_PUT`:

- a write setting the flush bit is completed in the handler, so D3D's wait
  loop finds it clear on its first read. This replaces `kick_ack_spin` and
  the kickoff announcement, and needs no D3D hook, so it holds for any title
  and XDK;
- a `PUT` write signals an event. The worker sleeps on it, walks the segment
  and advances `GET`. The walk stays off the title's thread, as on the
  console, so the title is never re-entered from its own register write.

### Time

The kernel timer thread (`kernel_timer_thread`) already sleeps until its next
deadline (vblank, due timers, DPCs). It takes over `KeTickCount` (each 1 ms),
the MCPX counters (derived from the clock, not counted), and the release of a
held flip at its vblank, which the worker then waits for with a timeout
instead of polling.

### Safety net

The worker also wakes every few milliseconds and runs one full pass, as
today. It counts the passes that found work no write announced and prints the
count once, at exit: nothing is logged while the title runs. A non-zero count
names a handshake the traps miss.

When nothing happens the thread sleeps, and a busy-bit handshake is answered
sooner than today: during the write, not on the next pass.

## Phases

Each phase is its own commit with its own A/B check (switch off and on, in
alternating pairs on a quiet machine: frame times and per-thread CPU).

1. **Clocks to the timer thread.** `KeTickCount`, MCPX and frame counters move
   off the loop; the counters follow time. The timer thread starts only at the
   first `KeSetTimer` today, so it has to start unconditionally.
2. **Sleep on an event.** The worker waits on a wake event with the safety-net
   timeout instead of `Sleep(0)`. The kickoff announcement signals it. A switch
   keeps the old loop.
3. **Trap the writes.** Flush and `PUT` writes are caught on their page;
   `kick_ack_spin` and the announcement go.

## Risks

- **Trap cost.** Each trapped write is an exception, a few microseconds. That
  is fine for `PUT` (about 30 a frame in MechAssault), not for a page with a
  register written thousands of times a frame. Count the writes per page
  before trapping one.
- **Writes slipping past.** The current handlers make the page writable and
  single-step the write. Another thread writing in that window is not seen.
  Emulating the store in the handler closes it; until then the safety net
  catches it, a few milliseconds late.
- **Handler interplay.** Several vectored handlers already exist (watch, AC97,
  APU, PGRAPH); their order and re-entry need care.
- **Instant completion.** A real flush takes time. Completing it at once is a
  legal GPU timing, but it can expose a race in a title that never lost it
  before.
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
