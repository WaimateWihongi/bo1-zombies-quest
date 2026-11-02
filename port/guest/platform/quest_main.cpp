/* The guest's main: the Quest version of the process start-up that Windows gives WinMain.
 *
 * The working directory becomes the data folder (BO1_DATA_ROOT, where the player copied main/ and
 * zone/ from their PC install), the command line is put together, and the engine's own WinMain
 * (win32/win_main.cpp) runs from there, as on Windows.
 *
 * Command line: the text of <data folder>/bo1_args.txt if that file exists, else the game as
 * Play.bat starts it on the PC (zombies, at the main menu). The base and home paths always
 * point at the data folder. The headless server test of milestone 3 is
 *   +set bo1_zombies 1 +set bo1_headless 1 +set QuitOnError 1 +devmap zombie_theater */
#include <windows.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "win32_internal.h"

int __stdcall WinMain(HINSTANCE__ *instance, HINSTANCE__ *previous, char *command_line, int show);

static char g_data_root[MAX_PATH] = ".";
static char g_command_line[1024];

const char *bo1_data_root(void) { return g_data_root; }
char *bo1_command_line(void) { return g_command_line; }

void bo1_log(const char *format, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line, sizeof line - 1, format, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    if (!n || line[n - 1] != '\n') line[n++] = '\n';
    /* one write per line: the host logs each stderr write as it arrives */
    (void)!write(2, line, (size_t)n);
}

static const char DEFAULT_COMMANDS[] = "+set bo1_zombies 1 +set logfile 2 +set net_ip 127.0.0.1 +set gpad_enabled 1 +set r_depthPrepass 0 +set r_aaSamples 1 +set r_dof_enable 0 +set r_motionblur_enable 0 +set ui_skipMainLockout 1 +set cg_drawVersion 0";

static void read_args_file(char *out, size_t size)
{
    char path[MAX_PATH + 16];
    snprintf(path, sizeof path, "%s/bo1_args.txt", g_data_root);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(out, size, "%s", DEFAULT_COMMANDS);
        return;
    }
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    for (char *p = out; *p; p++)
        if (*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
    bo1_log("command line from %s", path);
}

int main(int argc, char **argv)
{
    const char *root = getenv("BO1_DATA_ROOT");
    if (root && *root) snprintf(g_data_root, sizeof g_data_root, "%s", root);
    if (chdir(g_data_root)) bo1_log("can't enter the data folder %s: %s", g_data_root, strerror(errno));

    char commands[768];
    if (argc > 1) {
        /* commands passed by the host take the place of the file */
        size_t used = 0;
        commands[0] = 0;
        for (int i = 1; i < argc && used < sizeof commands - 2; i++)
            used += (size_t)snprintf(commands + used, sizeof commands - used, "%s%s", i > 1 ? " " : "", argv[i]);
    } else {
        read_args_file(commands, sizeof commands);
    }
    snprintf(g_command_line, sizeof g_command_line, "+set fs_b \"%s\" +set fs_h \"%s\" %s", g_data_root, g_data_root,
             commands);
    bo1_log("BO1 guest: data folder %s", g_data_root);
    bo1_log("BO1 guest: command line %s", g_command_line);

    char *command_line = strdup(g_command_line);
    return WinMain((HINSTANCE__ *)GetModuleHandleA(NULL), NULL, command_line, 1);
}

/* ---- runtime pieces the compiler expects ----------------------------------------------------- */

extern "C" {
/* Apple's libc call, which clang emits for some 16-byte pattern fills on the watchOS-derived target */
void memset_pattern16(void *destination, const void *pattern, size_t size)
{
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *p = (const unsigned char *)pattern;
    for (size_t i = 0; i < size; i++) d[i] = p[i & 15];
}

/* Android's system properties: none in the guest */
int __system_property_get(const char *, char *value)
{
    value[0] = 0;
    return 0;
}
}
