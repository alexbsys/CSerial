
#ifndef C_SERIAL_ATOMIC_HEADER
#define C_SERIAL_ATOMIC_HEADER

#include "c_serial_pdetect.h"

/*
 * Atomic implementation
 *
 * Two operations on a long shared between threads (is_read_active,
 * is_closing):
 *   cserial_atomic_exchange()  store new_val, return the previous value
 *   cserial_atomic_read()      load the current value
 * Both are sequentially consistent (full barrier), so a store in one thread
 * and a following load in another are seen in program order.
 *
 * Implementations:
 *   GCC       __atomic builtins (GCC, Clang, XL C)
 *   WINDOWS   MSVC _Interlocked* intrinsics; a compare-exchange of 0 with 0
 *             reads the value with a full barrier and never changes it
 *   NOATOMIC  plain volatile access, only for single-threaded use
 */
#define CSERIAL_ATOMIC_IMPL_GCC 1
#define CSERIAL_ATOMIC_IMPL_WINDOWS 2
#define CSERIAL_ATOMIC_IMPL_NOATOMIC 4

 /* User can override value CSERIAL_ATOMIC_IMPL by himself */
#if !defined(CSERIAL_ATOMIC_IMPL)
#if defined (CSERIAL_COMPILER_GCC_COMPAT)
#define CSERIAL_ATOMIC_IMPL CSERIAL_ATOMIC_IMPL_GCC
#elif defined(CSERIAL_PLATFORM_WINDOWS)
#define CSERIAL_ATOMIC_IMPL CSERIAL_ATOMIC_IMPL_WINDOWS
#else
#define CSERIAL_ATOMIC_IMPL CSERIAL_ATOMIC_IMPL_NOATOMIC
#endif

#endif /*!defined(CSERIAL_ATOMIC_IMPL)*/

#if CSERIAL_ATOMIC_IMPL==CSERIAL_ATOMIC_IMPL_WINDOWS
#include <intrin.h>
#pragma intrinsic(_InterlockedExchange)
#pragma intrinsic(_InterlockedCompareExchange)
#endif  /*CSERIAL_ATOMIC_IMPL==CSERIAL_ATOMIC_IMPL_WINDOWS*/

/* volatile only keeps the compiler from caching the value, the atomicity
 * comes from the functions below */
typedef volatile long atomic_long_type;

#if CSERIAL_ATOMIC_IMPL==CSERIAL_ATOMIC_IMPL_GCC

static long FUNC_INTERNAL_USED cserial_atomic_exchange(long volatile* variable, long new_val) {
  return __atomic_exchange_n(variable, new_val, __ATOMIC_SEQ_CST);
}

static long FUNC_INTERNAL_USED cserial_atomic_read(long volatile* variable) {
  return __atomic_load_n(variable, __ATOMIC_SEQ_CST);
}

#elif CSERIAL_ATOMIC_IMPL==CSERIAL_ATOMIC_IMPL_WINDOWS

static long FUNC_INTERNAL_USED cserial_atomic_exchange(long volatile* variable, long new_val) {
  return _InterlockedExchange(variable, new_val);
}

static long FUNC_INTERNAL_USED cserial_atomic_read(long volatile* variable) {
  return _InterlockedCompareExchange(variable, 0, 0);
}

#elif CSERIAL_ATOMIC_IMPL==CSERIAL_ATOMIC_IMPL_NOATOMIC

/* No atomic support: plain volatile access, only safe for single-threaded use */
static long FUNC_INTERNAL_USED cserial_atomic_exchange(long volatile* variable, long new_val) {
  long old_val = *variable;
  *variable = new_val;
  return old_val;
}

static long FUNC_INTERNAL_USED cserial_atomic_read(long volatile* variable) {
  return *variable;
}

#endif /*end of CSERIAL_ATOMIC_IMPL*/

#endif /*C_SERIAL_ATOMIC_HEADER*/
