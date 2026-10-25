/*
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
*/

/*
 * Small portability helpers shared by the examples: sleeping, a monotonic
 * clock, threads and error printing.  Not part of the library.
 */
#ifndef CSERIAL_EXAMPLE_UTIL_H
#define CSERIAL_EXAMPLE_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cserial/c_serial.h>

#if defined(_MSC_VER) && !defined(__cplusplus)
#define EX_INLINE static __inline
#else
#define EX_INLINE static inline
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

EX_INLINE void ex_sleep_ms( int ms ){
#ifdef _WIN32
    Sleep( (DWORD)ms );
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)( ms % 1000 ) * 1000000L;
    nanosleep( &ts, NULL );
#endif
}

EX_INLINE uint64_t ex_now_ms( void ){
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* Milliseconds left until deadline, never negative */
EX_INLINE int ex_remaining_ms( uint64_t deadline ){
    uint64_t now = ex_now_ms();
    return now >= deadline ? 0 : (int)( deadline - now );
}

typedef void (*ex_thread_fn)( void* arg );

struct ex_thread_start {
    ex_thread_fn fn;
    void* arg;
};

#ifdef _WIN32
typedef HANDLE ex_thread_t;

EX_INLINE DWORD WINAPI ex_thread_trampoline( LPVOID param ){
#else
typedef pthread_t ex_thread_t;

EX_INLINE void* ex_thread_trampoline( void* param ){
#endif
    struct ex_thread_start start = *(struct ex_thread_start*)param;
    free( param );
    start.fn( start.arg );
    return 0;
}

EX_INLINE int ex_thread_create( ex_thread_t* thread, ex_thread_fn fn, void* arg ){
    struct ex_thread_start* start = (struct ex_thread_start*)malloc( sizeof( *start ) );
    if( start == NULL )
        return -1;
    start->fn = fn;
    start->arg = arg;
#ifdef _WIN32
    *thread = CreateThread( NULL, 0, ex_thread_trampoline, start, 0, NULL );
    if( *thread != NULL )
        return 0;
#else
    if( pthread_create( thread, NULL, ex_thread_trampoline, start ) == 0 )
        return 0;
#endif
    free( start );
    return -1;
}

EX_INLINE void ex_thread_join( ex_thread_t thread ){
#ifdef _WIN32
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
#else
    pthread_join( thread, NULL );
#endif
}

/* Print a library error together with the OS error that caused it */
EX_INLINE void ex_print_error( const char* what, c_serial_port_type* port, int rc ){
    fflush( stdout );
    fprintf( stderr, "%s: %s (%d)", what, c_serial_get_error_string( rc ), rc );
    if( port != NULL ){
        c_serial_errnum_t native = c_serial_get_last_native_errnum( port );
        if( native != 0 ){
#ifdef _WIN32
            fprintf( stderr, ", GetLastError() = %lu", (unsigned long)native );
#else
            fprintf( stderr, ", errno = %d (%s)", (int)native, strerror( (int)native ) );
#endif
        }
    }
    fprintf( stderr, "\n" );
}

#endif /* CSERIAL_EXAMPLE_UTIL_H */
