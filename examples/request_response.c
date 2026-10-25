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
 * Request/response over a serial line with deadlines, e.g. AT commands or
 * a text protocol:
 *
 *   request_response <port> [command] [baud]
 *
 * Sends "command\r\n" (default "AT") and prints the reply lines that arrive
 * within one second.
 *
 * Things to keep in mind:
 *  - A read returns as soon as *some* data is there.  "OK\r\n" can arrive
 *    as "O", "K\r", "\n": always accumulate until a terminator or a deadline.
 *  - A write may also be partial.  c_serial_write_data_timeout() reports the
 *    number of bytes actually written even when it times out.
 *  - Stale bytes from a previous exchange or from the device booting are
 *    still in the input buffer.  Drain them before sending a request: a read
 *    with a 0 ms timeout returns buffered data or CSERIAL_ERROR_TIMEOUT
 *    without waiting.  (c_serial_flush() waits for *output* to be sent; it
 *    does not discard input.)
 *  - Use one overall deadline per operation, not a fresh timeout for every
 *    read call, or a device sending one byte per second keeps you forever.
 *  - On POSIX a read that returns CSERIAL_OK with 0 bytes means end of file:
 *    the other side hung up (USB adapter unplugged, pty closed).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "example_util.h"

#define CMD_TIMEOUT_MS 1000

/* Discard everything already received.  Bounded in time: a device that
 * streams continuously would otherwise keep us here forever. */
static int drain_input( c_serial_port_type* port ){
    uint64_t deadline = ex_now_ms() + 200;
    char junk[ 256 ];
    int len;
    int rc;

    do {
        len = sizeof( junk );
        rc = c_serial_read_data_timeout( port, junk, &len, NULL, 0 );
    } while( rc == CSERIAL_OK && len > 0 && ex_remaining_ms( deadline ) > 0 );

    return rc == CSERIAL_ERROR_TIMEOUT || rc == CSERIAL_OK ? CSERIAL_OK : rc;
}

/* Write the whole buffer or fail; partial writes are continued */
static int write_all( c_serial_port_type* port, const char* data, int length, uint64_t deadline ){
    while( length > 0 ){
        int chunk = length;
        int rc = c_serial_write_data_timeout( port, data, &chunk, ex_remaining_ms( deadline ) );

        data += chunk;
        length -= chunk;
        if( rc == CSERIAL_ERROR_TIMEOUT && ex_remaining_ms( deadline ) > 0 )
            continue;
        if( rc != CSERIAL_OK )
            return rc;
    }
    return CSERIAL_OK;
}

/*
 * Read one line terminated by '\n' into buf (NUL-terminated, '\r' and '\n'
 * stripped).  Reads one byte at a time so that nothing after the
 * terminator is consumed; for bulk data read bigger blocks and keep the
 * rest for the next call.
 */
static int read_line( c_serial_port_type* port, char* buf, int size, uint64_t deadline ){
    int used = 0;

    while( 1 ){
        char c;
        int len = 1;
        int rc = c_serial_read_data_timeout( port, &c, &len, NULL, ex_remaining_ms( deadline ) );

        if( rc != CSERIAL_OK )
            return rc;
        if( len == 0 )
            return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;    /* POSIX end of file */

        if( c == '\n' )
            break;
        if( c == '\r' )
            continue;
        if( used == size - 1 )
            return CSERIAL_ERROR_INVALID_PARAM;          /* line does not fit */
        buf[ used++ ] = c;
    }

    buf[ used ] = '\0';
    return CSERIAL_OK;
}

int main( int argc, char** argv ){
    const char* command = argc > 2 ? argv[ 2 ] : "AT";
    int baud = argc > 3 ? atoi( argv[ 3 ] ) : 115200;
    c_serial_port_type* port;
    char request[ 256 ];
    char line[ 256 ];
    uint64_t deadline;
    int lines = 0;
    int rc;

    if( argc < 2 ){
        fprintf( stderr, "usage: %s <port> [command] [baud]\n", argv[ 0 ] );
        return 1;
    }

    if( strlen( command ) > sizeof( request ) - 3 ){
        fprintf( stderr, "command is too long\n" );
        return 1;
    }
    snprintf( request, sizeof( request ), "%s\r\n", command );

    rc = c_serial_new( &port, NULL );
    if( rc != CSERIAL_OK ){
        ex_print_error( "c_serial_new", NULL, rc );
        return 1;
    }

    rc = c_serial_set_port_name( port, argv[ 1 ] );
    if( rc == CSERIAL_OK )
        rc = c_serial_set_baud_rate( port, baud );
    if( rc == CSERIAL_OK )
        rc = c_serial_open( port );
    if( rc != CSERIAL_OK ){
        ex_print_error( "open", port, rc );
        c_serial_free( port );
        return 1;
    }

    rc = drain_input( port );
    if( rc != CSERIAL_OK )
        ex_print_error( "drain", port, rc );

    deadline = ex_now_ms() + CMD_TIMEOUT_MS;
    rc = write_all( port, request, (int)strlen( request ), deadline );
    if( rc != CSERIAL_OK ){
        /* e.g. hardware flow control and the device holds CTS low */
        ex_print_error( "write", port, rc );
        c_serial_free( port );
        return 1;
    }

    while( ( rc = read_line( port, line, sizeof( line ), deadline ) ) == CSERIAL_OK ){
        /* Modems frame replies as "\r\nOK\r\n": skip the empty lines */
        if( line[ 0 ] == '\0' )
            continue;
        printf( "< %s\n", line );
        lines++;
        /* Typical AT-style final responses */
        if( strcmp( line, "OK" ) == 0 || strcmp( line, "ERROR" ) == 0 )
            break;
    }

    if( rc == CSERIAL_ERROR_TIMEOUT && lines == 0 )
        printf( "No answer within %d ms\n", CMD_TIMEOUT_MS );
    else if( rc != CSERIAL_OK && rc != CSERIAL_ERROR_TIMEOUT )
        ex_print_error( "read", port, rc );

    c_serial_free( port );
    return lines > 0 ? 0 : 2;
}
