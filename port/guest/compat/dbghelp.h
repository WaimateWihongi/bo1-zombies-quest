/* the symbol lookup the server profiler calls through GetProcAddress (absent on the guest) */
#pragma once
#include <windows.h>

typedef struct _SYMBOL_INFO {
    ULONG SizeOfStruct, TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index, Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value, Address;
    ULONG Register, Scope, Tag, NameLen, MaxNameLen;
    CHAR Name[1];
} SYMBOL_INFO, *PSYMBOL_INFO;
