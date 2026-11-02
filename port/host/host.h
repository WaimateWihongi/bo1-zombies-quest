#ifndef HOST_H
#define HOST_H

#include <stddef.h>
#include <stdint.h>
#include "bo1_abi.h"

#define HOST_TAG "BO1"

/* log */
void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define host_info(...) host_logf(4, __VA_ARGS__)
#define host_warn(...) host_logf(5, __VA_ARGS__)
#define host_error(...) host_logf(6, __VA_ARGS__)
_Noreturn void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));
void host_log_guest_write(int fd, const char *data, size_t size);

/* low memory: everything the guest can address lives below 4 GB */
int host_memory_initialize(void);
uint64_t host_memory_place_image(uint64_t size, uint64_t align); /* 0 when nothing fits */
void *host_low_map(size_t size, int prot);
void host_low_unmap(void *address, size_t size);
long long host_guest_mmap(uint64_t address, uint64_t size, int prot, int flags, int fd, int64_t offset);
long long host_guest_munmap(uint64_t address, uint64_t size);
long long host_guest_mprotect(uint64_t address, uint64_t size, int prot);
#define GUEST_PTR(type, value) ((type)(uintptr_t)(uint32_t)(value))
/* a guest path for the host's syscalls: '/' separators, and case-insensitive where it doesn't match */
const char *host_guest_path(int dirfd, uint64_t guest, char *out, size_t size);

/* the guest image */
struct host_image {
    const struct bo1_guest_header *header;
    uint32_t base, end;
};
extern struct host_image host_image;
int host_load_image(const void *elf, size_t size);
void (*host_resolve_import(const char *name))(void);

/* crash and hang backtraces in logcat (host_crash.c) */
void host_crash_initialize(void);

/* input (host_input.c) */
struct android_app;
typedef struct AInputEvent AInputEvent;
int32_t host_on_input(struct android_app *app, AInputEvent *event);
void host_input_pointer(float u, float v); /* 0..1 across and down the screen */
void host_input_buttons(int buttons);      /* bit 0 primary */
void host_input_key(int android_key, int down);
void host_input_scroll(float notches);
void host_input_xr_pad(const struct bo1_gamepad *pad); /* the controllers as a gamepad, in game */

/* OpenXR (host_xr.c) */
int host_xr_initialize(struct android_app *app); /* main thread, at start-up; 0 when there is a headset */
int host_xr_available(void);
int host_xr_active(void);                        /* a session exists */
int host_xr_start(void *display, void *config, void *context); /* render thread, context current */
uint32_t host_xr_frame_begin(uint32_t size[3]);
void host_xr_frame_end(void);
uint32_t host_xr_views(uint32_t guest_views);           /* game thread: a stereo pair's eyes */
void host_xr_set_eye(int32_t eye, uint32_t guest_views); /* render thread: the frame about to be presented */
uint32_t host_xr_panel_begin(uint32_t guest_size);      /* render thread: the 2D panel's image */
void host_xr_panel_end(void);
void host_xr_pointer_mode(int32_t mode);                /* 1: an in-world screen's menu has focus */

/* audio output (host_audio.c) */
int32_t host_audio_open(uint32_t rate);
uint32_t host_audio_write(uint32_t guest_frames, uint32_t frames);
uint32_t host_audio_queued(void);
uint32_t host_wma_decode(uint32_t guest_in, uint32_t in_bytes, uint32_t channels, uint32_t rate, uint32_t block_align,
                         uint32_t avg_bytes_per_sec, uint32_t guest_out, uint32_t out_bytes); /* host_wma.c */

/* OpenGL ES (host_gl.c) */
struct ANativeWindow;
void host_gl_set_window(struct ANativeWindow *window);

/* threads */
void host_enter_guest(uint32_t entry, uint32_t arg);
uint64_t host_call_guest(uint32_t function, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
int host_native_thread_create(void (*entry)(void *), void *arg, size_t stack_size);
void host_thread_initialize(void);

#endif
