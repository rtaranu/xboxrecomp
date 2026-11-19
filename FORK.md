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
| 91589dc | nv2a: the ack loop sleeps while a flip is held (RECOMP_ACK_HOLD_SLEEP_US) | not sent |
| 55e1ef1 | nv2a: nv2a_pb_flip_hold_wait, a sleep for a held flip before KickOff | not sent |
| cb2e9c1 | apu: mcpx_apu_vp_dump, every listed voice with its last voice command | fork-only (debug) |
| 415e2e6 | kernel: RECOMP_ETH_LINK=0 reports no Ethernet link | not sent |
| d2b3860 | kernel: RECOMP_VIDEO adds letterbox, 720p and 1080i to the dashboard's video flags | not sent (on top of upstream's #320) |
| fd58f15 | kernel: AV capabilities carry the dashboard's video flags | not sent (its branch pr/av-caps-video-flags was deleted 2026-11-20: upstream #320 is in) |
| 0f16126 | video: xbox_FramebufferWindowFullscreen | not sent (display, after wave 4) |
| e6ffc52 | kernel: read-only opens use the host cache despite no-buffering | not sent (wave 5: `pr/read-only-host-cache` ready, not opened) |
| 265582a | kernel: D3D's software method 5 wakes its far-fence wait | not sent |
| f39fef3 | apu: the trace never makes the audio wait on stderr | not sent (wave 5: `pr/apu-trace-buffered` ready, not opened, with 5099ec5) |
| 663c953 | kernel: answer an announced kickoff's PFB flush at once | not sent |
| a57efe2 | nv2a: trap software methods to the title's graphics interrupt | not sent |
| ec48adb | nv2a: follow the ring's JUMP when the pushbuffer wraps | not sent (wave 5: `pr/pb-scan-wrap-jump` ready, not opened, with dbf43e0) |
| 9319d52 | nv2a: the semaphore release goes to the title's own semaphore object | not sent (wave 5: `pr/nv2a-semaphore-object` ready, not opened, with 0dbc994) |
| 8034914 | nv2a: keep the pattern object's colour register in both walks | not sent |
| 9592c2e | nv2a: keep the colour clear value in PGRAPH 0x40186C | not sent |
| 91d1974 | kernel: an event the title sets itself is an event | not sent |
| e9c84b4 | docs: the NV2A ack thread, and a design to stop it polling | not sent (goes with the ack thread work) |
| 4ac621e | apu: only a trap writes FEDECMETH/FEDECPARAM | not sent (wave 5: `pr/apu-fedec-trap-only` ready, not opened) |
| b0bdac0 | apu: count the races between a title's methods and the frame thread | fork-only (debug; goes when the races are fixed) |
| 7ac7ce8 | apu: count the sequencer holds that run out (RECOMP_APU_SEQ_HOLD_US) | not sent (candidate: a hole in the sound that nothing reported) |
| 1ee85ef | apu: RECOMP_APU_SSL_TICK=0, no sample-limit interrupt | not sent (what the hardware raises the bit for is unchecked) |
| 341a9c6 | nv2a: present a paced frame on the vblank it is due on | not sent (needs 55e1ef1; candidate once the toolkit's own present uses it) |
| 97ea06c | kernel: [MEM] reports the most a title holds at once against the console's RAM | not sent (a report only; candidate on its own) |
| 7d100c0 | kernel: [MEM] counts what the console would spend pages on | not sent (with the above) |
| 4f959e8 | nv2a: the pushbuffer scan stops at the end of the window | not sent (wave 5: `pr/pb-scan-window-end` ready, not opened, with 0abbd3f) |
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
| e15bcad | nv2a: the flip interval, capped by RECOMP_FLIP_INTERVAL_MAX or the title | not sent (builds on the flip hold, 55e1ef1 and 341a9c6; no title calls it yet) |
| 24886b6 | docs: the flip interval, the game's cap and policy and the forced override | not sent (with the above) |
| 0abbd3f | nv2a: the scan does not follow a target past the window | not sent (review fix to 4f959e8, in its PR branch) |
| dbf43e0 | nv2a: a wrap finds the ring's JUMP before running anything | not sent (review fix to ec48adb, in its PR branch) |
| 0dbc994 | nv2a: the fence mirror stays until the semaphore object takes a release | not sent (review fix to 9319d52, in its PR branch) |
| 5099ec5 | apu: a cut trace line keeps its newline; the trace test | not sent (review fix to f39fef3, in its PR branch) |
| 7133f92 | kernel: persisted contiguous memory survives the self-relaunch | not sent (upstream candidate) |
| f0684a0 | nv2a: the forced flip interval cap can be switched while running | not sent (with e15bcad; a settings panel's switch) |

## Work in flight

Branches built on this one, not merged into it yet. Each is rebased onto
`integration` when it moves and gets its rows in the table above when it
lands. It sits on the last commit before this file. Memory model 2 (`mem2`)
landed here 2026-11-20 and its branch is gone.

| Branch | Base | Where | What |
|---|---|---|---|
| `nv2a-ack-thread` | 3c0a125 | fork, no worktree | the ack thread sleeps between kickoffs (`RECOMP_ACK_SLEEP`); design in `docs/technical/nv2a-ack-thread.md` |
