/* musl's internal features.h with weak_alias done by the assembler: the guest's Darwin target
   rejects __attribute__((alias)), but a .set in the emitted assembly becomes an ordinary ELF
   alias after port/tools/asm_convert.py. The pointer keeps a static target from being dropped. */
#ifndef BO1_GUEST_FEATURES_H
#define BO1_GUEST_FEATURES_H

#include_next "features.h"

#undef weak_alias
#define weak_alias(old, new) \
	extern __typeof(old) new; \
	static void *const __alias_keep_##new __attribute__((used)) = (void *)&old; \
	__asm__(".globl _" #new "\n.weak_definition _" #new "\n.set _" #new ", _" #old "\n")

#endif
