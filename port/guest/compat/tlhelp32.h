/* thread enumeration for the hang detector; the guest's snapshot comes back empty */
#pragma once
#include <windows.h>

#define TH32CS_SNAPTHREAD 0x00000004
typedef struct tagTHREADENTRY32 {
    DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID;
    LONG tpBasePri, tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32, *LPTHREADENTRY32;
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD process);
BOOL Thread32First(HANDLE snapshot, LPTHREADENTRY32 entry);
BOOL Thread32Next(HANDLE snapshot, LPTHREADENTRY32 entry);

/* module enumeration (Sys_IsGameProcess); also empty on the guest */
#define TH32CS_SNAPMODULE 0x00000008
typedef struct tagMODULEENTRY32 {
    DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage;
    BYTE *modBaseAddr;
    DWORD modBaseSize;
    HMODULE hModule;
    char szModule[256];
    char szExePath[MAX_PATH];
} MODULEENTRY32, *LPMODULEENTRY32;
BOOL Module32First(HANDLE snapshot, LPMODULEENTRY32 entry);
BOOL Module32Next(HANDLE snapshot, LPMODULEENTRY32 entry);
