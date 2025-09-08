#ifndef C_SERIAL_PDETECT_HEADER
#define C_SERIAL_PDETECT_HEADER

/*
 * Platform detect
 */
#if defined(unix) || defined(__unix__) || defined(__unix)
#define CSERIAL_PLATFORM_POSIX_BASED

#if defined(__CYGWIN__) && !defined(_WIN32)
#define CSERIAL_PLATFORM_CYGWIN
#endif  /* defined(__CYGWIN__) && !defined(_WIN32)*/

#if !defined(__linux__) && !defined(__linux) && !defined(linux) && !defined(__sun__) && \
    !defined(__sun) && !defined(__CYGWIN__) && !defined(__APPLE__) && !defined(_WIN32)
#define CSERIAL_PLATFORM_UNIX
#include <sys/param.h>

#if defined(BSD)
#define CSERIAL_PLATFORM_BSD
#endif  /* BSD*/

#if defined(__FreeBSD__)
#define CSERIAL_PLATFORM_FREEBSD
#endif  /*__FreeBSD__*/

#if defined(__NetBSD__)
#define CSERIAL_PLATFORM_NETBSD
#endif  /*__NetBSD__*/

#if defined(__DragonFly__)
#define CSERIAL_PLATFORM_DRAGONFLYBSD
#endif  /*__DragonFly__*/

#endif  /*! defined(__linux__) && !defined(__linux) && !defined(linux) &&
 ! !defined(__sun__) && !defined(__sun) && !defined(__CYGWIN__) &&
 ! !defined(__APPLE__) && !defined(_WIN32)*/

#endif  /* defined(unix) || defined(__unix__) || defined(__unix) */

#if !defined(CSERIAL_PLATFORM_UNIX) && defined(CSERIAL_PLATFORM_POSIX_BASED)
#if defined(__linux__) || defined(__linux) || defined(__gnu_linux) || defined(linux)
#define CSERIAL_PLATFORM_LINUX
#endif  /* defined(__linux__) || defined(__linux) || defined(__gnu_linux) || defined(linux)*/
#endif  /*! defined CSERIAL_PLATFORM_UNIX && defined(CSERIAL_PLATFORM_POSIX_BASED)*/


#if defined(__APPLE__) && defined(__MACH__)
#define CSERIAL_PLATFORM_POSIX_BASED
#define CSERIAL_PLATFORM_MAC

#include <TargetConditionals.h>
#if TARGET_IPHONE_SIMULATOR == 1
#define CSERIAL_PLATFORM_IPHONE
#define CSERIAL_PLATFORM_IPHONE_SIMULATOR
#elif TARGET_OS_IPHONE == 1
#define CSERIAL_PLATFORM_IPHONE
#elif TARGET_OS_MAC == 1
#define CSERIAL_PLATFORM_MACOSX
#endif /*TARGET_IPHONE_SIMULATOR == 1*/

#endif  /* defined(__APPLE__) && defined(__MACH__) */

#if defined(CSERIAL_PLATFORM_POSIX_BASED) && \
    (defined(__sun__) || defined(__sun) || defined(__SunOS) || defined(sun))
#define CSERIAL_PLATFORM_SOLARIS
#endif  /* defined(CSERIAL_PLATFORM_POSIX_BASED) && (defined(__sun__) || defined(__sun) ||
			defined(__SunOS) || defined(sun)) */

#ifdef _WIN32
#undef CSERIAL_PLATFORM_POSIX_BASED  /* it may be set if you use CYGWIN with GCC for Windows */
#undef CSERIAL_PLATFORM_UNIX
#define CSERIAL_PLATFORM_WINDOWS
#endif  /*_WIN32*/

#ifdef __ANDROID__
#define CSERIAL_PLATFORM_ANDROID
#endif /*__ANDROID__*/


/*
 * Compiler detect
 */

#ifdef _MSC_VER
#define CSERIAL_COMPILER_MSVC
#endif  /*_MSC_VER*/

#ifdef __GNUC__
#define CSERIAL_COMPILER_GCC
#endif  /*__GNUC__*/

#ifdef __IBMCPP__
#define CSERIAL_COMPILER_IBM
#endif  /*__IBMCPP__*/

#if defined(__ICC) || defined(__INTEL_COMPILER)
#define CSERIAL_COMPILER_ICC
#endif  /* defined(__ICC) || defined(__INTEL_COMPILER) */

#if defined(__SUNPRO_CC) || defined(__SUNPRO_C)
#define CSERIAL_COMPILER_SOLARIS
#endif  /* defined(__SUNPRO_CC) || defined(__SUNPRO_C) */

#if defined(__MINGW32__) || defined(__MINGW64__)
#define CSERIAL_COMPILER_MINGW
#endif  /* defined(__MINGW32__) || defined(__MINGW64__) */

#if defined(__clang__)
#define CSERIAL_COMPILER_CLANG
#endif /*__clang__*/

#if defined(__xlc__)
#define CSERIAL_COMPILER_XLC
#endif /*__xlc__*/

#if defined(CSERIAL_COMPILER_GCC) || defined(CSERIAL_COMPILER_CLANG) || defined(CSERIAL_COMPILER_XLC)
#define CSERIAL_COMPILER_GCC_COMPAT
#endif /*CSERIAL_COMPILER_GCC || CSERIAL_COMPILER_CLANG || CSERIAL_COMPILER_XLC*/

/****** FUNC_INTERNAL_USED macro declaration ******/

#if defined(__GNUC__) || defined(__clang__)
#define FUNC_INTERNAL_USED __attribute__((used))
#elif defined(_MSC_VER)
#define FUNC_INTERNAL_USED __pragma(warning(suppress: 4505 4514))
#else
#define FUNC_INTERNAL_USED
#endif

#endif /*C_SERIAL_PDETECT_HEADER*/
