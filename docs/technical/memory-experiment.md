# Memory model 2: one owner per physical number

Design document for memory model 2, the default on Windows hosts since
2026-11-20 (`RECOMP_MEM_MODEL=1` is the classic model, `=2` model 2). It was first developed on a `memory-experiment` branch (since deleted)
as a rewrite of the default; the `mem2` branch carries the same design
behind that switch, in `src/kernel/xbox_mem2.c` (section 7 says how the
branch is laid out). It starts with the
background the design rests on (how the console's memory works, what a title
relies on, what the host gives us), then the problem, the design, its status
and what is still open.

**Status on `mem2`:** unset, a title gets the classic model unchanged. With
the switch, everything below applies. The phase history in sections 8 to 11
is that first branch's.

**Status of the design:** step 1 (freed memory is always given back), the `[MEM]` report,
and phases 1 and 2 are done.
Phase 3 is done in the lighter of the two shapes described
below: every kind of guest memory has physical numbers of its own and the
device models share one resolver. `RECOMP_CONTIG_AVOID_IMAGE` is still
there, as a placement choice for bottom-up filling only.

## Contents

1. [The console's memory](#1-the-consoles-memory)
2. [What a title sees and relies on](#2-what-a-title-sees-and-relies-on)
3. [The host and the toolkit](#3-the-host-and-the-toolkit)
4. [The problem](#4-the-problem)
5. [What was measured](#5-what-was-measured)
6. [Options](#6-options)
7. [The design as built](#7-the-design-as-built)
8. [Phases](#8-phases)
9. [Phase 3](#9-phase-3)
10. [Differences from a console](#10-differences-from-a-console)
11. [Found while doing phases 1 and 2](#11-found-while-doing-phases-1-and-2)
12. [Risks](#12-risks)
13. [Not decided](#13-not-decided)

## 1. The console's memory

This section describes the console from the toolkit's code and notes and
general knowledge of its kernel. It has not been checked against a hardware
reference.

### Physical RAM

The console has 64 MB of memory chips. This is the only real storage: every
byte a title, the kernel or the hardware holds is in it. The bytes are
numbered from 0; that number is the **physical address**
(`0x00000000` to `0x03FFFFFF`).

RAM is handed out in **pages** of 4 KB, 16,384 of them.

- A physical address names one fixed place. There is one pool, and a page
  in it has one owner at a time.
- The hardware (graphics, sound, USB) speaks physical addresses. When a
  title tells a chip where its data is, it gives a physical address.

### Virtual addresses

The processor is 32-bit, so a pointer can name any address from 0 to
`0xFFFFFFFF`: 4 GB, 64 times the RAM that exists. These are **virtual
addresses**, and they are the only kind a title's code uses.

A virtual address is a label. It holds something only once the kernel has
attached a physical page to it; touching any other address is a fault. At
most 64 MB of the 4 GB can be attached at once.

```
virtual (4 GB of labels)             physical (64 MB of real bytes)

+------------+
|  empty     |
+------------+                        +----------+
|  attached  | ---------------------> |  page    |
+------------+                        +----------+
|  empty     |
+------------+                        +----------+
|  attached  | ---------------------> |  page    |
+------------+                        +----------+
|  empty     |
+------------+
```

- The two numbers are unrelated: virtual `0x00300000` can sit on physical
  `0x01C45000`.
- Neighbouring virtual pages can sit on physical pages scattered across RAM.
- One physical page can be attached at two virtual addresses. Both show the
  same bytes.

### How the kernel connects them

The kernel keeps the **page table** (which virtual page is attached to which
physical page) and the record of which physical pages are free. It attaches
memory in two ways.

**The window (upper half).** At boot the whole of RAM is attached in order
from 2 GB:

```
virtual 0x80000000 + P   --->   physical P
```

The rule is fixed. A request for **contiguous memory** finds a run of
adjacent free physical pages and returns their window address. It exists for
hardware: a chip is given a start and a length and walks straight along RAM.

**Virtual memory (lower half).** The image, the stacks and the heap are
attached from whatever physical pages are free, at an address the kernel
chooses (on 64 KB boundaries). Only the processor uses this memory, through
the page table, so scattered pages are fine.

```
virtual                                  physical RAM (64 MB)
0x00000000 +------------------+
           | image            |---.      +--------------------+
           | stacks, heap     |----\---->| pages, scattered   |
           | (mostly empty)   |     \    |                    |
0x80000000 +------------------+      `-->|                    |
           | window: all RAM  |=========>| every page, in     |
0x84000000 +------------------+          | order              |
           | (empty)          |          +--------------------+
0xFD000000 | chip registers   |   (not RAM: the chips themselves)
0xFFFFFFFF +------------------+
```

### Ownership and access are different things

- **Ownership:** a physical page is free, or part of one contiguous block,
  or under one piece of virtual memory. The kernel never gives a page to two
  requests.
- **Access:** a page with its one owner can be reached at more than one
  virtual address. A heap page is at its lower-half address and also in the
  window at 2 GB plus its physical number. That is one allocation with two
  doors, not two allocations.

### Physical address of a pointer

`MmGetPhysicalAddress` takes a virtual address and returns the physical
address behind it: for a window address, the address minus 2 GB; for a
lower-half address, a page table lookup. The answer is below 64 MB and names
one page. A buffer in the lower half can span scattered pages, so a library
asks once per page and gives the chip a list.

## 2. What a title sees and relies on

A title's code holds only virtual addresses. Its graphics, sound and input
libraries are linked into it, so "the title" includes them.

| Memory | From | Virtual address |
|---|---|---|
| Image (code, globals) | the kernel's loader | fixed, from `0x00010000` |
| Stacks | kernel, one per thread | lower half |
| Heap | the title's C library, which asks the kernel for chunks | lower half |
| Contiguous | the kernel, mostly on behalf of the libraries | window |

### The image in a recompiled title

The image is the original XBE. The toolkit copies its sections into guest
memory at their linked addresses. The translated code is in the host
executable, outside guest memory, and costs nothing from the 64 MB.

- Globals and constants in the loaded XBE are the live data.
- The original code bytes are never executed. They stay loaded because
  titles read tables and constants stored among their code.
- A function pointer is still a guest address. A call through one looks the
  address up in a table written at regeneration (guest address to translated
  function). Seeds are the input that tells the recompiler where functions
  start; an address missing from them has no entry.

### What a title relies on

Compiled in, and not ours to change:

- **Image addresses are fixed.**
- **The window rule.** Libraries compute `0x80000000 + physical` themselves.
- **Physical addresses fit in 28 bits.** Libraries mask them
  (`& 0x0FFFFFFF`). On a console this is harmless, since real addresses stop
  at 64 MB. The reason is unconfirmed; it may be a quick way to turn a
  window pointer into a physical address.
- **64 MB in total.** Some titles take fixed pools at boot sized for it;
  others ask the kernel how much is free.
- **Sometimes one specific physical address.** Halo asks for a block at
  physical `0x61000`.
- **Hardware may be pointed at ordinary memory.** A sound buffer or USB
  packet can be in the heap or a global. The chip then receives the physical
  number of a heap or image page.

### What a title does not rely on

- Which physical pages are under its image, stacks and heap. It never
  chooses them, and only forwards the numbers to a chip.
- Where its heap lands, in general. (A title could still pack pointers in a
  way that assumes low addresses.)

The experiment uses exactly these two freedoms.

### Where a heap allocation lands

Two layers decide it. The title's C library places a small allocation inside
a chunk it already holds; the kernel decides where each chunk goes. The
toolkit's heap fills from the bottom of its range upward on 64 KB
boundaries, reusing the lowest freed piece that fits, so addresses are the
same on every run.

## 3. The host and the toolkit

### The host

The host process is 64-bit. The console's 4 GB of virtual addresses is one
stretch inside it, and the mapping is one addition:

```
host address = base + guest virtual address
```

There is no page table on our side. That is what makes translated code fast,
and it is the source of every limit below: the guest address alone decides
where the bytes are.

The host gives three tools:

- **Reserve:** a range of addresses with nothing behind it. Free; a touch
  faults.
- **Commit:** usable memory behind a range. It costs host memory only once
  written.
- **A shared section with views:** storage that can be shown at several
  addresses. This is the host's "one page, two doors". Views sit on 64 KB
  boundaries.

The host's own paging, underneath, plays no part in the design.

### The toolkit as kernel

Without a page table the toolkit cannot attach "any free physical page"
anywhere. Each kind of memory gets a fixed stretch of guest addresses with
its own host storage.

| The title asks for | Console | Toolkit |
|---|---|---|
| Heap chunk | free pages, attached in the lower half | a piece of the arena |
| Contiguous block | adjacent free pages, as a window address | a piece of the window's storage |
| Thread stack | pages from the pool | a piece of the arena |
| Physical address | page table lookup | window address minus 2 GB; else the address itself |
| Free | pages return to the pool | the piece returns to its own store |

### The physical number is a ticket

Under a toolkit heap page there is no physical page, only host memory at
`base + address`. A physical address is a number the toolkit's kernel side
issues when asked, the title forwards untouched, and the toolkit's device
models redeem. It has to be unambiguous when redeemed, and it has to survive
the 28-bit mask.

## 4. The problem

Before this branch, guest RAM below 64 MB (image, toolkit region, stack,
heap) and the contiguous window were separate host storage, and both issued
the same physical numbers: `MmGetPhysicalAddress` is the identity for a heap
or image address and the window's offset for a window address. A device
given a number had to guess which copy the title meant, and each device
model guessed in its own order:

| Resolver | Order |
|---|---|
| `mcpx_apu_phys` (APU) | above 64 MB is RAM; named heap page; page the window handed out; image; below the window's high-water mark; RAM |
| `bus_resolve` (OHCI) | image; named heap block; below the high-water mark; RAM |
| `dma_resolve` (NV2A) | below the high-water mark; else the offset as an address |

The guess rested on bookkeeping in the heap (a record of pages a title had
named) and on `RECOMP_CONTIG_AVOID_IMAGE`. A slip surfaced as a dead pad, a
hiss or a buzz, with no error.

Two more things were wrong around it:

- Freed memory was given back only under `RECOMP_HEAP_RECLAIM`.
- RAM was mapped again 28 times up to 2 GB (the mirrors), so a stray
  pointer read plausible data instead of faulting.

## 5. What was measured

Four titles on this branch, 60 s headless to their intro screens, in MiB of
64:

| Title | Image | Heap committed | Contiguous | Sum |
|---|---|---|---|---|
| Splinter Cell | 3.7 | 8.4 | 8.8 | 20.9 |
| MechAssault | 5.3 | 37.0 | 5.3 | 47.6 |
| DOAX | 15.0 | 3.9 | 44.5 | 63.4 |
| Burnout 3 | 7.5 | 17.1 | 38.8 | 63.4 |

The `[MEM]` line on stderr reports this: image sections, heap committed and
contiguous in use, against 64 MB. It leaves out reserved address space and
the toolkit's own 8 MB main stack, and prints each time the peak grows by
1 MB, so the last line in a log is the peak.

- DOAX and Burnout 3 take fixed pools at boot and sit at 99% of 64 MB.
  Burnout 3's figure did not move through a race or 17 minutes of play.
- MechAssault's contiguous use reaches 24.8 MB in a level in older logs.
  Levels have not been measured on this branch.
- **Mirrors.** With every mirror unmapped all four reach their intro
  screens. On a build with the mirrors off and the heap still in RAM (so
  every address from 64 MB to 2 GB faults), Burnout 3, the title the mirrors
  were added for, was played for 17.6 minutes over several track and vehicle
  loads with no fault.
- Outside the toolkit nothing depends on where the heap is. The renderer
  reaches GPU memory only through the window.

## 6. Options

**A. Full simulation: one 64 MB pool for everything.** Image, heap, stacks
and contiguous memory share one section, as on the console; physical numbers
are genuine and below 64 MB, and the limit is enforced. Not chosen. Without
a page table, heap memory would be views of the section placed on 64 KB
boundaries where the console attaches 4 KB at a time, each attach is a host
call, the toolkit's own stack has to live elsewhere, and DOAX and Burnout 3
have under 1 MB of slack to absorb the rounding. Build it only if a title
needs every physical number below 64 MB.

**B. Virtual memory moves above RAM.** Chosen, and built in phase 2. The
heap, the toolkit's region and all stacks get their own storage at guest
addresses from 64 MB to 256 MB. Their physical numbers are their addresses,
which no window number can equal.

That leaves the image against the window, and two ways to finish (section
9): make the window a view of RAM so the two share one 64 MB, or keep them
apart and give the image numbers of its own.

## 7. The design as built

### The switch and the branch layout

`RECOMP_MEM_MODEL=2` turns the model on and `1` is the classic model, byte
for byte what the toolkit did before. Unset or empty, the model is on on
Windows hosts, where the five titles were played with it, and off elsewhere,
where it has not been run. A port can still set either in its own `main.c`
before the layout is initialised, like its other defaults; the toolkit's
tests ask for the classic model by name where they check it.

The `mem2` branch is laid out to rebase onto upstream with little friction:

| Commits | What | Conflicts when upstream edits the classic files |
|---|---|---|
| `[MEM]` report | Right in both models; an upstream candidate as it stands | Possible, small |
| `xbox_mem2.c`, `xbox_mem2.h` | The whole model: placing the arena, the image's second view, both translations between addresses and physical numbers | None: upstream does not have the files |
| The hooks | About 25 places in the classic files, each one to five lines marked `mem2` or calling `xbox_mem2_*`, of the form "if the model is on, ask it; otherwise carry on" | Possible, each trivial |

Every `xbox_mem2_*` entry point answers "no" while the model is off, so a
hook falls through to the code that was there. With the model on:

- freed memory is given back, as under `RECOMP_HEAP_RECLAIM`;
- the RAM mirrors are off unless `RECOMP_MIRRORS=1`;
- the contiguous window is filled from the top unless
  `RECOMP_CONTIG_TOP_DOWN=0` (see "The window fills from the top" below);
- the classic machinery for guessing an owner (the record of heap pages a
  title named, the window's high-water mark, the three resolvers' own rules)
  is still compiled and is never reached.

The rest of this section describes the model when it is on. Function names
are the first branch's; on `mem2` the same rules are
`xbox_mem2_guest_to_phys`, `xbox_mem2_phys_to_guest` and
`xbox_mem2_arena_has`.

### Address map (64 MB layout)

| Guest range | Before | Now |
|---|---|---|
| `0x00000000` to `0x04000000` | image, toolkit region, main stack, heap | image only |
| `0x04000000` to `0x10000000` | mirrors 1 to 3 | arena: toolkit region at `0x04000000`, main stack at `0x04080000` (8 MB), heap from `0x04880000`; the top few MB (the image's size, rounded up to 1 MB) are RAM a second time, at the image's physical numbers |
| `0x10000000` to `0x80000000` | mirrors 4 to 28 | unmapped; held no-access so a touch faults (`RECOMP_EXT_VMA` reservations as before) |
| `0x80000000` to `0x84000000` | window, own storage | the same |
| `0xF0000000` | tiled view of the window's storage | the same |

```
0x00000000 +---------------------+
           | image               |  "RAM" section, 64 MB
0x04000000 +---------------------+
           | toolkit region      |
           | main stack          |  arena, 192 MB
           | heap                |
           | image, second view  |  its physical numbers
0x10000000 +---------------------+  256 MB = 2^28
           | nothing (faults)    |
0x80000000 +---------------------+
           | contiguous memory   |  window section, 64 MB
0x84000000 +---------------------+
           | nothing             |
0xF0000000 | tiled view of the window
0xFD000000 | chip registers
```

The arena stops at 256 MB so that every address in it survives the 28-bit
mask: a heap buffer at `0x0505A500` reaches the chip unchanged, one at
`0x1505A500` would arrive as `0x0505A500`.

### Rules

1. **Freed memory is always given back.** Heap pieces split and merge;
   `MmFreeContiguousMemory` releases. There is no switch.
2. **Each kind of memory has physical numbers of its own.**

   | Guest memory | Physical number |
   |---|---|
   | Contiguous window, `0x80000000 + P` | `P`, below 64 MB |
   | Heap and stacks (the arena) | the address itself |
   | RAM from 0 to the end of the image | `g_xbox_image_phys` + address |

   The image's numbers are the top of the arena's address range and a view
   of RAM is mapped there, so a number at or above 64 MB is always the guest
   address of the same value. The same view is mapped at `0x80000000` + the
   number, where a title following the window rule would look.
   `xbox_GuestToPhys` is what `MmGetPhysicalAddress` answers;
   `xbox_PhysToGuest` is the one resolver the APU, OHCI and NV2A models use.
3. **One bound check.** `xbox_GuestMapped(va, bytes)` is true for RAM, the
   arena and the window. It replaces range checks that meant "below RAM
   size".
4. **The mirrors are off.** `RECOMP_MIRRORS=1` maps them, above the arena.
5. **`xbox_SetMapSize` raises the arena's top**, not the size of RAM.

### The window fills from the top

With the model on, a contiguous request gets the highest run of free pages
that fits its range and alignment; the classic model takes the lowest. The
toolkit's own notes say the console's kernel fills from the top (no hardware
reference has been checked for this), and titles are written against that:

- A request that must be below 16 MB (D3D's visibility report pages) finds
  low memory free without anything being kept back for it. The megabyte at
  15 MB is still kept, for a window that has filled all the way down.
- A title that places blocks itself at low addresses finds them free. Halo
  pins 26 MB from `0x61000` up; bottom-up, ordinary blocks were in the way
  unless `RECOMP_CONTIG_AVOID_IMAGE=1` happened to move them.
- Ordinary blocks reach the image's address range last, so
  `RECOMP_CONTIG_AVOID_IMAGE` has nothing left to do and is ignored. It
  still applies bottom-up (the classic model, and `RECOMP_CONTIG_TOP_DOWN=0`).

The `[CONTIG]` line reports "filled down to" in place of the high-water
mark. `RECOMP_CONTIG_TOP_DOWN=0` puts the model back on bottom-up, to tell a
placement problem from a model problem.

Found by it: the pushbuffer scan reads up to 4 MB from where it starts
without knowing where the pushbuffer ends, and with a pushbuffer near the
top of the window it read past the window's end (Halo, at its first frame).
The scan now stops there. Bottom-up, the same over-read stayed inside mapped
memory and was never seen.

Verified by 60 s headless boots of the five titles: top-down, top-down with
`RECOMP_CONTIG_AVOID_IMAGE=0`, and bottom-up. Same `[MEM]` peaks in all
three, no refused request, the DSP doorbell found at its new page in each.
Played with the model on: Halo, Splinter Cell, DOAX, MechAssault and
Burnout 3 into gameplay, each with the switch as its port sets it and with
`RECOMP_CONTIG_AVOID_IMAGE=0`, before the switch was ignored.

### What went away (on the first branch; bypassed, not deleted, on `mem2`)

`RECOMP_HEAP_RECLAIM`; the record of heap pages a title named
(`g_phys_ram_named`, the block table's `named` flag, `xbox_PhysNoteRamName`,
`xbox_PhysRamNamedAt`, `xbox_PhysRamNameMap`, `heap_forget_ram_names`,
`heap_block_at`); the named-page branches in the three resolvers. With
phase 3: the three resolvers' own rules, the window's high-water mark as an
address test and `xbox_ContiguousAllocatedBytes`.

### One case still guessed, and logged

A number below 64 MB that is inside the image's address range, on a window
page that was never handed out, cannot have come from `xbox_GuestToPhys`:
the title used an image address as its own physical number without asking
the kernel. `xbox_PhysToGuest` reads it as the image and prints a `[PHYS]`
line once. None of the four titles prints it at boot. If no title ever does,
the case can go.

## 8. Phases

Each is one or two commits and is checked the same way: the toolkit's memory
tests, the four titles launched headless with the `[MEM]` line and a shown
frame, then a played level.

| | Phase | State |
|---|---|---|
| Step 1 | Reclaim is the only path | Done |
| | `[MEM]` report | Done |
| 1 | Mirrors off by default | Done |
| 2 | Virtual memory above RAM; named-page record deleted | Done |
| 3 | One meaning for a physical number below 64 MB (shape 2) | Done |
| | The default on Windows hosts | Done |
| 4 | Later, separately: real reserve and commit for `NtAllocateVirtualMemory` (extending `guest_vmem` down to the arena); thread stacks at the size asked for; `MmQueryStatistics` from the toolkit's own counts (done: RAM less `[MEM]`'s sum); the block table's fixed size | Started |

### Verified

- `tests/memory_regressions` (five modes) and `tests/apu_phys` pass on
  Windows.
- The four titles rebuild from scratch and run 60 s
  headless to their intro screens with unchanged `[MEM]` figures.
- Splinter Cell takes scripted pad input and plays a voice from a heap
  buffer at `0x0505A500`: a heap physical number made the round trip to the
  sound chip.
- Burnout 3 ran a Single Event race for 220 s with no crash (before
  phase 3).
- After phase 3: the same tests pass, with `tests/apu_phys` checking that an
  image address and a window block pinned at the same number read different
  bytes. The four titles reach their intro screens with unchanged `[MEM]`
  figures and no `[PHYS]` line. The pad enumerates and serves reports in
  Burnout 3 (whose driver reads a descriptor into its image) and Splinter
  Cell. Splinter Cell, DOAX and MechAssault also boot with
  `RECOMP_CONTIG_AVOID_IMAGE=0`, their blocks then sharing numbers with the
  image's address range.
- Halo with `nv2a-ack-thread` merged in (2026-11-19): 20 s headless in
  models 1 and 2, ack sleep off and on, with the same `[MEM]` peak
  (66,465 KB) and GPU work in all four, then played in both models (one
  DirectSound crash, an APU bug fixed on `integration`). Its fence
  word lands at the top of the contiguous window under model 2, not in the
  arena, so the arena path in `fence_readable` is still untested. (Details
  in `nv2a-ack-thread.md`, "With memory model 2".)
- MechAssault's second mission with `MmQueryStatistics` from `[MEM]`
  (2026-11-20, before v0.18): buildings destroyed and rain, where the
  classic model ran out of heap. Peak 54,515 KB.
- Rebased onto upstream v0.18.0 (2026-11-20), the five
  titles regenerated, rebuilt and played by hand with the model on, on the
  Vulkan renderer, vblank pacing on:
  - MechAssault: three missions, each end a save and the title's own
    restart into the next. Peak 55,410 KB.
  - Splinter Cell: from the menus to the start of the Tbilisi level, three
    restarts of its own. Peak 55,422 KB.
  - DOAX: movies, the island and attract mode, 60 fps. Peak 64,942 KB.
  - Halo: played in a level at its own 30 fps. Peak 66,154 KB: over 64 MB,
    probably its pin at `0x61000` counted both as the image and as
    contiguous memory (unchecked).
  - Burnout 3: races with two level loads, 60 fps. Peak 64,894 KB.

  All five: sound by ear with no underrun, no `[PHYS]` line.

### Not verified

POSIX hosts; titles that call `xbox_SetMapSize`; the arena path in
`fence_readable`; a texture or vertex buffer in heap memory under the Vulkan
renderer (none of the five showed one).

## 9. Phase 3

What was left after phase 2: an image address and a contiguous block could
carry the same physical number, and the resolvers told them apart with the
window's high-water mark and `RECOMP_CONTIG_AVOID_IMAGE`. Shape 2 is built
(section 7, rule 2); shape 1 is kept here for the record.

### Shape 1: the window becomes a view of RAM

One 64 MB section. The window shows all of it, a page map owns it and marks
the image's pages as taken, and a pinned request is refused when a page is
owned.

It cannot keep the image at physical = virtual. Halo pins 3.4 MB at physical
`0x61000`, inside its image's address range. On a console that is no
conflict, because the loader put the image on whatever pages it took. So the
image has to be loaded at a place of the toolkit's choosing in RAM and that
place mapped at `0x00010000`.

### Shape 2: separate storage, the image gets numbers of its own

Recommended. Storage stays as it is. `MmGetPhysicalAddress` on an image
address returns the address plus a fixed offset into a range no other memory
uses, and the resolver turns it back. This is what phase 2 did for the heap.
Host memory is no obstacle either way: untouched pages cost nothing.

| | Shape 1 | Shape 2 |
|---|---|---|
| Guessing removed | yes | yes |
| Halo's pin at `0x61000` | needs the image relocated | works as today |
| Contiguous blocks move (Burnout 3 once crashed in a race when they did) | yes | no |
| Size of change | large | small |
| Image and contiguous limited to 64 MB together | yes | no |

Shape 2 gives up the last row. Phase 2 already gave it up for the heap, and
a title that ran on a console does not ask for more than the console had.

Still open after shape 2:

- **`RECOMP_CONTIG_AVOID_IMAGE` cannot simply be deleted.** It no longer
  decides which bytes a number names, but it has a second effect: it keeps
  ordinary blocks off the lowest few MB, which leaves them for requests
  that must sit below 16 MB. Halo needs that. It pins blocks from
  `0x61000` to 26 MB, which covers the 1 MB reserve at 15 MB, so only 384 KB
  below 16 MB is left; with the switch off, ordinary small blocks take it
  and every below-16 MB request fails (`[CONTIG] arena exhausted`). Removal
  was tried and undone for that. The console avoids this by filling
  contiguous memory from the top, and with the model on the toolkit now
  does too (section 7): Halo boots with the switch off, and top-down
  filling ignores it. The switch stays for bottom-up filling.
- **A pinned request that lands on memory already handed out** is zeroed
  before ownership is checked. It should be refused.

## 10. Differences from a console

| Console | Toolkit on this branch |
|---|---|
| The window shows all of RAM, including pages under the heap and image | The window shows only contiguous memory |
| One 64 MB pool; running out is a real limit | Image 64 MB, arena 192 MB, window 64 MB; only contiguous memory is held to 64 MB |
| Physical numbers are always below 64 MB | Heap and stack numbers are 64 MB to 256 MB |
| A physical number names one page | A physical number names one kind of memory and one place in it |
| Heap chunks may sit on scattered physical pages | A heap chunk is one run of host memory |
| Reserve and commit are separate | Reserving also commits |
| The kernel tends to hand out low virtual addresses | The heap starts at 72 MB |

The trade against the old layout:

| | Old layout | This branch |
|---|---|---|
| Who is affected | ordinary titles doing ordinary things | a title that assumes a low heap or physical numbers below 64 MB |
| How it shows | wrong sound or dead input, no error | mostly a fault that names the address |
| Seen | in three titles | not yet, in limited testing |

## 11. Found while doing phases 1 and 2

- **`xbox_SetMapSize` changed meaning.** It grew the RAM mapping with the
  heap inside. RAM is now always the console's size and the call raises the
  arena's top. No title in this workspace sets it.
- **Checks that meant "below RAM size" were in more places than the bound
  macros.** The C++ exception walker and the guest KEVENT lookup had the
  constant written out and would have rejected every stack frame and heap
  event.
- **On a host with no span reservation, unmapping the mirrors left their
  addresses free** for the host to reuse. They are now held no-access.
- **Outside this repository, since fixed:** the shared debug support's
  `xr_guest_readable` (static-cling) accepted only addresses below 64 MB and
  the window, so `XR_PEEK` and `XR_POLL` refused heap and stack addresses.
  It now asks the host about any other address (static-cling `80ae277`).
  Crash reports printed the guest stack throughout.
- **Not updated, cannot be built here:** `tests/memory_layout_posix` expects
  every mirror mapped and aliasing RAM.

## 12. Risks

| Risk | How it shows | Answer |
|---|---|---|
| A title assumes its heap is below 64 MB (pointer packing, a 26-bit mask) | Crash, or wrong bytes with no crash | Levels on the four titles |
| A title checks a physical number, or indexes a per-page table with it | Assertion or out-of-range access | The fault names it; a title that needs numbers below 64 MB needs option A |
| A title turns a heap physical number into a window address itself | Fault at `0x84000000` or above | Show the arena in the window too (it fits: the window region is 256 MB) |
| A title uses the mirrors in a level | Fault between 256 MB and 2 GB, naming the address | `RECOMP_MIRRORS=1` |
| Phase 3, shape 1: Burnout 3's contiguous blocks move | Its old race crash returns | Race sessions before anything is merged; shape 2 avoids it |
| POSIX hosts | Untested | Needs a POSIX run |
| `xbox_SetMapSize` titles | None in this workspace | Untested |

## 13. Not decided

- **A port's `RECOMP_CONTIG_AVOID_IMAGE=1`** can go now that the model is
  the default on Windows: top-down filling ignores it (section 7).
- **A hardware reference for top-down filling.** It rests on the toolkit's
  own notes and on five titles played.
- **Other weak points, untouched:** the 65,536-entry block table stops
  recording when full; heap size rounding wraps for requests near 4 GB.
- **Upstream.** Phase 2 moves every heap address in every title, which
  upstream's policy keeps behind a switch. The `mem2` branch is that switch;
  whether and how to propose it (the interface between the two models) is
  not decided.
- **The default on POSIX hosts.** The classic model until the model has
  been run there.
