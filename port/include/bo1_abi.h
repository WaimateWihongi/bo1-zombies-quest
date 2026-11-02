/* Shared by the 64-bit host (libmain.so) and the 32-bit guest (bo1_guest.elf).
 *
 * The guest is ILP32 AArch64 code living below 4 GB in the host's own address space, so guest
 * pointers are valid host pointers once zero-extended. Structs that cross the boundary use
 * fixed-width members only; a host function taking a guest size_t declares it uint32_t. */
#ifndef BO1_ABI_H
#define BO1_ABI_H

#include <stdint.h>

/* The address guest.ld links the image at. The host loads it wherever the low 4 GB has room
   (ART's heaps and JIT cache sit in different places on the Quest and the emulator) and moves
   its pointer words by the difference, from the .guest_relocs table (port/tools/guest_relocs.py). */
#define BO1_GUEST_LINK_BASE 0x80000000u
#define BO1_GUEST_IMAGE_ALIGN 0x10000u /* load addresses are multiples of this */
#define BO1_GUEST_MAGIC 0x47314f42u       /* 'BO1G' */
#define BO1_GUEST_ABI_VERSION 1u

/* At the start of the image; every field a guest address or count. */
struct bo1_guest_header {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t image_end;
    uint32_t import_table;  /* uint64_t[import_count], filled by the host loader */
    uint32_t import_names;  /* NUL-separated names, in table order */
    uint32_t import_count;  /* address of the uint32_t count (a link-time value) */
    uint32_t start;         /* void __guest_start(struct bo1_guest_boot *) */
    uint32_t thread_start;  /* void __guest_thread_start(struct guest thread *) */
    uint32_t thread_attach; /* void __guest_thread_attach(void): gives a host thread a guest thread */
};

/* Built by the host in guest memory before __guest_start. */
struct bo1_guest_boot {
    uint32_t argc;
    uint32_t argv; /* char *[argc + 1] */
    uint32_t envp; /* char *[], NULL-terminated */
    uint32_t page_size;
};

/* Input from Android, queued by the host (host_input.c) and taken by the guest each frame
   (platform/quest_input.cpp): host_input_poll(struct bo1_input_event *events, uint32_t max). */
enum {
    BO1_INPUT_KEY = 1,     /* a = Android keycode, b = 1 down / 0 up, c = Android meta state */
    BO1_INPUT_POINTER = 2, /* a, b = position across the window, 0..65535 */
    BO1_INPUT_BUTTONS = 3, /* a = pointer buttons held: bit 0 primary, 1 secondary, 2 middle */
    BO1_INPUT_SCROLL = 4,  /* a = wheel clicks up, times 120 (Windows' WHEEL_DELTA); negative = down */
};
struct bo1_input_event {
    uint32_t type;
    int32_t a, b, c;
};

/* The newest gamepad state, XInput's layout (host_input_gamepad(struct bo1_gamepad *)) */
struct bo1_gamepad {
    uint32_t connected; /* 0 until a gamepad has been seen */
    uint32_t packet;    /* changes whenever the state does */
    uint16_t buttons;   /* XINPUT_GAMEPAD_* bits */
    uint8_t left_trigger, right_trigger;
    int16_t left_x, left_y, right_x, right_y; /* y up is positive, as in XInput */
};

/* The headset's eyes for one stereo frame (host_xr_views / host_xr_set_eye). Poses are in the
   LOCAL reference space (metres; x right, y up, -z ahead; origin where the head was when the
   session began); fov holds the eye's frustum angles in radians: left (< 0), right, up, down (< 0). */
struct bo1_xr_eye {
    float orientation[4]; /* x, y, z, w */
    float position[3];
    float fov[4];
};
struct bo1_xr_views {
    uint32_t valid;
    struct bo1_xr_eye eye[2];
    uint32_t aim_valid;      /* the right controller's aim ray, LOCAL space, at the same time as the eyes */
    float aim_position[3];
    float aim_forward[3];
    float aim_orientation[4]; /* x, y, z, w */
    uint32_t left_valid;      /* the left controller's aim pose */
    float left_position[3];
    float left_orientation[4];
};

_Static_assert(sizeof(struct bo1_guest_header) == 36, "bo1_guest_header layout");
_Static_assert(sizeof(struct bo1_input_event) == 16, "bo1_input_event layout");
_Static_assert(sizeof(struct bo1_gamepad) == 20, "bo1_gamepad layout");
_Static_assert(sizeof(struct bo1_guest_boot) == 16, "bo1_guest_boot layout");
_Static_assert(sizeof(struct bo1_xr_views) == 168, "bo1_xr_views layout");

#endif
