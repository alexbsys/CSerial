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
 * Surviving a USB serial adapter being unplugged and plugged back in:
 *
 *   reconnect <port> [seconds]
 *
 * Prints received data; when the device disappears, keeps trying to open it
 * again.  Runs for the given number of seconds (default: forever).
 *
 * Things to keep in mind:
 *  - Opening fails with CSERIAL_ERROR_NO_SUCH_SERIAL_PORT while the device
 *    is absent.  Other failures (CSERIAL_ERROR_GENERIC) carry the OS error:
 *    EACCES on Linux means the user is not in the "dialout" group,
 *    ERROR_ACCESS_DENIED (5) on Windows means another program has the port.
 *  - An unplug shows up as a read error, or on POSIX as a successful read
 *    of 0 bytes (end of file).  Close the port before reopening it.
 *  - The port object keeps its settings across c_serial_close() and
 *    c_serial_open(): there is no need to configure it again.
 *  - After re-plugging, the OS may give the device a different name
 *    (/dev/ttyUSB1 instead of /dev/ttyUSB0).  Use stable names where
 *    available: /dev/serial/by-id/... on Linux.
 *  - POSIX does not stop a second program from opening the same port, and
 *    then the two steal bytes from each other.  TIOCEXCL makes the open
 *    exclusive (Windows ports are always exclusive).
 */

#include <stdio.h>
#include <stdlib.h>

#include "example_util.h"

#ifndef _WIN32
#include <sys/ioctl.h>
#endif

static int open_port( c_serial_port_type* port, uint64_t deadline ){
    int last_rc = 1;
    int delay = 250;

    while( 1 ){
        int rc = c_serial_open( port );
        if( rc == CSERIAL_OK ){
#ifdef TIOCEXCL
            c_serial_handle_t fd;
            if( c_serial_get_native_handle( port, &fd ) == CSERIAL_OK )
                ioctl( fd, TIOCEXCL );
#endif
            printf( "connected to %s\n", c_serial_get_port_name( port ) );
            return CSERIAL_OK;
        }

        /* Don't flood the log: report each new kind of failure once */
        if( rc != last_rc ){
            if( rc == CSERIAL_ERROR_NO_SUCH_SERIAL_PORT )
                printf( "waiting for %s to appear...\n", c_serial_get_port_name( port ) );
            else
                ex_print_error( "open", port, rc );
            last_rc = rc;
        }

        /* Settings that the hardware rejects will not get better by retrying */
        if( rc == CSERIAL_ERROR_INVALID_PARAM || rc == CSERIAL_ERROR_INVALID_FLOW ||
            rc == CSERIAL_ERROR_NOT_A_SERIAL_PORT || rc == CSERIAL_ERROR_NAME_TOO_LONG )
            return rc;

        if( ex_remaining_ms( deadline ) == 0 )
            return CSERIAL_ERROR_TIMEOUT;

        ex_sleep_ms( delay < ex_remaining_ms( deadline ) ? delay : ex_remaining_ms( deadline ) );
        if( delay < 2000 )
            delay *= 2;
    }
}

int main( int argc, char** argv ){
    int seconds = argc > 2 ? atoi( argv[ 2 ] ) : 0;
    uint64_t deadline = seconds > 0 ? ex_now_ms() + (uint64_t)seconds * 1000u : UINT64_MAX;
    c_serial_port_type* port;
    int rc;

    if( argc < 2 ){
        fprintf( stderr, "usage: %s <port> [seconds]\n", argv[ 0 ] );
        return 1;
    }

    rc = c_serial_new( &port, NULL );
    if( rc != CSERIAL_OK ){
        ex_print_error( "c_serial_new", NULL, rc );
        return 1;
    }

    rc = c_serial_set_port_name( port, argv[ 1 ] );
    if( rc == CSERIAL_OK )
        rc = c_serial_set_baud_rate( port, 115200 );
    if( rc != CSERIAL_OK ){
        ex_print_error( "configure", port, rc );
        c_serial_free( port );
        return 1;
    }

    while( ex_remaining_ms( deadline ) > 0 ){
        char buf[ 256 ];
        int len = sizeof( buf );

        if( !c_serial_is_open( port ) ){
            rc = open_port( port, deadline );
            if( rc == CSERIAL_ERROR_TIMEOUT )
                break;
            if( rc != CSERIAL_OK ){
                c_serial_free( port );
                return 1;
            }
        }

        /* A bounded timeout keeps the loop responsive (deadline, keep-alives) */
        rc = c_serial_read_data_timeout( port, buf, &len, NULL,
            ex_remaining_ms( deadline ) < 1000 ? ex_remaining_ms( deadline ) : 1000 );

        if( rc == CSERIAL_ERROR_TIMEOUT )
            continue;

        if( rc == CSERIAL_OK && len > 0 ){
            printf( "received %d bytes: %.*s\n", len, len, buf );
            continue;
        }

        if( rc == CSERIAL_OK )
            printf( "device hung up\n" );
        else
            ex_print_error( "connection lost", port, rc );
        c_serial_close( port );
    }

    c_serial_free( port );
    return 0;
}
