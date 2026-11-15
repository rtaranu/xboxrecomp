/**
 * Shared FMV player: Media Foundation decode onto a D3D8 texture.
 *
 * For a title whose video is a container Windows can already decode, the
 * decoder does not have to be emulated for the video to be watchable. The
 * recompiled title still decides when: it opens the file as its own logic
 * dictates, and the runtime plays the file it opened.
 */

#ifndef XBOXRECOMP_VIDEO_PLAYER_H
#define XBOXRECOMP_VIDEO_PLAYER_H

#include <stdint.h>

/* Initialize/shutdown Media Foundation (call once at app start/end) */
int  video_init(void);
void video_shutdown(void);

/* Open a video file for playback. Returns 0 on success. */
int  video_open(const char *path);

/* Advance playback by dt seconds, decode next frame if needed.
 * Returns 1 if a new frame is ready, 0 if unchanged, -1 if finished. */
int  video_update(float dt);

/* Render the current video frame as a fullscreen quad.
 * Call between BeginScene/EndScene. */
void video_render(void);

/* Check if the video has finished playing. */
int  video_is_finished(void);

/* Close the current video and release resources. */
void video_close(void);

/* === Boot sequence state machine === */

/* Boot phases */

/* Get current boot phase */

/* Advance boot state machine. Call once per frame.
 * skip=1 if user pressed a button to skip current video.
 * Returns the new phase. */

/* Render current boot phase (video frame or press-start screen). */


/* Play one video file, start to finish, in its own window on its own thread.
 * Returns 0 if the pump started. Blocking work happens on that thread, so the
 * guest carries on running while the video plays. */
int  xbox_VideoPlayFile(const char *host_path);
int  xbox_VideoIsPlaying(void);

/* Write the current decoded frame to a 24-bit BMP -- evidence that real
 * pixels reached the renderer, which a log line cannot give. */
int  video_dump_frame_bmp(const char *path);

/* Show the guest framebuffer in its own window (RECOMP_FB_WINDOW). Whatever
 * the title renders into guest RAM appears there; nothing else scans it out. */
void xbox_FramebufferWindowStart(void);
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
/* Rows per displayed row: 2 for a 2x2 anti-aliased surface, else 1. */
void xbox_FramebufferWindowSetAA(uint32_t sy);
int  xbox_FramebufferDumpBmp(const char *path);
/* Title bar: "<XBE title> | FPS: n | draws: n". The name is the certificate's
 * UTF-16 title (40 chars max); the stats come from each flip. */
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars);
void xbox_FramebufferWindowFrameStats(uint32_t draws);
/* For a host renderer presenting to the window itself: the HWND (NULL until
 * the window exists), and a switch that stops the window drawing guest
 * memory; it still pumps messages and updates the title bar. The toolkit's own
 * says 2 until it presents: the window draws, but not with D3D11, which would
 * keep it for itself; then 3, and it draws the video overlay too. */
void *xbox_FramebufferWindowHandle(void);
void  xbox_FramebufferWindowSetExternal(int on);
/* For 3. Whether the video overlay is to be drawn: 0 no, 1 as it was, 2 there is
 * a new frame, or it is no longer there. And draws it, over 640x480 pixels,
 * 0xAARRGGBB; its new frame's memory is the title's again then. One thread. */
int   xbox_FramebufferOverlayState(void);
void  xbox_FramebufferOverlay(uint32_t *rgb);
/* Borderless fullscreen (F11, Alt+Enter): on 1 or 0 asks for it, -1 only
 * asks. Returns whether the window is fullscreen now (a request lands on the
 * window's thread, a moment later). */
int   xbox_FramebufferWindowFullscreen(int on);

#if !defined(_WIN32)
/* POSIX hosts: the framebuffer window needs the process main thread (AppKit
 * on macOS), so a title that wants it calls this from main() with its boot
 * sequence. With RECOMP_FB_WINDOW set it opens the window, runs guest_main on
 * a pthread and pumps events until guest_main returns (its value is returned)
 * or the window is closed (the process exits). Otherwise, or headless, it
 * just calls guest_main. */
int  xbox_HostWindowMain(int (*guest_main)(void));
#endif

#endif /* BURNOUT3_VIDEO_PLAYER_H */
