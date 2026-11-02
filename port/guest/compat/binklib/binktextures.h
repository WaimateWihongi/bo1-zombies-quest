/* RAD's headers only know Linux by the old `linux` macro. Bink itself is an x86 Windows DLL,
   so the guest stubs its functions (cutscenes are skipped); this just lets the types compile. */
#pragma once
#define linux 1
#include_next <binklib/binktextures.h>
#undef linux
