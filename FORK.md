# This fork

[sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) with the
changes its recompiled games need, kept on top of upstream until upstream
takes them. This file is fork-only and never goes upstream.

## Branches

| Branch | What it is |
|---|---|
| `main` | Upstream's `main`, unchanged. Nothing is committed here; it only fast-forwards. |
| `splintercell` | The integration branch: `main` plus every change not upstream yet. The games pin commits on it (`TOOLKIT_COMMIT`). The name is historical: all the games use it. |
| `pr/<name>` | One upstream pull request each, cut from `main`, usually with a test the integration commit lacks. Rebased and force-pushed freely. |
| `integration-<date>` (tags) | The integration branch's tip before each rebuild, so a game pinned to an older commit can still fetch it. |

## Working rules

- New work starts on `splintercell` (or a topic branch on it), where the
  games can test it. One concern per commit, so it cherry-picks cleanly onto
  a `pr/<name>` branch from `main`.
- When upstream merges anything, rebuild the integration branch with
  `tools/fork/rebuild-integration.sh`: it fast-forwards `main`, tags the old
  tip, rebases onto `main` (dropping what upstream took) and prints the
  `range-diff` to review and the push commands. It never pushes.
- Before force-pushing the rebuilt branch, push its tag: older pins must
  stay reachable on GitHub.
- Keep the table below current: a row per commit, updated when a PR opens,
  merges or closes. A commit upstream took leaves the table at the next
  rebuild.

## Status of each commit

Against upstream `main` 343b6d8 (v0.17.0), 2026-11-19, oldest first.
"In PR" is open on upstream; "not sent" is meant for upstream but has no PR
yet; "fork-only" stays here. Not listed: the commits adding this file and
`tools/fork/` (fork-only).

| Commit | Change | Status |
|---|---|---|
| 1d2f081 | kernel: 64 KB-align kernel-chosen NtAllocateVirtualMemory addresses | in PR #331 |
| c5b273b | disasm: seed entries can mark an address "not_function" | in PR #339 (stacked on #333) |
| 9775f44 | nv2a_pb_exec: a colour clear marks its surface as drawn | in PR #332 |
| 7385421 | C++ exceptions: dispatch MSVC throws to their catch blocks | not sent (wave 3: announce first, likely behind a switch) |
| 6ac6aa0 | kernel: relaunch the process when the title launches its own XBE | not sent (wave 2) |
| aebec82 | nv2a_pb_exec: a switch to draw nothing, for a host renderer | not sent (wave 4: issue #312 first) |
| 061e38a | fb_present: let a host renderer present to the window | not sent (wave 4) |
| aa2152d | ohci: a heap block the driver named is RAM, not the window | evidence for upstream PR #286, not its own PR |
| 5094175 | apu: RECOMP_APU_AUDIT lists the voices periodically | fork-only (debug) |
| 7acf8aa | xaudio2: RECOMP_AUDIO_MUTE=1 silences the output | not sent (optional debug tool) |
| f2109c3 | usb: part-throw sticks in pad scripts | fork-only (debug) |
| 192ae05 | disasm: observed seeds are part of the cache key | in PR #333 |
| 4c74bc6 | heap: a merged-away slot keeps its address | in PR #330 |
| 6be5d31 | input: an overlay on the window can hold the keyboard and mouse | not sent (with wave 4) |
| 91dc5bd | apu: RECOMP_APU_SSL_LOG times a stream voice's segment lists | fork-only (debug) |
| 8291ac0 | apu: RECOMP_APU_SSL_ADVANCE passes a stream voice through an empty list | in PR #342 |
| 76f9a7d | nv2a: the ack loop sleeps while a flip is held (RECOMP_ACK_HOLD_SLEEP_US) | not sent |
| 5ccccd1 | nv2a: nv2a_pb_flip_hold_wait, a sleep for a held flip before KickOff | not sent |
| 6bfab77 | apu: [SSL] log lines for an idle-voice trap and the front end resuming | fork-only (debug) |
| dfff96e | apu: pace the voice processor one frame at a time (RECOMP_APU_PACE_FRAMES) | in PR #341 |
| 4e8ce1b | apu: report a stretch of over 20 ms without a voice-processor frame | in PR #341 |
| e546a7c | apu: voice_set_mask writes its field atomically | in PR #334 |
| 41c67ad | apu: mcpx_apu_vp_dump, every listed voice with its last voice command | fork-only (debug) |
| 698c6b7 | kernel: RECOMP_ETH_LINK=0 reports no Ethernet link | not sent |
| f8dda0e | kernel: RECOMP_VIDEO sets the dashboard's video flags | not sent (display, after b772667) |
| b772667 | kernel: AV capabilities carry the dashboard's video flags | not sent (local branch pr/av-caps-video-flags, waits for upstream #320) |
| 62ea9e0 | video: xbox_FramebufferWindowFullscreen | not sent (display, after wave 4) |
| 8777e97 | disasm: measure a jump table indexed from slot 1 | in PR #338 |
| c53b0c6 | kernel: keep the heap block table in address order when a slot is reused | in PR #330 (follow-up commit) |
| b612572 | apu: idle-voice traps take turns instead of starting at the list head | in PR #340 |
| df1fd21 | kernel: a new contiguous block gets the protection asked for | in PR #337 |
| f3853e3 | kernel: make reused contiguous pages writable before zeroing them | in PR #337 |
| 67e65ff | kernel: read-only opens use the host cache despite no-buffering | not sent |
| c94133e | kernel: D3D's software method 5 wakes its far-fence wait | not sent |
| 6d1cf33 | nv2a: draw vertices whose program w is +inf | in PR #343 |
| 5fbe182 | nv2a: an infinite fog distance follows the hardware | in PR #343 |
| c706108 | apu: the trace never makes the audio wait on stderr | not sent |
| 53c47c8 | kernel: answer an announced kickoff's PFB flush at once | not sent |
| 856ed5c | docs: RECOMP_KICK_SPIN_US | not sent (with 53c47c8) |
| 77b8cc1 | kernel: MmQueryStatistics reports the RAM really left | not sent |
| cd5e02d | nv2a: trap software methods to the title's graphics interrupt | not sent |
| c2fc4dd | nv2a: hold the pushbuffer until the trap handler reopens the FIFO | not sent (with cd5e02d) |
| 90c9a16 | nv2a: follow the ring's JUMP when the pushbuffer wraps | not sent |
| 7ca8b53 | Merge the software-method trap and the ring's JUMP | merge commit; a rebuild flattens it |
| 8ebaca8 | nv2a: the semaphore release goes to the title's own semaphore object | not sent |
| d907e01 | nv2a: keep the pattern object's colour register in both walks | not sent |
| 9810c8c | nv2a: keep the colour clear value in PGRAPH 0x40186C | not sent |
| e640319 | kernel: an event the title sets itself is an event | not sent |
| 68a3a89 | docs: the NV2A ack thread, and a design to stop it polling | not sent (goes with the ack thread work) |
| 6a0aaa7 | docs: NV2A ack thread design, gaps a review found | not sent (with the above) |
| d01b97a | docs: NV2A ack thread, what to measure before building | not sent (with the above) |
| 283412b | apu: only a trap writes FEDECMETH/FEDECPARAM | not sent (a crash fix; candidate on its own) |
| 307ea96 | apu: count the races between a title's methods and the frame thread | fork-only (debug; goes when the races are fixed) |
| cf39032 | apu: the [SSL] log shows each sequencer control write | fork-only (debug) |
| f85971b | apu: count the sequencer holds that run out (RECOMP_APU_SEQ_HOLD_US) | not sent (candidate: a hole in the sound that nothing reported) |
| e641c9e | apu: RECOMP_APU_SSL_TICK=0, no sample-limit interrupt | not sent (what the hardware raises the bit for is unchecked) |
| d0d502b | kernel: an image page the title asks the physical address of is RAM | not sent (candidate; mem2 may make it moot) |

## Work in flight

Branches built on this one, not merged into it yet. Each is rebased onto
`splintercell` when it moves and gets its rows in the table above when it
lands.

| Branch | Base | Where | What |
|---|---|---|---|
| `nv2a-ack-thread` | d01b97a | fork, worktree `xboxrecomp-ack` | the ack thread sleeps between kickoffs (`RECOMP_ACK_SLEEP`); design in `docs/technical/nv2a-ack-thread.md` |
| `mem2` | d5098a4 | fork, worktree `xboxrecomp-mem` | memory model 2 behind `RECOMP_MEM_MODEL=2`; design in `docs/technical/memory-experiment.md` |

`mem2`, oldest first:

| Commit | Change | Note |
|---|---|---|
| 8a4e9cc | kernel: [MEM] reports the most a title holds at once against the console's RAM | a report only; upstream candidate on its own |
| 806a504 | kernel: [MEM] counts what the console would spend pages on | with the above |
| 4ff42a8 | kernel: RECOMP_MIRRORS=0 leaves the RAM mirrors unmapped | opt-in; upstream candidate on its own |
| dbdd8b1 | kernel: memory model 2, in a file of its own (not called yet) | |
| c138e94 | kernel: RECOMP_MEM_MODEL=2 turns on memory model 2 | the hooks; unset = classic model |
| a6e1962 | tests: memory model 2 beside the classic model | |
| 46f8c03 | docs: memory model 2, the design and how the branch is laid out | |
| 648d141 | nv2a: the pushbuffer scan stops at the end of the window | a fix any layout can hit |
| 7bf34be | kernel: memory model 2 fills the contiguous window from the top | |
| af8325c | docs: memory model 2, the window fills from the top | |
| b6efa52 | kernel: memory model 2 ignores RECOMP_CONTIG_AVOID_IMAGE when filling from the top | |
| a179211 | docs: memory model 2 ignores RECOMP_CONTIG_AVOID_IMAGE, play sessions | |
| 91a1fee | docs: memory model 2, the first branch is gone | |
| 0b3d7c3 | docs: memory model 2 with the ack thread, on Halo | |
