/* Shared by the guest platform layer's files (port/guest/platform). */
#pragma once
#include <windows.h>
#include <io.h>

/* logcat, through stderr (the host tags it BO1) */
void bo1_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* the folder holding the player's game files (BO1_DATA_ROOT), and the engine's command line */
const char *bo1_data_root(void);
char *bo1_command_line(void);

static inline void bo1_find_to_crt(const WIN32_FIND_DATAA *found, struct _finddata64i32_t *data)
{
    data->attrib = found->dwFileAttributes & 0x37;
    if (found->dwFileAttributes == FILE_ATTRIBUTE_NORMAL) data->attrib = _A_NORMAL;
    const long long unix_epoch = 116444736000000000ll;
    data->time_create = data->time_access = data->time_write =
        (long long)(((unsigned long long)found->ftLastWriteTime.dwHighDateTime << 32 |
                     found->ftLastWriteTime.dwLowDateTime) - unix_epoch) / 10000000;
    data->size = found->nFileSizeLow;
    strncpy(data->name, found->cFileName, sizeof data->name - 1);
    data->name[sizeof data->name - 1] = 0;
}
