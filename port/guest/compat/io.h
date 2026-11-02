/* the MSVC CRT directory search Sys_ListFiles uses, implemented on opendir by the guest runtime */
#pragma once
#include <windows.h>
#include <unistd.h>
#include <sys/stat.h>

#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20
struct _finddata64i32_t {
    unsigned attrib;
    long long time_create, time_access, time_write;
    unsigned long size;
    char name[260];
};
#ifdef __cplusplus
extern "C" {
#endif
intptr_t _findfirst64i32(const char *pattern, struct _finddata64i32_t *data);
int _findnext64i32(intptr_t handle, struct _finddata64i32_t *data);
int _findclose(intptr_t handle);
#ifdef __cplusplus
}
#endif
