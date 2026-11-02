/* The subset of the Win32 API the engine uses, for the guest (ILP32, so LONG/DWORD are 32-bit as
   on Windows). Types and declarations only; the implementations on POSIX come with the platform
   layer (milestone 3). Grown as the engine's files need it. Windows.h is the same file on a
   case-insensitive filesystem; build.py adds the alias where case matters. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <intrin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINAPI
#define APIENTRY
#define CALLBACK
#define __stdcall
#define __cdecl
#define __fastcall
#define __thiscall
#define WINBASEAPI
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#define FORCEINLINE inline __attribute__((always_inline))
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

typedef int BOOL;
typedef unsigned char BYTE, UCHAR, BOOLEAN;
typedef unsigned short WORD, USHORT;
typedef short SHORT;
typedef int INT;
typedef unsigned int UINT, UINT32;
typedef long LONG;
typedef unsigned long ULONG, DWORD;
typedef long long LONGLONG, LONG64, INT64;
typedef unsigned long long ULONGLONG, ULONG64, DWORD64, UINT64, *PDWORD64;
typedef float FLOAT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef void VOID, *PVOID, *LPVOID, *HANDLE, *HGLOBAL;
/* distinct handle types, named as MSVC's headers name them (decompiled code spells HWND__ *) */
#define BO1_DECLARE_HANDLE(name) typedef struct name##__ { int unused; } *name
BO1_DECLARE_HANDLE(HWND);
BO1_DECLARE_HANDLE(HINSTANCE);
BO1_DECLARE_HANDLE(HDC);
BO1_DECLARE_HANDLE(HFONT);
BO1_DECLARE_HANDLE(HKEY);
BO1_DECLARE_HANDLE(HMONITOR);
BO1_DECLARE_HANDLE(HICON);
BO1_DECLARE_HANDLE(HBRUSH);
BO1_DECLARE_HANDLE(HMENU);
typedef HINSTANCE HMODULE;
typedef HICON HCURSOR;
typedef const void *LPCVOID;
typedef char *LPSTR, *PSTR;
typedef const char *LPCSTR, *PCSTR;
typedef wchar_t *LPWSTR;
typedef const wchar_t *LPCWSTR;
typedef DWORD *LPDWORD, *PDWORD;
typedef LONG *PLONG, *LPLONG;
typedef BYTE *LPBYTE, *PBYTE;
typedef BOOL *LPBOOL;
typedef uintptr_t ULONG_PTR, DWORD_PTR, UINT_PTR, SIZE_T, WPARAM, *PDWORD_PTR;
typedef intptr_t LONG_PTR, INT_PTR, SSIZE_T, LPARAM, LRESULT;
typedef LONG HRESULT;
typedef DWORD COLORREF;
typedef WORD ATOM;

#define TRUE 1
#define FALSE 0
#define INFINITE 0xffffffffu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define MAKEFOURCC(a, b, c, d) ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD_PTR)(l) >> 16))
#define LOBYTE(w) ((BYTE)((DWORD_PTR)(w) & 0xff))
#define HIBYTE(w) ((BYTE)((DWORD_PTR)(w) >> 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define UNREFERENCED_PARAMETER(p) (void)(p)
#define ZeroMemory(p, n) memset((p), 0, (n))
#define CopyMemory(d, s, n) memcpy((d), (s), (n))
#define FillMemory(d, n, v) memset((d), (v), (n))

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT, *LPRECT;

typedef struct tagPOINT {
    LONG x, y;
} POINT, *LPPOINT;

typedef struct _GUID {
    DWORD Data1;
    WORD Data2;
    WORD Data3;
    BYTE Data4[8];
} GUID, IID, CLSID;
#ifdef __cplusplus
#define REFGUID const GUID &
#define REFIID const IID &
#else
#define REFGUID const GUID *
#define REFIID const IID *
#endif

/* critical sections: a recursive mutex underneath */
typedef struct _RTL_CRITICAL_SECTION {
    void *impl;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;

void InitializeCriticalSection(LPCRITICAL_SECTION cs);
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);

/* time */
void Sleep(DWORD ms);
DWORD SleepEx(DWORD ms, BOOL alertable);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
DWORD timeGetTime(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER *count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);
void GetSystemTime(LPSYSTEMTIME t);
void GetLocalTime(LPSYSTEMTIME t);
void GetSystemTimeAsFileTime(LPFILETIME t);

/* threads and synchronisation */
typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
typedef struct _SECURITY_ATTRIBUTES *LPSECURITY_ATTRIBUTES;
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xffffffffu
#define CREATE_SUSPENDED 0x4
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#define HIGH_PRIORITY_CLASS 0x00000080
#define NORMAL_PRIORITY_CLASS 0x00000020
DWORD GetPriorityClass(HANDLE process);
BOOL SetPriorityClass(HANDLE process, DWORD priority_class);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID arg,
                    DWORD flags, LPDWORD id);
DWORD ResumeThread(HANDLE thread);
DWORD SuspendThread(HANDLE thread);
BOOL SetThreadPriority(HANDLE thread, int priority);
int GetThreadPriority(HANDLE thread);
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
DWORD SetThreadIdealProcessor(HANDLE thread, DWORD processor);
BOOL GetExitCodeThread(HANDLE thread, LPDWORD code);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
BOOL CloseHandle(HANDLE h);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual_reset, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCSTR name);
BOOL ReleaseMutex(HANDLE mutex);
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name);
BOOL ReleaseSemaphore(HANDLE semaphore, LONG count, LPLONG previous);
BOOL PulseEvent(HANDLE event);
BOOL SwitchToThread(void);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD ms);
#define CreateEvent CreateEventA
#define CreateMutex CreateMutexA
#define CreateSemaphore CreateSemaphoreA
DWORD TlsAlloc(void);
BOOL TlsFree(DWORD index);
LPVOID TlsGetValue(DWORD index);
BOOL TlsSetValue(DWORD index, LPVOID value);
#define TLS_OUT_OF_INDEXES 0xffffffffu

/* interlocked operations: type-generic macros in intrin.h */

/* memory */
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_TOP_DOWN 0x100000
#define MEM_LARGE_PAGES 0x20000000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_GUARD 0x100
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old);
typedef struct _MEMORYSTATUS {
    DWORD dwLength, dwMemoryLoad;
    SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;
void GlobalMemoryStatus(LPMEMORYSTATUS status);
typedef struct _SYSTEM_INFO {
    WORD wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
void GetSystemInfo(LPSYSTEM_INFO info);

/* files */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1
#define FILE_SHARE_WRITE 0x2
#define FILE_SHARE_DELETE 0x4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define INVALID_FILE_SIZE 0xffffffffu
#define INVALID_SET_FILE_POINTER 0xffffffffu
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_ALREADY_EXISTS 183
#define ERROR_IO_PENDING 997
#define ERROR_HANDLE_EOF 38
typedef struct _OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    union {
        struct {
            DWORD Offset, OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, WIN32_FIND_DATA, *LPWIN32_FIND_DATAA;
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                   DWORD flags, HANDLE template_file);
BOOL ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped);
BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped);
BOOL GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait);
DWORD SetFilePointer(HANDLE file, LONG distance, PLONG distance_high, DWORD method);
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method);
DWORD GetFileSize(HANDLE file, LPDWORD high);
BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size);
BOOL SetEndOfFile(HANDLE file);
BOOL FlushFileBuffers(HANDLE file);
BOOL GetFileTime(HANDLE file, LPFILETIME creation, LPFILETIME access, LPFILETIME write);
BOOL DeleteFileA(LPCSTR name);
BOOL MoveFileA(LPCSTR from, LPCSTR to);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa);
BOOL RemoveDirectoryA(LPCSTR name);
DWORD GetFileAttributesA(LPCSTR name);
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
BOOL FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data);
BOOL FindClose(HANDLE find);
DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer);
BOOL SetCurrentDirectoryA(LPCSTR name);
DWORD GetModuleFileNameA(HMODULE module, LPSTR name, DWORD size);
HMODULE GetModuleHandleA(LPCSTR name);
#define CreateFile CreateFileA
#define DeleteFile DeleteFileA
#define MoveFile MoveFileA
#define CopyFile CopyFileA
#define CreateDirectory CreateDirectoryA
#define RemoveDirectory RemoveDirectoryA
#define GetFileAttributes GetFileAttributesA
#define FindFirstFile FindFirstFileA
#define FindNextFile FindNextFileA
#define GetCurrentDirectory GetCurrentDirectoryA
#define SetCurrentDirectory SetCurrentDirectoryA
#define GetModuleFileName GetModuleFileNameA
#define GetModuleHandle GetModuleHandleA

/* slim reader/writer locks: a pointer-sized word, as on Windows */
typedef struct _RTL_SRWLOCK {
    PVOID Ptr;
} SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT {0}
void InitializeSRWLock(PSRWLOCK lock);
void AcquireSRWLockExclusive(PSRWLOCK lock);
void ReleaseSRWLockExclusive(PSRWLOCK lock);
void AcquireSRWLockShared(PSRWLOCK lock);
void ReleaseSRWLockShared(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock);

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size);

/* structured exceptions: the engine's crash handlers name these; nothing raises them here */
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct _CONTEXT CONTEXT, *PCONTEXT;
typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0

LPSTR GetCommandLineA(void);
#define GetCommandLine GetCommandLineA

/* errors, debugging, misc */
DWORD GetLastError(void);
void SetLastError(DWORD error);
void OutputDebugStringA(LPCSTR text);
#define OutputDebugString OutputDebugStringA
BOOL IsDebuggerPresent(void);
void DebugBreak(void);
void ExitProcess(UINT code);
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size);
#define GetEnvironmentVariable GetEnvironmentVariableA
int MessageBoxA(HWND window, LPCSTR text, LPCSTR caption, UINT type);
#define MessageBox MessageBoxA
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_YESNO 4
#define IDYES 6
#define IDNO 7
#define IDOK 1
int MultiByteToWideChar(UINT codepage, DWORD flags, LPCSTR in, int in_size, LPWSTR out, int out_size);
int WideCharToMultiByte(UINT codepage, DWORD flags, LPCWSTR in, int in_size, LPSTR out, int out_size,
                        LPCSTR default_char, LPBOOL used_default);
#define CP_ACP 0
#define CP_UTF8 65001

/* Windows.h's min/max macros, which the engine uses; libc++ guards its headers against them */
#ifndef NOMINMAX
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#endif

/* MSVC CRT names */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _alloca __builtin_alloca
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define __noop(...) ((void)0)
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
char *strdup(const char *);

#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
int posix_memalign(void **, size_t, size_t);
void free(void *);
static inline void *_aligned_malloc(size_t size, size_t alignment)
{
    void *p;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    return posix_memalign(&p, alignment, size) ? NULL : p;
}
#define _aligned_free free
void *_aligned_realloc(void *ptr, size_t size, size_t alignment);
int GetWindowTextA(HWND window, LPSTR text, int size);
LONG GetWindowLongA(HWND window, int index);
#define GWL_STYLE (-16)
HINSTANCE ShellExecuteA(HWND window, LPCSTR operation, LPCSTR file, LPCSTR parameters, LPCSTR directory,
                        INT show);
#define THREAD_SUSPEND_RESUME 0x0002

typedef struct _PROCESS_MEMORY_COUNTERS {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS, *PPROCESS_MEMORY_COUNTERS;
typedef struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX;
BOOL K32GetProcessMemoryInfo(HANDLE process, PPROCESS_MEMORY_COUNTERS counters, DWORD size);
#define GetProcessMemoryInfo K32GetProcessMemoryInfo
BOOL GetProcessHandleCount(HANDLE process, PDWORD count);
BOOL DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process, HANDLE *target,
                     DWORD access, BOOL inherit, DWORD options);
#define DUPLICATE_SAME_ACCESS 0x00000002
BOOL GetProcessAffinityMask(HANDLE process, PDWORD_PTR process_mask, PDWORD_PTR system_mask);
DWORD GetCurrentProcessorNumber(void);
BOOL SetFileAttributesA(LPCSTR name, DWORD attributes);

/* the hybrid-CPU mod option looks this API up with GetProcAddress, which finds nothing on the guest */
typedef ULONG *PULONG;
typedef enum _CPU_SET_INFORMATION_TYPE { CpuSetInformation } CPU_SET_INFORMATION_TYPE;
typedef struct _SYSTEM_CPU_SET_INFORMATION {
    DWORD Size;
    CPU_SET_INFORMATION_TYPE Type;
    struct {
        DWORD Id;
        WORD Group;
        BYTE LogicalProcessorIndex, CoreIndex, LastLevelCacheIndex, NumaNodeIndex, EfficiencyClass;
    } CpuSet;
} SYSTEM_CPU_SET_INFORMATION, *PSYSTEM_CPU_SET_INFORMATION;

/* thread sampling (the server profiler): GetThreadContext fails on the guest, so callers bail out */
struct _CONTEXT {
    DWORD ContextFlags;
    DWORD Eip, Esp, Ebp;
};
#define CONTEXT_CONTROL 0x00010001
#define THREAD_GET_CONTEXT 0x0008
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD id);
BOOL GetThreadContext(HANDLE thread, PCONTEXT context);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD id);
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_VM_READ 0x0010
BOOL GetThreadTimes(HANDLE thread, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user);
BOOL GetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user);
BOOL GetSystemTimes(LPFILETIME idle, LPFILETIME kernel, LPFILETIME user);
void RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments);

typedef struct _MEMORYSTATUSEX {
    DWORD dwLength, dwMemoryLoad;
    ULONGLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile;
    ULONGLONG ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX status);

typedef INT_PTR (*FARPROC)(void);
HMODULE LoadLibraryA(LPCSTR name);
FARPROC GetProcAddress(HMODULE module, LPCSTR name);

/* clipboard */
#define CF_TEXT 1
#define GMEM_MOVEABLE 0x0002
HGLOBAL GlobalAlloc(UINT flags, SIZE_T size);
LPVOID GlobalLock(HGLOBAL memory);
BOOL GlobalUnlock(HGLOBAL memory);
BOOL OpenClipboard(HWND owner);
BOOL EmptyClipboard(void);
HANDLE SetClipboardData(UINT format, HANDLE data);
BOOL CloseClipboard(void);

/* windows and display: no desktop windows on the Quest */
typedef BOOL (*WNDENUMPROC)(HWND window, LPARAM param);
BOOL EnumThreadWindows(DWORD thread, WNDENUMPROC callback, LPARAM param);
typedef struct tagMONITORINFO {
    DWORD cbSize;
    RECT rcMonitor, rcWork;
    DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;
#define MONITOR_DEFAULTTONULL 0
#define MONITOR_DEFAULTTOPRIMARY 1
#define MONITOR_DEFAULTTONEAREST 2
BOOL GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info);
HMONITOR MonitorFromPoint(POINT point, DWORD flags);
HMONITOR MonitorFromWindow(HWND window, DWORD flags);

/* Desktop window management. The Quest has no desktop windows: the guest platform layer
   answers these with one fixed full-screen "window" the size of the eye buffers. */
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
int GetSystemMetrics(int index);
#define GWL_EXSTYLE (-20)
#define WS_POPUP 0x80000000L
#define WS_VISIBLE 0x10000000L
#define HWND_NOTOPMOST ((HWND)(intptr_t)-2)
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_CLOSE 0x0010
#define WM_ACTIVATEAPP 0x001C
#define WA_INACTIVE 0
#define WA_ACTIVE 1
typedef struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *LPMSG;
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
BOOL PeekMessageA(LPMSG msg, HWND window, UINT first, UINT last, UINT remove);
BOOL GetMessageA(LPMSG msg, HWND window, UINT first, UINT last);
BOOL PostMessageA(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
BOOL TranslateMessage(const MSG *msg);
LRESULT DispatchMessageA(const MSG *msg);
HWND CreateWindowExA(DWORD ex_style, LPCSTR class_name, LPCSTR title, DWORD style, int x, int y, int width,
                     int height, HWND parent, HANDLE menu, HINSTANCE instance, LPVOID param);
BOOL DestroyWindow(HWND window);
BOOL IsWindow(HWND window);
BOOL IsIconic(HWND window);
BOOL ShowWindow(HWND window, int command);
HWND SetFocus(HWND window);
HWND GetForegroundWindow(void);
BOOL SetWindowPos(HWND window, HWND insert_after, int x, int y, int width, int height, UINT flags);
BOOL GetWindowRect(HWND window, LPRECT rect);
BOOL GetClientRect(HWND window, LPRECT rect);
BOOL ClientToScreen(HWND window, LPPOINT point);
BOOL AdjustWindowRectEx(LPRECT rect, DWORD style, BOOL menu, DWORD ex_style);
BOOL EqualRect(const RECT *a, const RECT *b);
BOOL IsProcessDPIAware(void);

#ifdef __cplusplus
/* as in C++ on Windows: an extended MONITORINFO, so it passes as LPMONITORINFO */
typedef struct tagMONITORINFOEXA : tagMONITORINFO {
    CHAR szDevice[32];
} MONITORINFOEXA;
#else
typedef struct tagMONITORINFOEXA {
    MONITORINFO base;
    CHAR szDevice[32];
} MONITORINFOEXA;
#endif
typedef BOOL (*MONITORENUMPROC)(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param);
BOOL EnumDisplayMonitors(HDC dc, const RECT *clip, MONITORENUMPROC callback, LPARAM param);
typedef struct _devicemodeA {
    CHAR dmDeviceName[32];
    WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    DWORD dmFields;
    DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
} DEVMODEA;
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
BOOL EnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEA *dev_mode);

/* hang detection: thread and module queries */
#define THREAD_QUERY_INFORMATION 0x0040
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *module);
BOOL QueryThreadCycleTime(HANDLE thread, ULONG64 *cycles);
BOOL QueryProcessCycleTime(HANDLE process, ULONG64 *cycles);
HWND GetActiveWindow(void);
HWND GetDesktopWindow(void);
HDC GetDC(HWND window);
int ReleaseDC(HWND window, HDC dc);
BOOL SetDeviceGammaRamp(HDC dc, LPVOID ramp);
int ShowCursor(BOOL show);
LONG SetWindowLongA(HWND window, int index, LONG value);
LONG ChangeDisplaySettingsA(void *mode, DWORD flags);

typedef void (*LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD error, DWORD transferred, LPOVERLAPPED overlapped);
BOOL ReadFileEx(HANDLE file, LPVOID buffer, DWORD size, LPOVERLAPPED overlapped,
                LPOVERLAPPED_COMPLETION_ROUTINE completion);
typedef struct _LUID { DWORD LowPart; LONG HighPart; } LUID;
typedef struct _LUID_AND_ATTRIBUTES { LUID Luid; DWORD Attributes; } LUID_AND_ATTRIBUTES;
typedef struct _TOKEN_PRIVILEGES {
    DWORD PrivilegeCount;
    LUID_AND_ATTRIBUTES Privileges[1];
} TOKEN_PRIVILEGES, *PTOKEN_PRIVILEGES;


/* the startup, crash, message-loop and clipboard calls in win_main.cpp */
#define FILE_APPEND_DATA 0x0004
UINT timeBeginPeriod(UINT period);
UINT timeEndPeriod(UINT period);
BOOL TerminateProcess(HANDLE process, UINT exit_code);
#define GetMessage GetMessageA
#define DispatchMessage DispatchMessageA
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
DWORD FormatMessageA(DWORD flags, LPCVOID source, DWORD message, DWORD language, LPSTR buffer, DWORD size,
                     va_list *arguments);
HANDLE GetClipboardData(UINT format);
SIZE_T GlobalSize(HGLOBAL memory);
HGLOBAL GlobalFree(HGLOBAL memory);
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_MOUSEWHEEL 0x020A
#define MK_LBUTTON 0x0001
typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    CHAR szCSDVersion[128];
} OSVERSIONINFOA, *LPOSVERSIONINFOA;
BOOL GetVersionExA(LPOSVERSIONINFOA info);
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(struct _EXCEPTION_POINTERS *info);
typedef LPTOP_LEVEL_EXCEPTION_FILTER PTOP_LEVEL_EXCEPTION_FILTER;
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOOPENFILEERRORBOX 0x8000
UINT SetErrorMode(UINT mode);
#define _OUT_TO_STDERR 1
#define _WRITE_ABORT_MSG 1
static inline int _set_error_mode(int mode) { (void)mode; return 0; }
static inline unsigned int _set_abort_behavior(unsigned int flags, unsigned int mask) { (void)flags; (void)mask; return 0; }
BOOL SystemParametersInfoA(UINT action, UINT param, PVOID data, UINT win_ini);
BOOL SetCursorPos(int x, int y);
BOOL GetCursorPos(LPPOINT point);
BOOL ScreenToClient(HWND window, LPPOINT point);

/* window classes (win_main's Win_RegisterClass); the Quest's single "window" ignores them */
typedef LRESULT (*WNDPROC)(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
typedef struct tagWNDCLASSEXA {
    UINT cbSize, style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName, lpszClassName;
    HICON hIconSm;
} WNDCLASSEXA;
ATOM RegisterClassExA(const WNDCLASSEXA *window_class);
HICON LoadIconA(HINSTANCE instance, LPCSTR name);
HCURSOR LoadCursorA(HINSTANCE instance, LPCSTR name);
HBRUSH CreateSolidBrush(COLORREF color);
LONG CompareFileTime(const FILETIME *a, const FILETIME *b);
BOOL SystemTimeToFileTime(const SYSTEMTIME *system_time, LPFILETIME file_time);
DWORD GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buffer, LPSTR *file_part);

#ifdef __cplusplus
}
#endif

/* Windows.h brings Winsock 1 along */
#include <winsock.h>
