/* tpidr_el0 belongs to the host's bionic thread; the guest's thread pointer is kept by the host. */
unsigned __guest_get_tp(void);

static inline uintptr_t __get_tp()
{
	return __guest_get_tp();
}

#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 16

#define MC_PC pc
