/* Android entry (NativeActivity): reserve the guest's address space, load assets/bo1_guest.elf,
   and run the guest's main on a thread with a stack below 4 GB. */
#include "host.h"
#include "android_native_app_glue.h"

#include <android/asset_manager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_MAIN_STACK (16 * 1024 * 1024)

/* Copies a string into guest memory. */
static uint32_t guest_string(char **cursor, const char *s)
{
    size_t n = strlen(s) + 1;
    memcpy(*cursor, s, n);
    uint32_t address = (uint32_t)(uintptr_t)*cursor;
    *cursor += n;
    return address;
}

static struct bo1_guest_boot *make_boot(const char *const *argv, const char *const *envp)
{
    char *page = host_low_map(64 * 1024, PROT_READ | PROT_WRITE);
    if (!page) return NULL;
    struct bo1_guest_boot *boot = (void *)page;
    uint32_t *vectors = (uint32_t *)(page + sizeof *boot);
    int argc = 0, envc = 0;
    while (argv[argc]) argc++;
    while (envp[envc]) envc++;
    char *strings = (char *)(vectors + argc + 1 + envc + 1);

    boot->argc = (uint32_t)argc;
    boot->argv = (uint32_t)(uintptr_t)vectors;
    for (int i = 0; i < argc; i++) *vectors++ = guest_string(&strings, argv[i]);
    *vectors++ = 0;
    boot->envp = (uint32_t)(uintptr_t)vectors;
    for (int i = 0; i < envc; i++) *vectors++ = guest_string(&strings, envp[i]);
    *vectors = 0;
    boot->page_size = (uint32_t)sysconf(_SC_PAGESIZE);
    return boot;
}

static void guest_main(void *boot)
{
    host_enter_guest(host_image.header->start, (uint32_t)(uintptr_t)boot);
}

static int start_guest(struct android_app *app)
{
    host_thread_initialize();
    if (host_memory_initialize()) return -1;

    AAsset *asset = AAssetManager_open(app->activity->assetManager, "bo1_guest.elf", AASSET_MODE_BUFFER);
    if (!asset) {
        host_error("assets/bo1_guest.elf is missing from the APK");
        return -1;
    }
    int r = host_load_image(AAsset_getBuffer(asset), (size_t)AAsset_getLength(asset));
    AAsset_close(asset);
    if (r) return -1;
    host_crash_initialize();

    const char *data = app->activity->externalDataPath ? app->activity->externalDataPath
                                                       : app->activity->internalDataPath;
    char data_env[512], home_env[512];
    snprintf(data_env, sizeof data_env, "BO1_DATA_ROOT=%s", data);
    snprintf(home_env, sizeof home_env, "HOME=%s", app->activity->internalDataPath);
    const char *argv[] = {"bo1", NULL};
    const char *envp[] = {data_env, home_env, "TZ=UTC", NULL};
    struct bo1_guest_boot *boot = make_boot(argv, envp);
    if (!boot) return -1;
    host_info("data folder: %s", data);
    return host_native_thread_create(guest_main, boot, GUEST_MAIN_STACK) ? -1 : 0;
}

/* the window comes and goes with the activity; the guest's renderer draws into it (host_gl.c) */
static void on_app_command(struct android_app *app, int32_t command)
{
    if (command == APP_CMD_INIT_WINDOW) host_gl_set_window(app->window);
    else if (command == APP_CMD_TERM_WINDOW) host_gl_set_window(NULL);
}

void android_main(struct android_app *app)
{
    host_info("BO1 Quest host starting");
    app->onAppCmd = on_app_command;
    app->onInputEvent = host_on_input;
    if (host_xr_initialize(app)) host_warn("xr: no OpenXR runtime; the flat panel only");
    if (start_guest(app)) host_error("the guest could not start");

    while (!app->destroyRequested) {
        int events;
        struct android_poll_source *source;
        if (ALooper_pollOnce(-1, NULL, &events, (void **)&source) >= 0 && source)
            source->process(app, source);
    }
    host_info("activity destroyed");
    exit(0);
}
