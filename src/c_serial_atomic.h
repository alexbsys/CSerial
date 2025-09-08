
#ifndef C_SERIAL_ATOMIC_HEADER
#define C_SERIAL_ATOMIC_HEADER

#include "c_serial_pdetect.h"

/*
 * Atomic implementation
 */
#define CSERIAL_ATOMIC_IMPL_GCC 1
#define CSERIAL_ATOMIC_IMPL_WINDOWS 2
#define CSERIAL_ATOMIC_IMPL_NOATOMIC 4

 /* User can override value LOG_ATOMIC_IMPL by himself */
#if !defined(CSERIAL_ATOMIC_IMPL)
#if defined (CSERIAL_COMPILER_GCC_COMPAT)
#define CSERIAL_ATOMIC_IMPL CSERIAL_ATOMIC_IMPL_GCC
#elif defined(CSERIAL_PLATFORM_WINDOWS)
#define CSERIAL_ATOMIC_IMPL CSERIAL_ATOMIC_IMPL_WINDOWS
#else
#define CSERIAL_ATOMIC_IMPL CSERIAL_ATOMIC_IMPL_NOATOMIC
#endif

#endif /*!defined(CSERIAL_ATOMIC_IMPL)*/

#if CSERIAL_ATOMIC_IMPL==LOG_ATOMIC_IMPL_WINDOWS
long _InterlockedIncrement(long volatile*);
long _InterlockedDecrement(long volatile*);
long __cdecl _InterlockedExchange(long volatile*, long);
long __cdecl _InterlockedCompareExchange(long volatile*, long, long);

#pragma intrinsic(_InterlockedIncrement)
#pragma intrinsic(_InterlockedDecrement)
#pragma intrinsic(_InterlockedCompareExchange)
#endif  /*end of LOG_ATOMIC_IMPL==LOG_ATOMIC_IMPL_WINDOWS*/

typedef volatile long atomic_long_type;

#if CSERIAL_ATOMIC_IMPL==CSERIAL_ATOMIC_IMPL_GCC
/* GCC implementation*/
static long FUNC_INTERNAL_USED atomic_increment(long volatile* variable) {
  return __atomic_add_fetch(variable, 1, __ATOMIC_SEQ_CST);
}

static long FUNC_INTERNAL_USED atomic_decrement(long volatile* variable) {
  return __atomic_sub_fetch(variable, 1, __ATOMIC_SEQ_CST);
}

static long FUNC_INTERNAL_USED atomic_exchange(long volatile* variable, long new_val) {
  return __atomic_exchange_n(variable, new_val, __ATOMIC_SEQ_CST);
}

static int FUNC_INTERNAL_USED atomic_compare_exchange(long volatile* variable, long new_val, long* expected_val) {
  return __atomic_compare_exchange_n(variable, expected_val, new_val, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#elif CSERIAL_ATOMIC_IMPL==CSERIAL_ATOMIC_IMPL_WINDOWS
/* Windows implementation */
static long FUNC_INTERNAL_USED atomic_increment(long volatile* variable) {
  return _InterlockedIncrement(variable);
}

static long FUNC_INTERNAL_USED atomic_decrement(long volatile* variable) {
  return _InterlockedDecrement(variable);
}

static long FUNC_INTERNAL_USED atomic_exchange(long volatile* variable, long new_val) {
  return _InterlockedExchange(variable, new_val);
}

static int FUNC_INTERNAL_USED atomic_compare_exchange(long volatile* variable, long new_val, long* expected_val) {
  long old_val = _InterlockedCompareExchange(variable, new_val, *expected_val);
  int ret = old_val == *expected_val;
  *expected_val = old_val;
  return ret;
}

#elif CSERIAL_ATOMIC_IMPL==CSERIAL_ATOMIC_IMPL_NOATOMIC

/* NO ATOMIC SUPPORT, JUST EMULATE OPERATIONS */
#pragma warning("Atomic operations are not supported!")

static long FUNC_INTERNAL_USED atomic_increment(long volatile* variable) {
  long old_val = *variable;
  *variable++;
  return old_val;
}

static long FUNC_INTERNAL_USED atomic_decrement(long volatile* variable) {
  long old_val = *variable;
  *variable--;
  return old_val;
}

static long FUNC_INTERNAL_USED atomic_exchange(long volatile* variable, long new_val) {
  long old_val = *variable;
  return *variable;
}

static int FUNC_INTERNAL_USED atomic_compare_exchange(long volatile* variable, long new_val, long* expected_val) {
  long old_val = *variable;
  if (old_val == *expected_val) {
    *variable = new_val;
  }

  int ret = old_val == *expected_val;
  *expected_val = old_val;
  return ret;
}

#endif /*end of CSERIAL_ATOMIC_IMPL*/

static long FUNC_INTERNAL_USED atomic_read(long volatile* variable) {
  long exp_val = 0;
  atomic_compare_exchange(variable, 0, &exp_val);
  return exp_val;
}

#endif /*C_SERIAL_ATOMIC_HEADER*/
