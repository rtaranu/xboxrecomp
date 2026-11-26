# Vulkan Rasteriser: Surfaces, Textures and Waiting

> Part of the [Documentation Index](../INDEX.md)

> **Status: design, agreed 2026-11-23.** Step 4 of
> [A Vulkan Rasteriser](nv2a-vulkan.md#steps). Step 5 (section 8), 6a
> (section 2, [as built](nv2a-vulkan.md#step-6a-as-built)), 6b
> ([as built](nv2a-vulkan.md#step-6b-as-built)) and 6c
> ([as built](nv2a-vulkan.md#step-6c-as-built)) and
> 7's reports ([as built](nv2a-vulkan.md#step-7-as-built)) are done.

## The problem

The console has one memory. A picture the NV2A draws can be sampled, read by
the CPU or shown at once. A PC has two: guest memory, and the graphics card's.
Step 3 keeps them equal by copying every drawn surface back to guest memory,
waiting for the GPU each time, before anything uses it.

Two needs are mixed in that one copy, and they have different answers:

| Who needs the picture | Example | Answer |
|---|---|---|
| **The GPU again** | a reflection, a shrunken copy, the last frame blended into this one | Use the card's copy. Always correct, so not an option but how it works |
| **The title's CPU code** | a screenshot for a save, a title measuring brightness | Copy back, only if it really looks. [A switch](#copying-back-the-switch), off by default |

## What was measured

The captured race frame of [Measured](nv2a-vulkan.md#measured), replayed
offline with temporary prints (2026-11-23, not kept).

What it samples of what it draws:

| Drawn | Sampled as | Match |
|---|---|---|
| 6 swizzled 128x128 targets | one cube texture, 61 times | each is one face, whole |
| 2 swizzled targets, 64x32 and 128x128 (5:6:5) | 2D | exact |
| 2 linear 640x480 | 2D, 13 times | exact |
| 3 linear 5:6:5, 320x240 and 160x120 | 2D, 14 times | exact |

*Exact*: same address, size, pitch, layout and pixel size. No draw samples the
surface it draws into, and none samples the frame before: every surface is
drawn before its first use. That is one frame of one title.

The other 6 copies are the cube faces: each is bound as linear, its alpha
cleared whole, then bound as swizzled and drawn. Step 3 sees two shapes of one
memory, so it copies back and uploads, twelve uploads in all.

What the copies cost, by leaving them out (the pictures are then wrong, so
this is a limit, not a result; best of 12 passes, per frame, the better of two
runs, which differ by up to 1.3 ms):

| Copies | ms |
|---|---|
| all 21 | 43.4 |
| all but the flip's | 41.3 |
| the flip's only | 36.2 |
| none | 36.0 |

- Copies and the waits they force are about 7 ms. Once the others are gone,
  the flip's own copy costs 0.2 ms: what is left of it is waiting.
- In the 36.0 the vertex transform is about 24-25 and the one wait for the GPU
  about 7. The rest, commands, state and texture checks, is about 4 to 5.
- So with step 5, and the CPU and GPU working at the same time, neither side
  would need much more than 7 ms, against a target of 8-9. That is tight on
  the GPU's side: its time was not measured on its own and the card's clocks
  were not recorded. It also leaves out what step 5 adds on the CPU (copying
  the vertices).
- Hashing 9.2 MB of textures a frame costs about 1 ms. Write tracking for
  textures is therefore not urgent.

## Design

### 1. Who has the newer pixels

One record per drawn surface, as now, with a state:

| State | Means | Changes when |
|---|---|---|
| *same* | guest memory and card agree | |
| *card newer* | drawn, not copied back | a draw or clear |
| *guest newer* | the title's CPU wrote pixels | found by comparing (below) or trapped |

**Every read the toolkit makes itself is known and always honoured,** in every
mode of the switch: sampling, another surface over the same memory, a batch the
CPU rasteriser draws because the back end refused it, vertices or palettes in
a surface, frame dumps. Only reads by the title's own code are ever skipped.

Comparing stays as in step 3: guest bytes against a copy kept at the last sync.
Changed pixels are uploaded over the card's, in runs, without copying anything
back first. Its limit, when guest memory is stale and nothing traps: a title that writes
the very bytes already there is not seen. A movie frame drawn by the CPU works;
a CPU clear to black over memory that was already black may not, and a texture
loaded over an old surface could keep some of its pixels. Two rules narrow it:
freed contiguous memory drops its surfaces, and a changed range used as
anything but that surface's own shape is the title's, whole.

### 2. Using a drawn picture as a texture

In order, the first that fits:

| Case | What happens | Wait |
|---|---|---|
| Texture is exactly one surface | the surface's own image is sampled, after a barrier | no |
| Texture is made of whole surfaces (cube faces, mip levels, slices) | copied into the texture's image on the card, when one changed | no |
| Texture is a rectangle inside a surface of the same pitch and pixel size | copied on the card | no |
| Same bytes, other layout (linear and swizzled, same pixel size) | rearranged on the card by a small draw | no |
| Samples the surface being drawn into | a copy is taken on the card first | no |
| Anything else | copied back and decoded, as now | yes |

The last row keeps the result right. It is counted and logged once per kind,
so a title that needs another row shows which. The fourth row also replaces
the twelve uploads of the cube faces.

**The last frame blended into this one** (motion blur, trails) is the first
row. A surface's image lives as long as its memory is not reused for another,
across flips, so the previous frame is already on the card. If the title keeps
it with a copy of its own, that copy is a draw and is on the card too. If it
draws into the same surface it blends from, that is the fifth row.

**Pixel formats.** Step 3 keeps every surface as 8:8:8:8 and rounds when
copying back. Without the copy the rounding would be lost and 5:6:5 effects
would look unlike the console's. Surfaces therefore get their real format on
the card (5:6:5, 1:5:5:5, 8, 8:8, 8:8:8:8), with the alpha of formats without
one read as 1 by the view. More precision can be an enhancement later.

**Depth** stays on the card. Sampling depth (shadow maps) is step 8 and uses
the same rows.

### 3. Copying back: the switch

`RECOMP_NV2A_VK_COPYBACK`, and a call a game project makes in its `main.c` for
its title's default; the variable wins.

| Mode | The title's CPU sees | Cost | For |
|---|---|---|---|
| `always` | every picture, at each flip | a wait at each flip | comparing, debugging |
| `touch` | every picture it looks at | nothing until it looks, then one wait and one copy | a title that looks |
| `never` | what it wrote itself, never what the GPU drew | nothing | the default; a title that does not look |

What `never` saves depends on the title:

- Over `always`: the wait at each flip.
- Over `touch`: little when the title does not look, only the page protection
  (its cost is not measured). Much when it looks each frame at something that
  does not matter.

What it can break: a title that reads its own pictures gets old bytes. Likely
places: a save's thumbnail, a brightness or lens-flare test by pixels, a still
picture behind a menu, a screenshot. And section 1's limit applies.

**Which titles need it** is answered by counting, not guessing: in `touch`,
`RECOMP_NV2A_VK_STATS=1` counts the traps and at exit lists each surface read,
its size and the guest address that read it. A play session with none is the
case for `never`, for what was played.

A fourth mode, handing over the last finished copy without waiting (one or two
frames old), fits brightness measurements. Not built until a title needs it.

### 4. Seeing the title look (`touch`)

Page protection on the surface's pages:

| State | Pages | A touch |
|---|---|---|
| *card newer* | no access | read or write: copied back first, then *same* |
| *same* | read only | write: opened, *guest newer* |
| *guest newer* | open | |

What makes it work or fail:

- **Every view.** Contiguous memory is mapped at `0x80000000` and again at
  `0xF0000000`; protection belongs to a view, so both are set.
- **The toolkit's own accesses** go through a third view that is never
  protected, or the executor would trap on itself. Only contiguous memory is
  tracked; a surface elsewhere is treated as `always`.
- **System calls do not trap.** `ReadFile` into a protected buffer fails
  instead. The kernel's file functions, and whatever else hands guest pointers
  to Windows, must first call a "make this range accessible" function. This is
  the change most easily missed, and it belongs to the runtime, not to this
  back end.
- **The trap runs on the title's thread and Vulkan belongs to the executor's.**
  The handler posts a request and waits. The executor serves requests where it
  already answers busy bits (every 256 words, and each pass of its loop) and
  before it waits for anything itself. It must never wait for a title thread;
  whether it does anywhere today has to be checked first.
- **Other handlers** (register pages, AC97, APU) exist; this one claims only
  addresses inside tracked surfaces.
- **Neighbours.** Surfaces end inside a page. A touch of something else in the
  last page also traps; the list above says whether the address was inside the
  surface.

Windows first. Elsewhere `touch` falls back to `always` until done.

### 5. Not waiting

The title is told the GPU has passed a point by a semaphore, a report or a
flip. Being told that lets it do three things:

| It may then | Why no wait is needed |
|---|---|
| reuse vertices, textures, palettes | they were read or copied when the batch was recorded |
| read a picture | section 4 traps it, or section 3 says it does not matter |
| go on to the next frame | bounded, below |

So:

- **Semaphores and fences** are released when the commands before them are
  recorded, as today.
- **Frames in flight:** two. Each has its own command buffers, part of the
  upload buffer, and list of things to destroy. At a flip the executor submits
  and goes on; it waits only if the GPU is two frames behind. One frame in
  flight is a setting, for lower delay.
- **Reports** (visibility tests) are answered today with a count of 0 for
  everything drawn on Vulkan (read from the code; its effect in a title is not
  checked, and the captured frame has none). They become occlusion queries; the answer is
  written to guest memory when the GPU has it. A title polls for it, so
  `GET_REPORT` submits what is recorded: otherwise a title could wait for
  commands never sent. Results arrive up to two frames later than the CPU
  rasteriser's.
- **The pacing of flips** (`RECOMP_FLIP_PACING`) is the executor's and does
  not change.
- **A render thread** is not planned yet. Recording is about 4 to 5 ms; decide
  after step 5, from a measurement.

### 6. Showing the picture

Today the window reads guest memory right after `present_sync`, which is why
the flip must wait. Two stages:

1. **Late copy.** The flip records a copy and goes on. When the GPU is done,
   the pixels are handed to the window from the copy, not through guest
   memory. Needs a window call that takes pixels. No stall; the picture
   appears when it is ready. 0.2 ms of CPU today at 640x480, more at higher
   resolutions.
2. **No copy.** The window service shows the card's image. Upstream wants the
   window to stay the host's (issue #306), so this needs an agreed interface:
   step 7, and only if stage 1 measures badly.

A flip to memory the title drew with the CPU (movies) shows guest memory, as
now. Frame dumps ask for a copy and wait.

### 7. Room for enhancements

A surface's record keeps its guest size and, apart from it, its card size.
Coordinates of linear textures are divided by the guest size, so a larger
image samples right. Copies on the card scale; copies back shrink; CPU pixels
are enlarged. Choosing which surfaces to enlarge is step 8.

### 8. Step 5's vertex shader: what it is written from

For the operations, `NV_vertex_program` and `NV_vertex_program1_1`. For the
instruction words, which those do not describe, the toolkit's own
`nv2a_vsh_interp.c` (its header documents the encoding and how it was checked)
and envytools where it has them. The CPU interpreter is the test: random
programs on both, as was done for the combiners. The fixed-function path is
described in [Pushbuffer Executor](pushbuffer-executor.md#fixed-function-transform)
and is translated after programs; until then those batches keep the CPU
transform.

## Order of work

Each lands on its own, with its tests, and is measured on the same capture.

| | What | Expected |
|---|---|---|
| 5 | Vertex programs on the GPU | the largest part: 24 ms of transform becomes a copy |
| 6a | Real pixel formats; section 2's rows | about 7 ms of copies and waits inside the frame |
| 6b | Frames in flight, late copy, `never` | CPU and GPU at the same time |
| 6c | `touch`, its list | correct for titles that look |
| 7 | Reports; no-copy presentation if needed | |

6b before 6c: `never` shows the speed at once for the first title, at its
known risk: a title that reads its own pictures needs `always` or `touch`.

## Tests

Without a title, through the executor, as `tests/nv2a_vk` does now:

- each row of section 2, checked by reading the result back; and that only the
  last row waits;
- a surface kept over a flip and blended into the next;
- the linear clear then swizzled draw;
- CPU pixels over a drawn surface, in each mode;
- each mode: a read after a draw gives new bytes (`always`, `touch`) or the
  old (`never`);
- `touch`: a read through either address, a write, a file read into a surface,
  a read while the executor is mid-walk;
- a title waiting on a report it just asked for.

Then the capture compared with step 3's picture. Differences are expected only
where 5:6:5 rounding now happens at each blend instead of at the copy.

## Risks

| Risk | Found by | Then |
|---|---|---|
| GPU time alone misses the target | timestamps, in step 5 | fewer state changes, specialised shaders |
| Page protection is slow or trips other code | 6c's measurement, the list | `always` or `never` per title |
| A title's CPU reads each frame, for a reason | the list | the fourth mode |
| A runtime function hands a protected pointer to Windows | fails with access denied, logged | add the call |
| Another title samples in ways this frame does not | the last row's log | another row |

## Decided

1. The modes and the default as above (`never`; `always` or `touch` per title).
2. The order, in particular step 5 first and `never` before `touch`.
3. Two frames in flight by default.
4. Step 5's sources (section 8).
