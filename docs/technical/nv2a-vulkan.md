# A Vulkan Rasteriser for the Pushbuffer Executor

> Part of the [Documentation Index](../INDEX.md)

> **Status: draws.** Steps 1 to 7 of the [plan](#steps) are done: one title's
> menus and races draw on Vulkan, vertex programs and renders to texture
> included. What is drawn stays on the card, or is copied back to guest memory at
> each flip or when the title looks. Step 4's
> [design](nv2a-vulkan-sync.md) is agreed and the rest follows it.

The [pushbuffer executor](pushbuffer-executor.md) decodes what a title asks
the NV2A to do and draws it on the CPU, into guest memory. This page plans a
rasteriser on Vulkan, fed by the same executor, that can carry a 3D title at
full speed. The CPU rasteriser stays, for comparison and for hosts without
Vulkan.

## Why at the pushbuffer, not at the D3D calls

A renderer can also hook the title's Direct3D entry points and draw each call
itself. That breaks down when a title writes NV2A commands of its own,
replays recorded pushbuffers (`RunPushBuffer`), or links a Direct3D whose
small functions are inlined (D3D8LTCG): the hooking renderer then has to
decode pushbuffer commands as well, and is two renderers in one. The executor
sees every command whichever way it was produced.

## Upstream

Upstream's direction is in issue #306: `Nv2aBackend` stays the contract and
grows in small PRs with synthetic tests, full renderers stay downstream for
now, and presentation is a host service (a back end produces and reads back
surfaces and does not own the window). So:

- The renderer is developed downstream, under MIT, game-neutral, in a form
  that could go upstream. It lives in a folder of its own on the toolkit
  fork.
- The contract extensions, renderer-neutral helpers and tests it needs are
  proposed upstream one at a time.
- PR #346 (a D3D11 renderer for the executor) covers much the same ground.
  This design is independent of it: its code is not read until this one
  draws, then the results are compared.

## Licensing

The practice is the toolkit's (see [NOTICE](../../NOTICE)):

- The code is this project's own, under MIT.
- A hardware rule taken from xemu is cited in a comment where it is applied
  and listed in NOTICE under "Derived algorithms". No xemu code is copied
  into MIT files.
- Nothing is taken from GPL-licensed renderers.
- The Vulkan loader is opened at run time; building needs the Vulkan headers
  (Apache-2.0 or MIT).

The combiner shader is written from the public OpenGL combiner specifications
(`NV_register_combiners`, `NV_register_combiners2`) and the NV2A's register
words, not ported from `nv2a_combiner.c`, which serves to compare output.
Where those specifications do not settle an NV2A rule, the source used is
cited like any other.

NOTICE lists `d3d8_swizzle.h` and `d3d8_combiners.h`. It does not list
`nv2a_combiner.*`, whose header says its encoding follows xemu's `psh.c`.

## What the CPU rasteriser covers

It is a partial reference. Read from the code at `ec64864`:

| Drawn | Not drawn |
|---|---|
| Triangles, strips, fans, polygons, quads, quad strips | Points, lines |
| Depth test and write, in host float buffers (4, by zeta address) | Stencil test and operations, depth in guest memory |
| Blending, alpha test, colour mask, fog | Face culling, polygon offset |
| Four stages: 2D/3D projective, cube, pass-through, clip plane | Bump-environment and dot-product stage modes (read zero) |
| Register combiners | |

The cull, stencil and clip-range registers reach a back end
(`Nv2aRenderState`), but nothing in the CPU fill reads them. A Vulkan frame
that differs from the CPU one is therefore not wrong by itself.

What the CPU rasteriser does not draw is checked by small synthetic tests
whose result is known from the state's definition (a stencil-masked quad, a
culled triangle), and by eye against frames of the Direct3D-level renderer.
Its code is not read, only its pictures.

## Decisions

Some steps carry stopgaps that later steps remove. These rules keep them from
limiting the end result:

1. **The hand-off is the batch, through `Nv2aBackend`.** A versioned,
   optional extension hands over the batch before transform: attribute
   streams, indices (or inline and immediate data), the vertex program and
   constants, all four texture stages and the combiner state. A back end
   without it keeps getting triangles as now.
2. **No GPU stall in the steady state.** A flip, a semaphore release or a
   report waits for the GPU only when the guest is about to read what was
   drawn.
3. **Decoding stays separate from recording.** The executor runs on the NV2A
   ack thread, which also answers the title's kickoffs. The hand-off can be
   queued to a render thread, even while the first version records inline.
4. **Pipelines are cached by state from the start,** keyed by what Vulkan
   bakes into one (blend, depth, stencil, topology). Combiner state goes to
   the shaders as data.
5. **Performance has a target and checkpoints.** The first title must run at
   least as fast as the Direct3D-level Vulkan renderer it would replace, at
   the same spots, on the same toolkit commit, from unlogged runs.
6. **Enhancements come later, but the caches must allow them.** Internal
   resolution above the title's own, multisampling and widescreen are goals.
   The surface cache design keeps host size apart from guest size.

Windows first. Other hosts are untested until someone can run them.

## Shaders

Ubershaders: one vertex and one fragment shader that interpret the NV2A state
from buffers. Their SPIR-V is committed as a generated header with the source
and a script to regenerate it, so a toolkit build needs no shader compiler, and
a bundled GLSL compiler is not planned.

The fragment's is specialised too (`state.glsl`): what decides its program can be
constants of the pipeline, which the driver folds. Such a pipeline takes about
280 ms to compile and a frame of the first title has 36, so threads at the lowest
priority, a quarter of the cores, compile them, and the pipeline that reads the
state draws until one is there: the same picture, but for a level or two in a
byte of a million. `RECOMP_NV2A_VK_SHADER_COMPILE=off` turns that off; with `sync`
each is compiled before its first draw, to compare.

- The combiners' registers are no array: indexed by the state it is in memory,
  and was nine tenths of a frame's GPU time at a high resolution.
- The pipelines that read the state share their fragment part
  (`VK_EXT_graphics_pipeline_library`): one compile of half a second for 15.
- Compiled pipelines are kept in `nv2a.cache` beside the executable
  (`RECOMP_NV2A_VK_PIPELINE_CACHE=<path>`, or 0), written three seconds after the
  last one. Not shown to help on the one card here, whose driver keeps its own.

## Steps

0. **Baselines.** A separate build on the toolkit commit this branch is based
   on. Two measurements are taken later, because they need a person at the
   pad or a frame to replay:
   - *where the CPU rasteriser's time goes*, from step 2's replay: see
     [Measured](#measured);
   - *the Direct3D-level renderer's frame times* at the spots of rule 5: the
     race start is [measured](#on-vulkan), traffic and crashes are not.
     Burnout 3 is known to reach a race on the CPU
     rasteriser, far too slowly to compare the two live.
1. **Contract extension**, with a fake-backend test. The CPU rasteriser's
   behaviour does not change.
2. **Capture and replay.** One frame's commands and the guest memory they
   read, saved locally (game data: never committed) and replayed offline
   through both rasterisers with an image diff. A capture taken while
   another renderer draws lacks whatever the frame reuses from earlier ones:
   the executor then draws nothing into guest memory.
3. **First Vulkan path.** CPU-transformed vertices, combiners, four stages,
   blending, depth, stencil and culling. Shown by a readback into guest
   memory at each flip through the existing `present_sync` hook, which stalls
   the GPU: a stopgap steps 6 and 7 remove, and nothing may depend on it.
4. **Cache and sync design review**, from what step 3 measured: renders to
   texture, target reads, CPU writes to surfaces. The
   [design](nv2a-vulkan-sync.md) proposes an order for steps 5 to 7.
5. **GPU vertex stage,** the CPU transform kept as a fallback. Done.
6. **Texture and surface caches** with guest write tracking, and readback on
   demand.
7. **Presentation and reports.** Readback cost measured against a no-copy
   path, then decided; reports from occlusion queries.
8. **The rest:** anti-aliased surfaces, remaining formats and primitives,
   performance, then the enhancements of rule 6.

## Step 3 as built

`src/nv2a_vk`, off by default. Build with `-DXBOXRECOMP_NV2A_VK=ON` (and
`XBOXRECOMP_VULKAN_INCLUDE`, the Vulkan headers); run with
`RECOMP_PB_EXEC=1 RECOMP_NV2A_VK=1`. It needs a Vulkan 1.3 device with
descriptor indexing; without one the CPU rasteriser draws.

- **Guest memory stays the truth.** A drawn surface is copied back at the
  flip, before a batch samples it, and before another surface that overlaps
  it is used (since 6a at the flip, and otherwise only where the card cannot
  do it). What the title writes into a surface is merged by comparing with
  a copy of what was last read or written. Depth and stencil stay on the GPU.
- **Colour surfaces are BGRA8 on the host,** converted on the way in and out
  (since 6a in the title's own format).
- **Textures are decoded on the CPU** and looked up by a hash of their guest
  bytes, taken at most once a frame each.
- **The vertices come from `nv2a_batch_transform()`,** multiplied back by w so
  that Vulkan clips and interpolates. Since step 5 only those without a vertex
  program do.
- **Pipelines** are keyed by topology, blending, colour mask, depth attachment
  and depth clamp; the rest is dynamic state or data.

Regenerate the SPIR-V header with the `nv2a-vk-spirv` target of
`src/nv2a_vk/shaders` (needs a glslang library).

Where its rules come from: `NV_register_combiners`, `NV_register_combiners2`
and `NV_texture_shader` for what the combiners and stage programs compute;
envytools' register database (`nv10_3d.xml`, `nv20_3d.xml`) for the bit
layouts; xboxdevwiki's NV2A/Pixel_Combiner page for what the NV2A adds. Three
rules come from no document:

- Window positions with w > 0 are cut to 1/16 pixel. Direct3D's own viewport
  offset of 0.53125 is half a pixel plus half of such a step, and without the
  cut a full-screen copy samples one texel row outside its source.
- `SET_TEXTURE_SET_BUMP_ENV_MAT` is Direct3D's 00, 01, 11, 10, from one title's
  plain (0.5, 0.5) scaling arriving as 0.5, 0, 0.5, 0.
- The window clip rectangles are applied as their bounding box, scaled by the
  anti-aliasing factor.

What this step left out is in [step 8](#step-8-so-far), or still
[not done](#not-done). Facing is decided by Vulkan after clipping, which the
executor page says differs from the NV2A for a triangle that reaches behind the eye.

It has never run under the Vulkan validation layers
(`RECOMP_NV2A_VK_VALIDATE=1` asks for them) and only on one NVIDIA GPU.

`tests/nv2a_vk` checks texture decode without a GPU, and through the executor:
clears, coverage, depth, stencil, culling, blending, alpha test, colour masks,
5:6:5 and swizzled targets, textures, render to texture, CPU writes, lines,
points, and 400 random combiner programs against `nv2a_combiner.c`. Those all
agree but for one input: the evaluator adds a negative spare0 to the secondary
colour as it is, where `NV_register_combiners` maps both with
`UNSIGNED_IDENTITY` first.

## Step 5 as built

`shaders/program.vert` interprets the vertex program: the instruction words,
the 192 constants and the raw vertex data go into the stream buffer and the
shader reads them from there. `nv2a_vk_vtx.c` finds which attributes a program
reads and copies the range of them a batch uses (one by one where the indices
are few and far apart). Positions then take the same path as before.

The fixed function is the CPU's but for the position: `shaders/batch.vert` takes
the title's through the composite matrix with the operations a program of D3D's
ends with (DP4s, RCC, MAD). A title may draw a light's pass with a program where
the depths equal a fixed-function pass's: with the positions from the CPU,
divided there, they differed in a part of the pixels.

On the CPU still, batch by batch: the rest of the fixed function, positions that
are already pixels, programs that write constants, attributes outside guest
memory.

Operations are `NV_vertex_program`'s and `NV_vertex_program1_1`'s. The
instruction words, and what those documents do not say, are
`nv2a_vsh_interp.c`'s. One rule differs from that interpreter: 0 times infinity
or NaN is 0, as the extension asks.

`tests/nv2a_vk` runs 967 random programs on both and finds no difference, every
vertex format at every alignment, arrays, inline and immediate data, and the
extension's corner cases. The captured frame with the two differs in 92 pixels,
by 2 of 255 at most.

## Step 6a as built

Section 2 of the [design](nv2a-vulkan-sync.md): what the GPU drew is sampled
on the GPU. Case by case it is as designed, with these differences:

- **A rectangle, a copy of the surface being drawn into, cube faces and mip
  levels are one thing:** the texture has an image of its own in the title's
  texel format, and each face and level of it is a surface's pixels, copied on
  the card when that surface has changed, or guest memory's, uploaded when
  those have. Only one whole surface is sampled as it is.
- **Texel formats are views.** A 5:6:5 surface sampled as A1R5G5B5, or
  A8R8G8B8 as A8B8G8R8, is the same image through another format or channel
  order. What no format covers (R6G5B5, palettes, YUV, signed channels, DXT)
  takes the way through memory.
- **Two records may cover the same memory** in two layouts. Binding one hands
  the pixels over with a draw, sampling one takes a copy; only equal ranges,
  tightly packed, and the same pixel format. Other overlaps go through memory.
- **5:6:5 is made of a pixel by the card, at each draw.** How is the device's
  choice: the one tested cuts the low bits of eight, as copying back used to.
- **X1R5G5B5's spare bit** is written by clears and left alone by draws.

A texture or rebinding that goes through guest memory instead, as before 6a, is
logged, once for each texture format.

`tests/nv2a_vk` has each case with its picture, and that only the flip waits;
the way through memory; and 12,000 random draws, clears, samples, flips and
writes by the CPU over overlapping surfaces of 8-bit channels, the same as
by way of guest memory, byte for byte.

## Step 6b as built

Sections 3, 5 and 6 of the [design](nv2a-vulkan-sync.md), without `touch`
(6c):

- **`RECOMP_NV2A_VK_COPYBACK=never`,** the default, or `nv2a_vk_copyback()` for a
  title's own (the variable wins). `always` waits at each flip as before.
- **Frames in flight** (`never` only): two, each with its command buffers, its
  half of the upload buffer and its list of things to destroy.
  `RECOMP_NV2A_VK_FRAMES_IN_FLIGHT=1` or 3 to compare. The flip submits and goes
  on, waiting only when the GPU is as many frames behind.
- **The picture** is not copied to the host: the flip records a copy on the
  card for [the window](#the-window). The executor asks through
  `nv2a_backend_set_present()`; a back end without it presents as before.
- **What reads guest memory itself gets it, with a wait:** textures the card
  cannot make, palettes, vertex arrays in a drawn surface, batches the CPU
  rasteriser draws, frame dumps. A capture (`RECOMP_FRAME_CAPTURE`) does not
  ask: what it saves of drawn surfaces is stale with `never` and zero with
  `touch`, so `always` is the default while it is set.
- **What the title's CPU writes** is found by comparing with stale memory, so
  a write of what was there already does not show. Three rules narrow it: a
  surface it rewrote more than half of is its, whole; so is a surface it wrote
  to and then uses as something else; and freeing contiguous memory
  (`RECOMP_HEAP_RECLAIM`) forgets the surfaces in it.

`tests/nv2a_vk`: twenty flips without a wait; CPU pixels over a
picture, and a whole one; a picture sampled in the next frame; a texture loaded
over an old surface; freed memory; vertices out of a drawn surface; and 7,200
random operations ending in the same memory as with `always`.

## Step 6c as built

Section 4 of the [design](nv2a-vulkan-sync.md): `RECOMP_NV2A_VK_COPYBACK=touch`
hands the title's CPU what the GPU drew when it looks. Flips are 6b's. Not the
default: no title that reads its own pictures has been played with it.

- **`src/kernel/guest_guard.[ch]`** is the protection, for any renderer: both
  views of the contiguous window guarded, a third that is not, one owner thread
  that decides, and the others' requests to it. The rasteriser reads and writes
  guest memory through the third view.
- **Who answers.** The executor's thread, at every word of the pushbuffer it
  walks, each pass of its loop, and while it waits for a title's trap handler.
  The other things it waits for are the GPU and a thread, which touch no
  guarded page. When that thread itself touches a guarded page (a batch the
  executor draws, a dump) it copies back inside the fault.
- **Nobody answers for two seconds:** everything is opened, said on stderr, and
  the title goes on with old pixels. What it wrote meanwhile is found by
  comparing once the owner is back.
- **`guest_guard_access()`** goes before a guest pointer is given to Windows:
  `NtReadFile`, `NtWriteFile`, the memory kept over a quick reboot,
  `MmSetAddressProtect`, `MmQueryAddressProtect` and the video overlay's check.
  A title's own read-only pages become writable once a surface was on them.
- **A guarded surface is not compared:** until a write opens it, guest memory
  is what it was. Nor is a clear of it written to guest memory. Both are less
  work than `never` does.
- **A surface outside the window** is copied back at each flip, said once.
- **The list** of the design is printed as it grows, with
  `RECOMP_NV2A_VK_STATS=1`: the first time each place in the program touches
  each surface, in it or beside it on the page, with the function's name where
  the title's crash reporter has loaded symbols. The line each second counts
  touches.

Windows only, as designed: elsewhere `touch` says so and is `always`.

`tests/nv2a_vk`: every other test again with `touch`; a read and a write through
each view, in a frame and after it; writes of what stale memory held; three
surfaces and something else on one page; freed memory; a surface outside the
window; vertices out of a drawn surface; another thread reading, and writing, in
the middle of frames; a file written from and read into a drawn surface, refused
by Windows without `guest_guard_access` and not with it; an owner that does not
answer; and 12,000 random operations with the CPU reading and writing at any
time, which sees and leaves the same bytes as with a flip and a copy back after
each. Not tested: a file through `NtReadFile` in a title, a trap handler that
looks, and a batch the CPU rasteriser draws.

## Step 7 as built

Visibility tests only, section 5 of the [design](nv2a-vulkan-sync.md). Showing
the card's image without a copy is left: no measurement calls for it so far.

- **The contract** (`nv2a_backend_batch.h`, version 2): `report_clear`, `report`
  and `idle`. A back end that takes a report answers it later, from any thread,
  with `nv2a_batch_report()`. What the executor's own drawing counted is added.
- **Counting:** batches drawn while `SET_ZPASS_PIXEL_COUNT_ENABLE` is on are in
  occlusion queries, with exact counts where the device has them. A query ends
  with the rendering it is in, so one test may be several.
- **Answering:** a thread waits for the submission (a timeline semaphore), adds
  the queries up and writes the report. The executor does not wait.
- **Submitting:** a flip does, and so does `idle`, which the executor calls when
  it has carried out all the title sent and a report was taken. That is what a
  title waiting for its answer has come to. The design had each `GET_REPORT`
  submit; a title with many tests a frame would pay for each.
- A frame may be up to 16 submissions, each with its own command buffers; past
  that one waits.
- Reports into memory the title freed are dropped.

An answer arrives when the GPU has it, as on the console, and may be two frames
late. A title that reads a report after a fence or a semaphore, without looking
whether it is done, would find it unfinished: those are released when recorded.
Direct3D's own `GetVisibilityTestResult` looks.

Burnout 3 asks one test a frame, for the sun, and reads an earlier one without
waiting; until it is there it keeps the last answer.

`tests/nv2a_vk`: counts worked out by hand, with depth, alpha test, masks,
clears, changes of target and flips between; the frame going on after a
submission; titles that wait at once, 40 times a frame; 3,000 tests and 5,000
queries in flight; freed memory. Not tested: a test whose batches the executor
draws in part (the contract's side is), a device without exact counts, and a
title that waits (Burnout 3 does not).

## Step 8, so far

**Dithering** (`SET_DITHER_ENABLE`), into surfaces of five and six bits a
channel. The shader picks one of the two values nearest to the colour, as
OpenGL has it, by a 4x4 ordered pattern. The NV2A's own pattern was not found;
envytools has the NV1's and NV3's, which are for five bits. OpenGL dithers
after the blend, the shader before it: the same when adding, weaker otherwise.

Burnout 3 makes one such picture a frame: the finished frame at 320x240 in
5:6:5, which it then blends over the 640x480 one. With dithering 70,473 pixels
of the captured frame change, 44 by 2 of 255 and the rest by 1; the frame takes
as long. Its other dithered draws are into 8:8:8:8, where there is nothing to
do.

`tests/nv2a_vk`: 52 greys into 5:6:5 and 1:5:5:5, adding, 8:8:8:8, and off.

**What was left out.** A second public source was found for this: the results
of `nxdk_pgraph_tests` (github.com/abaire), pictures taken on an Xbox 1.0, with
the tests that draw them (MIT; the pictures CC0). Nothing of either is in the
toolkit; a rule taken from them says so where it is. The register layouts are
envytools' (`nv10_3d.xml`, `nv20_3d.xml`), the stage programs
`NV_texture_shader`'s.

- *Both faces' colours* (`SET_TWO_SIDE_LIGHT_ENABLE`), of programs and of the
  executor's own lighting. *Specular colour* on or off, and its alpha.
- *Fill modes*, front and back, with the polygon offset of each; a quad or
  polygon as lines has no diagonals. *Line width* and *point size*, a program's
  own too, *point sprites*, *smooth lines* (Burnout 3 draws its wires so).
- *Border texels* of a swizzled 2D texture's first level; of the others only
  the texture itself is read, and the size it has in memory is right.
- *Logic operations.*
- *Floating-point depth.* The images stay D32F, a depth in them is the title's
  over the largest of its format. Of the 24-bit one's, those below 4 are 0.
- *Stage programs 9 to 18:* dot products looked up in 2D, 3D and cube textures,
  mirrored at them, or as a depth; and the dependent reads. A draw that sets
  its depth has a fragment shader of its own, as such a one is tested late.
  Three bits of `SET_DOT_RGBMAPPING` for each stage from the first one that
  can have it, in four: envytools begins a stage earlier, titles' programs do
  not.
- *Depth buffers as textures,* compared as `SET_SHADOW_COMPARE_FUNC` says, with
  a linear filter four at a time: a copy made on the card, or guest memory
  where the GPU drew none. The four are gathered at the corner they share: at a
  light's plane through the centres of texels the GPU may round to other ones than
  the weights are for.
- *W-buffers* (`SET_CONTROL0`'s `Z_PERSPECTIVE_ENABLE`), as Splinter Cell's scenes
  are. The image holds 1 over each depth, which is linear across a triangle as a GPU
  has it, so early tests and several samples work as before; comparisons and the
  clear are turned round, and who samples it gets depths. `SET_CLIP_MIN` and `MAX`
  are tested for each pixel: as clip distances they made the depths of two passes
  with different limits differ, where a title draws one where the other's are equal.
  The first draw after a clear of all of a buffer decides its kind.
- *No colour surface:* depths and stencils alone, with no colour attachment.
- *Texels of 32 bits as another kind* (`shaders/depths.frag`, at every scale): a
  Z24S8 buffer sampled as A8R8G8B8 or another order of those, its stencil in the
  lowest byte, and pixels that were drawn sampled as 24-bit depths. Splinter Cell
  finds how far a pixel is from its depth buffer's upper two bytes, and shrinks a
  shadow map through a colour surface. Where a depth buffer and a surface share
  memory, the one drawn into last is meant; a depth buffer last drawn into more
  than 60 frames ago is not sampled as colours, as the title may have put a texture
  there since.
- *Volumes of DXT blocks,* in `NV_texture_compression_vtc`'s order. No title has
  shown one.

None of it is used in the Burnout 3 scenes looked at but the smooth lines and
the specular switch, so the rest is tested by `tests/nv2a_vk` alone. The rules
were broken in the code one by one, and the tests failed each time but one: a
linear filter on a sampler of depths, which only a validation layer would see.
None has been run here yet.

**Larger pictures** (`RECOMP_NV2A_VK_SCALE`, 1 to 4, or `nv2a_vk_scale()` at any
time; `fit` for as many as fill the picture in the window, once
that has kept its size for 30 flips): every image the GPU draws into is so many
times as wide and high, and everything else keeps the title's sizes. Copies to and from
guest memory pass through an image of the title's size. Three things need more
than a factor:

- A vertex of a flat picture (w = 1) on the centre of a pixel of the title's
  goes to the centre of a drawn one. Direct3D 8's pictures without the half
  pixel begin there, and would otherwise lack their first rows and columns.
- Visibility counts are divided, rounded up.
- A mip level one texel wide may be drawn larger than that level of an image
  that is larger by the scale; its part is cut.

The rest differs as it must: a thin line or an ordered dither is a smaller one,
averaged where the title's CPU reads it back. `tests/nv2a_vk` draws, samples,
counts and copies at 2, 3 and 4, and repeats the textures drawn by the GPU at
2 and 3.

**Antialiasing** (`RECOMP_NV2A_VK_AA`, `nv2a_vk_samples()`, off, 2, 4 or 8). A
surface has a second image, of several samples, beside the one everything else
uses, and which of them is newest is known. Drawing in depth goes to the samples.
Anything else that looks at the surface has them resolved first, once; anything
that changes the other image has its pixels spread over the samples before they
are next drawn to.

- *In depth* is a draw that depths or the stencil decide, or that writes them, and
  a clear of either. A picture drawn without depths goes to the resolved image,
  with no depth buffer, so a title's two-dimensional parts keep their pixels and
  full-screen passes cost what they did. A title that goes back and forth does so
  eight times a frame; the rest of that frame stays with the samples.
- *Which depth buffers:* 320x200 or larger, not square, and not of a surface the
  title itself has anti-aliased. Others are a texture's: environment maps, shadows.
  Neither is one drawn into without a colour surface, until there is one. One that
  grows from a small to a large one, or becomes one of these, loses its depths, as at
  a change of scale.
- *Edges on the centre of a pixel* of a flat picture go through the edge of one, as
  they go through the first drawn one's centre in a larger picture: it is covered
  whole or not at all. Its coordinates are half a drawn pixel on.
- *Visibility counts* weigh a query by its samples. *Depths as textures*, as depths
  or as colours, are one sample of each pixel.
- *Cut-outs* (`RECOMP_NV2A_VK_ALPHA_TEST_AA`): an alpha test for more or less, without
  blending, keeps or drops a whole pixel (`pixel`). Or (`sharp`, the default) a
  fragment shader of its own keeps as many samples as the pixel's part of the edge,
  from how fast alpha changes there (`fwidth`), or (`soft`) as many as alpha says;
  where alpha does not change, it is the test. Or (`sample`) the fragment's program
  runs for each sample, with its varyings there; a flat picture's (w = 1) stay the
  pixel's, or text drawn pixel for pixel would blur. The alpha written stays the
  title's.
- *Supersampling* (`ssaa2` to `ssaa8`, `nv2a_vk_shade_samples()`): every batch that
  goes to the samples has its fragment's program run for each of them, as `sample`
  does, and a texture's level of detail finer by as much. Cut-outs keep their modes:
  with `soft` and `sharp` a sample stays if it is one of those its own alpha covers,
  with `pixel` a cut-out is shaded once for a pixel. Their textures keep the pixel's
  level of detail in every mode, as alpha is another one in a finer level: with that,
  Halo's bushes were fuller than without supersampling.
- A card with fewer samples uses what it has; without memory for them they are off,
  said once.

Burnout 3's scene is one 640x480 surface with a depth buffer, about 400 draws, some
160 of them alpha tested, all but 16 to 19 without blending. Nothing drawn after it
looks at depths. So a frame has one resolve, and nothing is spread.

`tests/nv2a_vk` (`test_many`): partly covered pixels, pictures over them and depths
after those, sampling in between, the title's own pixels, counts, stencil, turns,
masked clears, depths as textures, growing, cut-outs each way, and 2x and 3x
pictures with each number of samples.

**Faster.** Runs of indices and inline vertices go to the executor whole
(`nv2a_pb_exec_run()`: more than half of Burnout 3's words are index pairs), and
the hash that finds changed textures takes eight lanes of 16 bytes at a time,
four times as fast.

### Not done

Logged once when a title uses it: stage program 8 (BRDF), dot mappings 5 to 7
(hemispheres), border texels of volumes, cube maps and mip levels, points scaled by
distance in the fixed function, depth buffers sampled with another layout, as
another depth format or, of 16 bits, as colours; w-buffer draws over other depths,
which are lost. Silent: linear texture-coordinate generation, edge flags, smooth polygons,
fixed-function specular, depths in guest memory (the title's CPU finds none); in a
w-buffer, depths of less than 1, the polygon offset's slope by the host's rule, and
other draws' depths exact only where flat, without their range. The software
renderer has no w-buffer.

## Settings

`src/nv2a_vk/nv2a_vk_settings.c`: scale, antialiasing, cut-outs, anisotropy,
the picture's aspect ratio, the console's widescreen setting, the window's filter
and vsync, fullscreen, and the performance panel's mode, in `nv2a.ini` beside
the executable. Only what was chosen is written. A setting's environment variable
wins over the file; what is chosen while the title runs wins over both, and is
saved. Tools and tests, which call
`nv2a_vk_init()` themselves, read none; scripted runs set `RECOMP_NV2A_VK_SETTINGS=0`.
Widescreen is what the title is told when it starts, so it is the next start's.

- **Anisotropy** is the title's, or at least the chosen one for a texture with
  mip levels, linear in and between texels, whose levels the title does not
  clamp to the first. What is nearest (text, pixel art, depths) stays so.
- **The aspect ratio** is 16:9 when the title set a widescreen display mode
  (`xbox_DisplayIsWide()`). Direct3D 8's mode tables hold each mode twice, with
  bit 28 for the widescreen one (seen in six titles' executables; in one, set
  when the console's setting is on). Movies are stretched to it as on the console.
- **The menu** (`nv2a_vk_menu.cpp`, F1, in a build with `XBOXRECOMP_IMGUI`) is
  drawn in the panel's Dear ImGui context, by the thread that presents. The
  window's thread hands it the messages (`xbox_FramebufferWindowOnInput()`); the
  title sees no key or mouse meanwhile (`xbox_InputSetOverlay()`), its pads stay
  its own.
  What a setting does is its tooltip; a `*` is an environment variable that is set.
- **Text** is 14 pixels high whatever the window's size, and more only on displays
  higher than 1440: the menu and the panel cover a part of the picture.

`tests/nv2a_vk` does all of it, the menu by key and mouse messages, in a window
off the screen.

## The window

`src/nv2a_vk/nv2a_vk_present.c`, the only way these pictures are shown, on
Windows. A host or a card that cannot leaves the CPU rasteriser to draw; the
tools and the tests draw without a window.

- **The flip** records a copy of the surface into one of six images on the card
  and goes on. A picture no surface holds is uploaded from guest memory.
- **A thread** waits for the vblank the flip will be held until
  (`nv2a_pb_exec_flip_due`), then copies the image into the window's swap chain,
  in its [aspect ratio](#settings) and centred, and presents it. Flips are not
  evenly spaced, as the executor reaches them ahead of the hold; what is shown is.
  A flip that is late is shown at once, and with no vblanks or no pacing, all are.
- **Its queue** is the card's second graphics queue, so the executor never waits
  for the display; with one queue, a lock.
- **Vsync** is `RECOMP_PRESENT_VSYNC`. `fifo`, the default: each at a
  refresh of its own, so a slower display has the thread wait and pictures passed
  over. `mailbox`: the newest at the display's next refresh, with less delay, and a
  slower display does not hold it up. `adaptive`: as `fifo`, but a late one at
  once, torn.
  `immediate` or `0`: all at once. What the driver lacks becomes `fifo`, or
  `mailbox` for `immediate`.
- **Until the first flip** the window draws guest memory, with GDI
  (`xbox_FramebufferWindowSetExternal(2)`): a D3D11 swap chain would keep the
  window for itself. If Vulkan cannot use it, then or later, that is said and the
  process ends. A window closed with `RECOMP_CLOSE_KEEPS_RUNNING=1` only ends the
  pictures.
- **The performance panel** ([below](#the-panel)) is drawn over the picture by the
  same thread. It is drawn again twice a second when no
  picture comes, and at once when F10 changes it.
- **The video overlay** (PVIDEO): while there is one, the picture is copied to
  host memory at 640x480, the window's code draws the overlay over it, and that is
  shown (`SetExternal(3)`). The thread looks for its frames every 8 ms, so it
  needs none of the title's pictures. It costs the thread a wait for the GPU and
  the executor nothing. A 5:6:5 picture's colour key may be off by one.
  Tested with a made-up overlay only.

Presenting is a host service upstream (above); this stays an option of the fork.

Burnout 3's intro and menus, 24 s, muted, windowed, 144 Hz display, time between
presents as RTSS measures it (2026-11-23):

| | median | 1st - 99th percentile | within 1 ms of 16.67 | over 25 ms |
|---|---|---|---|---|
| D3D11, each picture once | 16.62 | 1.66 - 32.19 | 19% | 204 of 1430 |
| Vulkan | 16.68 | 15.38 - 17.87 | 95% | 0 of 1440 |

`nv2a-vk-draw-test present` (`nv2a-vk-present` in ctest) opens a window and reads
back what it is given: size, bars, formats, overload, resizing, minimising, the
wait for the vblank and, with `XBOXRECOMP_IMGUI`, the panel.

## Where a frame's time goes

`nv2a_perf.h`, part of the executor, so it needs no Vulkan. A frame is from one
flip to the next, as the thread that carries out the title's commands reaches
them; a flip is then held (`RECOMP_FLIP_PACING`), so the hold is at the start of
a frame.

| In the `[PERF]` line | Is |
|---|---|
| work | that thread busy, split into commands, draws, vertices and textures |
| once only | pipelines and textures made: not for every frame to do |
| waits | for the title to send more, for the held flip, for the GPU, copying back |
| gpu | the card busy, by its own clock, not a part of the above |

Work, once only and waits add up to the frame, to the clock's precision: they
are timed, each without what happens inside it. The split of the work is not
timed, since it changes thousands of times a frame: the code stores where it is
and a thread looks every half millisecond.

Which one limits the frame rate: the title if the thread mostly waits for it,
the GPU if it waits for that, the pacing if it waits for the flip, else the
work. The title works while this thread does, so the wait for it is not the time
it takes.

With it off, a pass over the captured frame takes 0.35 ms less than before (14.3
against 14.7, two frames), because the clock is no longer read at each draw. On,
it costs nothing that shows (14.3). The same picture, byte for byte.

A frame may be several submissions, for visibility tests. Each has its own two
timestamps; before, a frame was timed from its first to its last command, with
the time between two submissions.

Not done: the software rasteriser's work is under "draws" whole, and a
title that spins, waiting, is not told from one that works.

### The panel

`nv2a_vk_panel.cpp`: the same numbers over the picture in the [Vulkan
window](#the-window). F10 goes from off to a line, the frame graph, and all four
graphs; `RECOMP_NV2A_VK_PANEL` starts in one. The log does not need it, nor it the log.

It is drawn with Dear ImGui, which the toolkit does not carry:
`-DXBOXRECOMP_IMGUI=<its folder>` (1.92 or later; tried with 1.93.0) builds it,
on Windows, and the project must have C++ enabled. The thread that presents does
all of it, so it costs the renderer a copy of the kept frames, locked, at each
picture.

| | |
|---|---|
| FRAME | CPU (work, once only, copying back) and GPU. They overlap in time, so their sum is not the frame's. The line is what most frames take, a tick a hitch |
| WORK | its four parts |
| FIRST USE | pipelines and textures made; the numbers are the worst frame of ten seconds |
| WAITS | for the title, the held flip, the GPU, copying back |

The rate, its range, the jitter, the hitches and the ticks are those of the
pictures in the window (`nv2a_perf_shown`), which are more even than the flips;
"+2" after the hitches is two frames never shown. The bars are the executor's
frames, from one flip to the next. At the same time, RTSS and the panel agree:
15.8 - 17.5 ms (1st - 99th percentile) against 15.2 - 18.1 (least and most).

"limit" is what keeps frames from coming faster: pacing if flips are held over
5% of the time, else the GPU if it is waited for over 10%, else the title, else
the renderer.

### Tested

`tests/nv2a_perf`: with a clock set by hand. `tests/nv2a_vk`: that the GPU's time
reaches the right frame, all submissions of it, and what is made, copied and
waited for; and, in its window, the panel's bars and ticks.

## Measured

### On the CPU

A capture of two frames of a Burnout 3 race start (2026-11-23, replayed
offline on an idle 16-thread host, best of six passes, per frame):

| | ms | Runs on |
|---|---|---|
| Walk and state | 4.2 | one thread |
| Vertex transform | 23.6 | one thread |
| Triangle setup and fill | 152 | the worker pool |
| Total | 180 | |

1266 batches, 129,000 vertices. Fill is 85% of the frame, so step 3 comes
before step 5 as planned; but with the transform still on one CPU thread a
frame takes 28 ms before any Vulkan work, so step 5 is what reaches 60 fps.
One spot: other scenes will differ.

The replayed frame looks like the Direct3D-level renderer's picture of the same
moment, apart from thin wires, green traffic lights and some car and railing
detail. The wires are its line batches, which the CPU rasteriser does not draw
and the Vulkan one does; the car is probably the culled faces and the
reflection below; the rest is not explained. The frame uses no stencil,
polygon offset or reports, culls about half its batches, renders to six 128x128
swizzled targets (probably the faces of the cube map it samples) and to 320x240 and 64x32
ones, and samples DXT1, DXT4/5, P8, 5:6:5 linear and cube textures.

### On Vulkan

The same capture and host, an RTX 3080, per frame, CPU and GPU one after the
other:

| | ms |
|---|---|
| Total (best pass) | 41 |
| of it: vertex transform | 23.8 |
| copying surfaces back, with the wait for the GPU | 8.0 |
| textures (hashing 9.4 MB, decoding 3.5 MB) | 1.8 |

The GPU is waited for 16 times a frame and 21 surfaces are copied back, 14 of
them before the flip: the six cube faces, the reduced and blended copies of
the picture, each sampled right after it is drawn. 12.6 surfaces are uploaded
again and 7.1 textures decoded. The first pass takes 767 ms, most of it
creating pipelines.

The picture differs from the CPU's in the lines, the culled faces, the car's
bump-mapped reflection, interpolation with perspective of fog and colours, and
texture levels: one sign's texture fades out in its smaller levels, and the
CPU samples level 0 only. With level 0 forced and those set aside, what is
left is sub-texel edges (mean difference 2.5 of 255).

Live (Burnout 3, windowed, muted, 2026-11-23): 60 fps in the menus, 21-22 fps
in the first seconds of a race.

The Direct3D-level renderer at the same race start (same host and toolkit
commit, flip pacing and host vsync off, 1x, no anti-aliasing, 4:3, anisotropy
as the title asks, one run, frames counted each second): 123 fps over the
first 9 s, about 110 in the first three; 137 over the next 30 s of driving,
two crashes included. The target at that spot is about 8-9 ms a frame,
against 45 live on Vulkan now. The run also wrote the renderer's small
thumbnails. Dense traffic and longer crashes are not measured yet.

For step 4: the mid-frame copies cost more in stalls than in copying. Sampling
a drawn surface without leaving the GPU would remove most of the 16 waits.

With step 5, the same capture, best of 12 passes:

| | Vertices on the CPU | on the GPU |
|---|---|---|
| A frame, ms | 41-42 | 15.6-15.9 |
| of it: vertex transform | about 24 | 0 |
| copying vertices, programs and constants | | 0.4 |

1244 of the 1266 batches run on the GPU; 2.5 MB of vertices are copied where 16
were written. The GPU's own clock says 3.3 to 4.4 ms a frame.

Live, the same menu presses as above (2026-11-23, windowed, muted, paced,
statistics on): 43 to 45 fps over 50 s of driving, against 21-22. The GPU ran at
360-465 MHz and needed 6 to 7.5 ms there; it waits for the CPU and the CPU for
it, so neither figure is a limit.

With 6a, the same capture, best of 40 passes of both builds in turn:

| | Step 5 | 6a |
|---|---|---|
| A frame, ms | 15.9-16.2 | 11.4-11.5 |
| Waits for the GPU | 14 to 16 | 1 |
| Surfaces copied back before the flip | 14 | 0 |

(The waits and copies of step 5 are step 3's, which it did not change.) In a
frame 98 texture stages sample a drawn surface, for 12 rearrangements and 6
copies. Of the 11.5 ms, 3 are the one wait for the GPU and 1.3 the copying of
13 surfaces after it. The GPU's own clock says 2.8 ms.

The picture differs from step 5's in 24540 pixels, 24497 of them by 1 of 255 and
none by more than 4. By way of guest memory, 110 differ, the same 43 of them
by more than 1: those come from the formats, the others from sampling 5:6:5 as
it is instead of expanded to bytes.

Live, as above: 59 fps, which is the pacing (60), where step 5 had 43 to 45.
For a few seconds in one place it was 30, not looked into; with 6b's `never`
that was not seen again.

With 6b, the same capture, 100 passes, the mean (a pass no longer ends with the
GPU's work):

| | ms a frame |
|---|---|
| `always` | 12.7 |
| `never`, 1 frame in flight | 11.3 |
| `never`, 2 | 8.1 |
| `never`, 3 | 8.1 |

The picture is the same, pixel for pixel. With two frames the GPU is never
waited for: the CPU's 8 ms are the limit, the GPU's clock says 2.8.

Live, as above, `never`: 59 fps paced, and about 120 without pacing
(`RECOMP_FLIP_PACING=0`), where the Direct3D-level renderer ran at 123 to 137
at a race start. Not the same second of the race: the two were not driven alike.

With 6c, the same capture, four runs of 100 passes of each mode in turn (the
replay's memory is mapped twice since, so that pages can be guarded):

| | ms a frame, mean | Copied back, a frame |
|---|---|---|
| `always` | 11.6 to 11.9 | 13 |
| `never` | 7.4 to 7.8 | 0 |
| `touch` | 7.2 to 7.4, one run 8.1 | 0.5 |

The half a surface is the replay itself, which puts a surface's bytes into
memory at the start of each pass. The picture is the same, pixel for pixel.

Live, `touch`: in three scripted runs, some eight minutes with the menus and a
race start among them, the title touched none of its pictures. (Frame dumps find
the way through the menus, and what a dump has copied back may be read unseen.)
In the race, without dumps, nothing was copied back or waited for. 59 fps paced. Without pacing 118, within half a frame, for half a
minute: that flat it is some other limit, not this renderer's speed, and so may
the 120 above be.

Played by hand, `touch`, about seven minutes: no touch, nothing copied back, no
wait but for the flip's, 60 fps paced with less only where it loads.

With step 7, a little over two minutes of racing in a window: up to one report a
frame, when the sun is in view, and the sun's visible fraction, 0 in every frame
before, between 0 and 0.9. Three reports in ten are submitted before the flip.
No flushes, 60 fps paced. The capture takes as long as before.

With step 8 so far, the same capture, milliseconds for a pass (two frames),
unlogged:

| | Best | Mean |
|---|---|---|
| Before logic operations | 7.6 | 7.95 |
| With all that was left out | 7.8 | 8.08 |
| Runs of indices | 7.25 | 7.5 |
| The faster hash | 6.2 | 6.5 |
| Larger pictures, at 1 | 6.1 | 6.35 |
| at 2 | 9.1 | |
| at 3 | 14.4 | |
| at 4 | 21.8 | |

The last four steps at 1 have the same picture, pixel for pixel.

Live it does not show: at a car standing still, unpaced, 232.6 fps against 230.8
before (one run each, other programs busy). There the title's own thread is the
limit: 98% of a core, about a quarter of it in the exceptions that carry its
writes to the audio registers, while the thread that draws sleeps 13% of the
time. The larger pictures have been looked at from the replay, not in the window.

A pass is timed without what goes before it, the capture's memory put back, while
the GPU still works on the pass before. So where the GPU is the limit these times
are too low.

Antialiased, a capture of the idle spot in widescreen (2026-11-24), the GPU's own
milliseconds for a frame (`RECOMP_NV2A_VK_STATS`, RTX 3080, specialised):

| Scale | Off | MSAA 4x, `sharp` | `sample` | MSAA 8x, `sharp` | `sample` | SSAA 2x | SSAA 4x | SSAA 8x |
|---|---|---|---|---|---|---|---|---|
| 1 (640x480) | 1.4 | 1.4 | 1.4 | 1.4 | 1.4 | 1.4 | 1.4 | 1.4 |
| 2 (1280x960) | 1.5 | 1.5 | 1.5 | 1.6 | 1.6 | 1.5 | 1.6 | 1.6 |
| 3 (1920x1440) | 1.5 | 1.6 | 1.6 | 1.8 | 1.8 | 1.6 | 1.8 | 2.0 |
| 4 (2560x1920) | 1.6 | 1.8 | 1.8 | 2.0 | 2.1 | 1.8 | 2.0 | 2.6 |

How it got there, at 4:

| | Off | SSAA 4x | SSAA 8x |
|---|---|---|---|
| The registers an array | 18.9 | 37 | 61 |
| No array: what draws until the rest is compiled | 5.7 | 10.6 | 16 |
| Specialised | 1.6 | 2.0 | 2.6 |

Live at that spot, unpaced, at 4 with SSAA 8x, 16x anisotropy and widescreen:
227 to 231 fps with 2.7 ms of GPU, where the Direct3D-level renderer does 129 with
the same settings (one run each). The title's own thread is the limit. At 1 it
hid all of this: compare at high settings too.

## Deferred

Title-side hooks that depend on Direct3D calls (widescreen for a title that has
none, HUD placement) need a pushbuffer-level replacement. That comes much later, with the
enhancements of step 8.
