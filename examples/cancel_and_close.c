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
 * Stopping a reader thread that is blocked in c_serial_read_data():
 *
 *   cancel_and_close <port>
 *
 * A background thread reads with an infinite timeout and prints whatever
 * arrives.  The main thread first interrupts it with c_serial_read_cancel()
 * (the reader goes on reading afterwards), then shuts it down with
 * c_serial_close().
 *
 * Things to keep in mind:
 *  - c_serial_read_cancel() only aborts the read that is running right now;
 *    the reader sees CSERIAL_ERROR_CANCELLED and the next read blocks again.
 *    With no read in progress it returns CSERIAL_OK immediately.
 *  - c_serial_close() is the way to stop a reader for good: it cancels the
 *    running read, waits for it to leave the library, closes the handle, and
 *    every later read fails with CSERIAL_ERROR_NO_SUCH_SERIAL_PORT.  The
 *    reader needs no extra "stop" flag: it exits on that error code.
 *  - Never c_serial_free() a port while another thread may still use it.
 *    Close, join the reader, then free.
 *  - Only one read at a time per port.  Reading from one thread while
 *    writing from another one is fine.
 */

#include <stdio.h>

#include "example_util.h"

struct reader_ctx {
    c_serial_port_type* port;
    int cancelled;      /* only touched by the reader thread until it is joined */
    int final_status;
};

static void reader_thread( void* arg ){
    struct reader_ctx* ctx = (struct reader_ctx*)arg;
    char buf[ 128 ];

    while( 1 ){
        int len = sizeof( buf );
        int rc = c_serial_read_data( ctx->port, buf, &len, NULL );

        if( rc == CSERIAL_OK ){
            printf( "reader: got %d bytes\n", len );
            if( len == 0 ){
                /* POSIX end of file: the device went away */
                ctx->final_status = CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
                return;
            }
            continue;
        }

        if( rc == CSERIAL_ERROR_CANCELLED ){
            printf( "reader: read cancelled, reading again\n" );
            ctx->cancelled++;
            continue;
        }

        /* CSERIAL_ERROR_NO_SUCH_SERIAL_PORT after c_serial_close(), or a real I/O error */
        ctx->final_status = rc;
        return;
    }
}

int main( int argc, char** argv ){
    struct reader_ctx ctx;
    ex_thread_t thread;
    uint64_t start;
    int rc;

    if( argc < 2 ){
        fprintf( stderr, "usage: %s <port>\n", argv[ 0 ] );
        return 1;
    }

    ctx.cancelled = 0;
    ctx.final_status = CSERIAL_OK;
    rc = c_serial_new( &ctx.port, NULL );
    if( rc != CSERIAL_OK ){
        ex_print_error( "c_serial_new", NULL, rc );
        return 1;
    }

    rc = c_serial_set_port_name( ctx.port, argv[ 1 ] );
    if( rc == CSERIAL_OK )
        rc = c_serial_open( ctx.port );
    if( rc != CSERIAL_OK ){
        ex_print_error( "open", ctx.port, rc );
        c_serial_free( ctx.port );
        return 1;
    }

    /* Nothing is reading yet: returns at once */
    rc = c_serial_read_cancel( ctx.port, 1000 );
    printf( "main: cancel without a reader: %s\n", c_serial_get_error_string( rc ) );

    if( ex_thread_create( &thread, reader_thread, &ctx ) != 0 ){
        fprintf( stderr, "cannot create thread\n" );
        c_serial_free( ctx.port );
        return 1;
    }

    ex_sleep_ms( 300 );

    /* Interrupt the current read; wait at most 1 s for the reader to leave it */
    start = ex_now_ms();
    rc = c_serial_read_cancel( ctx.port, 1000 );
    printf( "main: c_serial_read_cancel: %s after %d ms\n",
        c_serial_get_error_string( rc ), (int)( ex_now_ms() - start ) );

    ex_sleep_ms( 300 );

    /* Stop the reader for good */
    start = ex_now_ms();
    c_serial_close( ctx.port );
    ex_thread_join( thread );
    printf( "main: reader stopped %d ms after c_serial_close: %s\n",
        (int)( ex_now_ms() - start ), c_serial_get_error_string( ctx.final_status ) );
    printf( "main: reads cancelled: %d\n", ctx.cancelled );

    /* The port object is still valid and keeps its settings: it can be
     * reopened, or freed now that no other thread uses it. */
    c_serial_free( ctx.port );
    return ctx.final_status == CSERIAL_ERROR_NO_SUCH_SERIAL_PORT ? 0 : 1;
}
