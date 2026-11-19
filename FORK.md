# This fork

[sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) with the
changes its recompiled games need, kept on top of upstream until upstream
takes them. This file is fork-only and never goes upstream.

## Branches

| Branch | What it is |
|---|---|
| `main` | Upstream's `main`, unchanged. Nothing is committed here; it only fast-forwards. |
| `integration` | `main` plus every change not upstream yet. The games pin commits on it (`TOOLKIT_COMMIT`), below this file's commit. Called `splintercell` until 2026-11-20; its last tip there is the tag `integration-0.17.0`. |
| `pr/<name>` | One upstream pull request each, cut from `main`, usually with a test the integration commit lacks. Rebased and force-pushed freely. |
| `integration-<version>` (tags) | The integration branch's tip before each rebuild, named for the upstream release it was built on (`integration-0.17.0`), so a game pinned to an older commit can still fetch it. |

## Working rules

- New work starts on `integration` (or a topic branch on it), where the
  games can test it. One concern per commit, so it cherry-picks cleanly onto
  a `pr/<name>` branch from `main`.
- When upstream merges anything, rebuild the integration branch with
  `tools/fork/rebuild-integration.sh`: it fast-forwards `main`, tags the old
  tip, rebases onto `main` (dropping what upstream took) and prints the
  `range-diff` to review and the push commands. It never pushes.
- Before force-pushing the rebuilt branch, push its tag: older pins must
  stay reachable on GitHub.
- Games pin `integration^`, the last commit before this file, never this
  file's commit: it is amended and replayed on top at every change, so its
  hash never lasts, while the commits under it stay in the branch's history
  until a rebuild. Moving this file then needs no tag and no re-pin.
  (`pin-<hash>` tags keep the FORK.md tips pinned before 2026-11-21.)
- Keep the table below current: a row per commit, updated when a PR opens,
  merges or closes. A commit upstream took leaves the table at the next
  rebuild.

## Status of each commit

Against upstream `main` 37b61d5 (v0.18.0), 2026-11-21, oldest first.
v0.18.0 took PRs #330-#343 and #286 (which replaced our OHCI fix).
With memory model 2 on this branch, fixes to the classic model alone are
dropped (2026-11-20): MmQueryStatistics, the image-page RAM name and
`RECOMP_MIRRORS=0`. The first two are still in tag `integration-0.17.0`;
the last went with the old `mem2` branch (model 2 has the mirrors off).
"In PR" is open on upstream; "not sent" is meant for upstream but has no PR
yet; "fork-only" stays here. Not listed: the commits adding this file and
`tools/fork/` (fork-only).

| Commit | Change | Status |
|---|---|---|
| 2ebd54b | C++ exceptions: dispatch MSVC throws to their catch blocks | not sent (wave 3: announce first, likely behind a switch) |
| e0e018f | kernel: relaunch the process when the title launches its own XBE | not sent (wave 2) |
| 9b5576c | nv2a_pb_exec: a switch to draw nothing, for a host renderer | not sent (wave 4: issue #312 first) |
| 081fb4d | fb_present: let a host renderer present to the window | not sent (wave 4) |
| 1a66805 | apu: RECOMP_APU_AUDIT lists the voices periodically | fork-only (debug) |
| b01919f | xaudio2: RECOMP_AUDIO_MUTE=1 silences the output | not sent (optional debug tool) |
| 3dae82c | usb: part-throw sticks in pad scripts | fork-only (debug) |
| 76069bc | input: an overlay on the window can hold the keyboard and mouse | not sent (with wave 4) |
| 45d717a | apu: RECOMP_APU_SSL_LOG times a stream voice's segment lists | fork-only (debug) |
| 91589dc | nv2a: the ack loop sleeps while a flip is held (RECOMP_ACK_HOLD_SLEEP_US) | not sent (parked 2026-11-27) |
| 55e1ef1 | nv2a: nv2a_pb_flip_hold_wait, a sleep for a held flip before KickOff | not sent (parked 2026-11-27) |
| cb2e9c1 | apu: mcpx_apu_vp_dump, every listed voice with its last voice command | fork-only (debug) |
| 415e2e6 | kernel: RECOMP_ETH_LINK=0 reports no Ethernet link | not sent |
| d2b3860 | kernel: RECOMP_VIDEO adds letterbox, 720p and 1080i to the dashboard's video flags | not sent (on top of upstream's #320) |
| fd58f15 | kernel: AV capabilities carry the dashboard's video flags | not sent (its branch pr/av-caps-video-flags was deleted 2026-11-20: upstream #320 is in) |
| 0f16126 | video: xbox_FramebufferWindowFullscreen | not sent (display, after wave 4) |
| e6ffc52 | kernel: read-only opens use the host cache despite no-buffering | in PR (#366, `pr/read-only-host-cache`) |
| 265582a | kernel: D3D's software method 5 wakes its far-fence wait | not sent |
| f39fef3 | apu: the trace never makes the audio wait on stderr | in PR (#370, `pr/apu-trace-buffered`, with 5099ec5) |
| 663c953 | kernel: answer an announced kickoff's PFB flush at once | not sent |
| a57efe2 | nv2a: trap software methods to the title's graphics interrupt | in PR (#377, `pr/pgraph-trap`, opened 2026-11-27, with 7862295; 4419ff0 squashed in, test `tests/pgraph_trap`: Halo flips through it) |
| ec48adb | nv2a: follow the ring's JUMP when the pushbuffer wraps | in PR (#369, `pr/pb-scan-wrap-jump`, on #368, with dbf43e0) |
| 9319d52 | nv2a: the semaphore release goes to the title's own semaphore object | reverted by afe3e97 (2026-11-27, with 0dbc994: saved one hand-found offset, not worth keeping; drop all three at the next rebuild) |
| 8034914 | nv2a: keep the pattern object's colour register in both walks | not sent |
| 9592c2e | nv2a: keep the colour clear value in PGRAPH 0x40186C | not sent |
| 91d1974 | kernel: an event the title sets itself is an event | not sent |
| e9c84b4 | docs: the NV2A ack thread, and a design to stop it polling | not sent (goes with the ack thread work) |
| 4ac621e | apu: only a trap writes FEDECMETH/FEDECPARAM | in PR (#367, `pr/apu-fedec-trap-only`) |
| b0bdac0 | apu: count the races between a title's methods and the frame thread | fork-only (debug; goes when the races are fixed) |
| 7ac7ce8 | apu: count the sequencer holds that run out (RECOMP_APU_SEQ_HOLD_US) | not sent (candidate: a hole in the sound that nothing reported) |
| 1ee85ef | apu: RECOMP_APU_SSL_TICK=0, no sample-limit interrupt | not sent (what the hardware raises the bit for is unchecked) |
| 341a9c6 | nv2a: present a paced frame on the vblank it is due on | not sent (parked 2026-11-27; needs 55e1ef1; candidate once the toolkit's own present uses it) |
| 97ea06c | kernel: [MEM] reports the most a title holds at once against the console's RAM | not sent (a report only; candidate on its own) |
| 7d100c0 | kernel: [MEM] counts what the console would spend pages on | not sent (with the above) |
| 4f959e8 | nv2a: the pushbuffer scan stops at the end of the window | in PR (#368, `pr/pb-scan-window-end`, with 0abbd3f) |
| 4e3be39 | kernel: memory model 2, in a file of its own (not called yet) | fork-only (memory model 2) |
| 334785e | kernel: RECOMP_MEM_MODEL=2 turns on memory model 2 | fork-only (memory model 2: the hooks; unset = classic model until 223ac3c) |
| 8f8773c | tests: memory model 2 beside the classic model | fork-only (memory model 2) |
| 20f71f2 | kernel: memory model 2 fills the contiguous window from the top | fork-only (memory model 2) |
| 0ce2d55 | kernel: memory model 2 ignores RECOMP_CONTIG_AVOID_IMAGE when filling from the top | fork-only (memory model 2) |
| 18209cb | kernel: under memory model 2, MmQueryStatistics reports the RAM really left | fork-only (memory model 2; the classic-model version, `77b8cc1` in tag `integration-0.17.0`, was dropped 2026-11-20) |
| fcdf452 | docs: memory model 2, its design, the branch and what was played | fork-only (memory model 2) |
| 06955e3 | kernel: KeBugCheck logs its call site and the stack above it | not sent (a report only; candidate on its own) |
| 4da8f8f | kernel: a bug check reports the thread's IRQL and every raised thread | not sent (a report only; with the above) |
| 6e3f41f | usb: report an OHCI ISR that runs 50 ms or longer | not sent (a report only; candidate on its own) |
| 223ac3c | kernel: memory model 2 is the default on Windows | fork-only (memory model 2) |
| e15bcad | nv2a: the flip interval, capped by RECOMP_FLIP_INTERVAL_MAX or the title | fork-only for now (2026-11-27; builds on the flip hold, 55e1ef1 and 341a9c6; no title calls it yet) |
| 24886b6 | docs: the flip interval, the game's cap and policy and the forced override | fork-only for now (with the above) |
| 0abbd3f | nv2a: the scan does not follow a target past the window | in PR (#368, squashed into 4f959e8 there) |
| dbf43e0 | nv2a: a wrap finds the ring's JUMP before running anything | in PR (#369, squashed into ec48adb there) |
| 0dbc994 | nv2a: the fence mirror stays until the semaphore object takes a release | reverted by afe3e97 (with 9319d52) |
| 5099ec5 | apu: a cut trace line keeps its newline; the trace test | in PR (#370, squashed into f39fef3 there) |
| 7133f92 | kernel: persisted contiguous memory survives the self-relaunch | not sent (upstream candidate) |
| f0684a0 | nv2a: the forced flip interval cap can be switched while running | fork-only for now (with e15bcad; a settings panel's switch) |
| 95d53f1 | kernel: the per-read [READ] line only under RECOMP_LOG_READS=1 | not sent (candidate on its own: a line per read in every run's log) |
| fdc9213 | recomp: mov to or from a control register is an unimplemented site | not sent (upstream candidate, with its test: Fable's cr3 reloads did not compile) |
| d21a16a | kernel: buffers in committed extended VMA pages are mapped | not sent (upstream candidate: Fable's heap above the mirrors had its paths refused) |
| 6fbb4b3 | kernel: fault in extended VMA buffers before host file I/O | not sent (upstream candidate, with d21a16a: Fable's demand paging) |
| 419e0b0 | kernel: room for 32 live worker stacks, not 8 | in PR (#378, `pr/thread-stacks`, with db3d9e0: Fable's workers past eight ran inline and hung boot) |
| e9de7c9 | kernel: file I/O into extended VMA buffers goes through a host buffer | not sent (upstream candidate; replaces 6fbb4b3's touch, which left a window for a page-out) |
| 647c9ae | kernel: guest-bytes critical sections do not lose a wakeup | not sent (upstream candidate: `RECOMP_CS_MODE=guest` deadlocked Fable's frame allocator) |
| eafe065 | kernel: guest-bytes critical sections take a release from any thread | not sent (upstream candidate, with 647c9ae; console semantics, not proven to fix a title yet) |
| 7fda577 | kernel: RECOMP_CS_WATCH traces one lock in either mode | in PR (#379, `pr/cs-tryenter`, second commit, on top of the fix; without eafe065's any-thread release) |
| e4e23ec | kernel: RtlTryEnterCriticalSection takes the lock Enter and Leave use | in PR (#379, first commit, with a new test `tests/kernel_cs_tryenter` not on this branch: Fable's loader froze after its intro movies) |
| 7862295 | nv2a: a trap's wait answers the PFB flush as it appears | in PR (#377, second commit, with a57efe2: with RECOMP_PGRAPH_TRAP, Fable ran at 20 fps, a steady 30 with it) |
| ec64864 | nv2a: the DMA walk logs when it gives up and skips to PUT | not sent (upstream candidate: a walk looping on a stale jump-back skipped Fable's fence releases silently) |
| 586f260 | nv2a: a batch below the threads' threshold counts one worker on Windows too | in PR (#371, `pr/raster-last-workers`, opened 2026-11-27; moot if #231 lands: 0410734 is upstream's, its Windows half missed it; fixes the nv2a-raster-batch test) |
| e6d2664 | pb_exec: transform inline and immediate batches in fixed-function mode | in PR (#359, `pr/pb-exec-inline-ffp`; from branch `pb-exec-fixes`: Burnout 3's traffic lights and headlight glows were missing) |
| 53ec467 | pb_exec: the verbose switch is read once, not for every draw | in PR (#365, `pr/pb-exec-runs`: speed only) |
| 8bfcfee | pb_exec: indices and inline vertices taken in runs | in PR (#365, with 53ec467; there without the kickoff ack and the segment test, which upstream lacks) |
| bce3f89 | vsh: a position written in part has 1 for its w | in PR (#361, `pr/vsh-position-w`: Halo's sun mask went to infinity; after #360) |
| 0ae173d | nv2a: emission and ambient from the vertex's colours | in PR (#362, `pr/ffp-color-material`: Splinter Cell's vertex-lit surfaces were dark brown) |
| 54387bf | nv2a: texture matrices in the fixed function | in PR (#363, `pr/ffp-texture-matrix`: Splinter Cell's night-vision noise did not scroll) |
| 038a69d | nv2a: reflection, normal and sphere map coordinates in the fixed function | in PR (#364, `pr/ffp-texgen`, on #363: Splinter Cell's window reflections were stuck to the glass) |
| 57390d5 | nv2a: the CPU rasteriser reads AY8 textures | in PR (#360, `pr/nv2a-ay8`: Halo's HUD and ground; before #361) |
| 05866de | nv2a: the fixed function divides by w as D3D's programs do | in PR (#372, `pr/ffp-position-rcc`: Splinter Cell's lights over fixed-function floors were dots and speckle on the CPU rasteriser) |
| 7700732 | nv2a: the CPU rasteriser samples depth textures with the shadow compare | in PR (#373, `pr/cpu-shadow-map`, one commit with 3ed29ed: Splinter Cell's shadow maps sampled white on the CPU rasteriser, so no light cast shadows) |
| 3ed29ed | nv2a: the CPU rasteriser reads a D24S8 texel as a word | in PR (#373, squashed into 7700732's commit: a D24S8 depth texture in guest memory was read as 3 bytes) |
| 52a32eb | nv2a: a depth buffer read as a colour texture gives its depth words | in PR (#374, `pr/depth-as-color`, on #373: Splinter Cell copies its shadow map as A8R8G8B8 and the CPU rasteriser read stale memory, a bright band over the top of the picture) |
| 2493e45 | nv2a: the CPU rasteriser culls faces | not sent (parked 2026-11-27; branch `cpu-light-shafts`: SET_CULL_FACE was ignored, so back faces of Splinter Cell's light shafts drew over the front; upstream candidate) |
| 16a0445 | nv2a: the CPU rasteriser interpolates colours and fog in perspective | in PR (#376, `pr/persp-color`: colours and fog were linear on the screen, so the light shafts' fade was wrong) |
| 3e02492 | nv2a: transform a batch's vertices on the raster pool | not sent (held 2026-11-27: overlaps upstream #231, the maintainer's open pipeline PR, which rewrites the same pool and drops the threshold; PR branch `pr/raster-pool-transform` 5999ce2 on v0.18.0, local; test `tests/nv2a_xf_threads`; its shared fill split also carries 586f260's fix; 7-19% faster on captured BO3, Halo and SC1 frames, same pictures) |
| 63c4054 | nv2a: wake the raster pool for runs of 256 pixels, not 8192 | not sent (held with the above: #231 drops the threshold altogether; same branch at f04beba; up to 24% faster frames on top of it, SC1's briefing unchanged) |
| 0a91c08 | nv2a: the CPU rasteriser keeps a flat triangle's depth exact | not sent (parked 2026-11-27; Burnout 3's full-screen movies at the far plane were speckled black: LEQUAL against the cleared far plane failed pixel by pixel; only those two captured frames change; upstream candidate, upstream main has the same line) |
| 2a1d4aa | nv2a: the CPU rasteriser's edge functions are computed in doubles | not sent (a floor clipped at the near plane, corners 1e5 pixels off the screen: SC1's floor detail texture came out as a grid of dots; test `tests/nv2a_far_corner`; large changes on captured frames move toward the Vulkan rasteriser, about 3% slower; upstream candidate) |
| 215a980 | nv2a: the software GPU runs 2D image blits | not sent (CopyRects and PersistDisplay's copy were dropped: SC1's briefing screen after a level load was black; test `tests/nv2a_image_blit`; no captured frame has a blit, so none changes; not yet done for the Vulkan pushbuffer back end; upstream candidate) |
| 0506b1f | nv2a: the CPU rasteriser applies polygon offset to filled triangles | in PR (#375, `pr/cpu-poly-offset`, after #373: the offset registers were never used, so shadow-map receivers met their own depths: SC1's lit floors and walls came out dotted, with no character shadows; test `tests/nv2a_poly_offset`; captured SC1 frames move toward the Vulkan rasteriser, BO3 and Halo unchanged) |
| 4419ff0 | nv2a: a trap's wait reads PGRAPH_INTR under the page's lock | not sent (fixes a57efe2: mid-step, a handler's write of 0 read as the acknowledgement and the pushbuffer resumed with the trap pending; found by `tests/pgraph_trap`; in PR #377, squashed into a57efe2's commit) |
| afe3e97 | nv2a: revert the semaphore object (9319d52, 0dbc994) | fork-only (undoes two commits that never went upstream; drop all three at the next rebuild) |
| db3d9e0 | kernel: stop with a message when a worker gets no stack | in PR (#378, with 419e0b0: a worker past the cap used to run inline and hang its caller; test `tests/kernel_thread_stack_cap`) |

## Work in flight

Branches built on this one, not merged into it yet. Each is rebased onto
`integration` when it moves and gets its rows in the table above when it
lands. It sits on the last commit before this file. Memory model 2 (`mem2`)
landed here 2026-11-20 and its branch is gone.

| Branch | Base | Where | What |
|---|---|---|---|
| `nv2a-ack-thread` | 3c0a125 | fork, no worktree | the ack thread sleeps between kickoffs (`RECOMP_ACK_SLEEP`); design in `docs/technical/nv2a-ack-thread.md` |
| `fable` | db3d9e0 | local only; Fable's `xboxrecomp-fable` worktree, Fable pins its tip `d8505db` | `irq-exclusive` plus the `jt-table-holes` fix, the two Fable needs that are not here yet. Goes away once both land; the detached line it replaced is tag `fable-old` |
| `irq-exclusive` | db3d9e0 (from ec64864, 2026-11-27) | local only, no worktree | ISRs and DPCs exclusive of running guest threads, as on the console; plan and stage 0 results in `docs/technical/interrupt-exclusivity.md`. So far: the `[IRQ]` summary lists pgraph and counts routines run beside a running guest thread; `RECOMP_IRQ_EXCLUSIVE=1` (off by default) stops the guest threads for each ISR and DPC (`a76816e`), test `tests/irq_exclusive` (`faab86e`); Fable at 30 fps either way. Upstream candidate, not proposed |
| `jt-table-holes` | db3d9e0 (from ec64864, 2026-11-27) | local only, no worktree | switch tables read past zero slots between arms (`8dd928f`, test `tools/recomp/test_jump_table_holes.py`): Fable's object-type switches lost the arms after a zero slot and crashed on a type-19 object. Changes what every title lifts at its next regen. Upstream candidate, not proposed |
| `nv2a-vk-clean` | 57390d5 | local only; worktree `xboxrecomp-nv2a-vk-clean` (detached at the tip); rewritten from `nv2a-vk`, whose last tip is tag `nv2a-vk-old` | a Vulkan rasteriser for the pushbuffer executor, to replace a Direct3D-level renderer; design and state in `docs/technical/nv2a-vulkan.md`, first title Burnout 3. 54 commits: draws, vertex programs, surfaces sampled on the GPU, frames in flight, visibility tests, its own window with a performance panel and settings menu, larger pictures, antialiasing, shaders specialised by state. The rewritten branch is tested by its suites, not yet in a title. Upstream candidate, not proposed |
| `nv2a-vk-upstream` | upstream `v0.18.0` (not this branch) | on `fork`, and its first 25 commits as `nv2a-renderer-interface`; worktree `C:/xb/wt/nv2a-vk-upstream` | the same rasteriser rebuilt on upstream main, the interface changes first and the renderer as new files only, to show the interface carries it. Done 2026-11-26. The first 25 commits (to `294d7cf`) are the batch and present interfaces, capture and replay, the frame-time log, executor fixes, and the renderer in two commits that change no file upstream owns but seven build lines. This branch's 68 commits and three put-backs sit on top (`f0b0297`), so that titles run: Third Degree Burns did. Then the timing script and two values of `RECOMP_NV2A_VK_PRESENT`, `shown` and `late` (tip `e704133`). Measured there: a back end that copies the shown picture back without waiting for the GPU runs as fast as one that presents at the title's resolution (234 against 236 frames a second), and about 9 per cent slower at scale 4 with 8 samples; what present is for is showing a picture above the title's resolution. Replayed onto `586f260` (local branch `up5-rebase`) the renderer's commits met no conflict, the interface and executor commits 13 regions. The plan page with every result (`docs/technical/nv2a-renderer-port.md`) was taken out of both branches before they were published, with the commits that only changed it; it and the old hashes are on the local branch `nv2a-port-notes`, never pushed. The first 25 commits are the upstream candidates, not proposed: the present interface's shape needs upstream's answer on #306 first |
| `park/cs-init-reset` | eafe065 (not rebased) | local only | initialising a shadowed critical section resets it; parked, it never fired on Fable's freeze after the intro movies |
