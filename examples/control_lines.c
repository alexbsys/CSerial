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
 * Modem control lines:
 *
 *   control_lines <port> reset    pulse DTR/RTS, e.g. to reset an Arduino or
 *                                 an ESP32 dev board
 *   control_lines <port> watch    print CTS/DSR/CD/RI changes for 10 seconds
 *
 * Things to keep in mind:
 *  - Only DTR and RTS are outputs; CTS, DSR, CD and RI are inputs.
 *    c_serial_set_control_line() ignores the input fields.
 *  - Many USB adapters, Bluetooth serial ports and ptys have no modem lines
 *    at all.  Reads then report lines.unsupported = 1, waiting for a line
 *    change returns immediately, and c_serial_get_control_lines() fails.
 *  - To wait only for line changes, pass data = NULL.  Without any flags set
 *    with c_serial_set_serial_line_change_flags() such a read returns at
 *    once, it never blocks forever.
 *  - With hardware flow control the driver owns RTS: setting it by hand has
 *    no lasting effect.
 *  - On POSIX systems opening the port asserts DTR and RTS, which is what
 *    resets an Arduino when a terminal connects.  On Windows CSerial opens
 *    the port with both lines off.
 */

#include <stdio.h>
#include <string.h>

#include "example_util.h"

static void print_lines( const char* prefix, const c_serial_control_lines_type* lines ){
    if( lines->unsupported ){
        printf( "%s: control lines are not supported by this port\n", prefix );
        return;
    }
    printf( "%s: CTS=%d DSR=%d CD=%d RI=%d | DTR=%d RTS=%d\n", prefix,
        lines->cts, lines->dsr, lines->cd, lines->ri, lines->dtr, lines->rts );
}

static int set_lines( c_serial_port_type* port, int dtr, int rts ){
    c_serial_control_lines_type lines;

    memset( &lines, 0, sizeof( lines ) );
    lines.dtr = dtr;
    lines.rts = rts;
    return c_serial_set_control_line( port, &lines, 0 );
}

/* The classic "auto reset" sequence: drop DTR for a moment */
static int pulse_reset( c_serial_port_type* port ){
    int rc = set_lines( port, 0, 0 );
    if( rc != CSERIAL_OK )
        return rc;
    ex_sleep_ms( 100 );
    return set_lines( port, 1, 1 );
}

static int watch_lines( c_serial_port_type* port, int seconds ){
    uint64_t deadline = ex_now_ms() + (uint64_t)seconds * 1000u;
    c_serial_control_lines_type lines;
    int rc;

    /* Unlike a read, this reports missing modem lines as an error
     * (ENOTTY/EINVAL on POSIX) rather than with lines.unsupported */
    rc = c_serial_get_control_lines( port, &lines );
    if( rc != CSERIAL_OK ){
        printf( "cannot read control lines, the port probably has none\n" );
        return rc;
    }
    print_lines( "initial", &lines );

    c_serial_set_serial_line_change_flags( port,
        CSERIAL_LINE_FLAG_CTS | CSERIAL_LINE_FLAG_DSR | CSERIAL_LINE_FLAG_CD | CSERIAL_LINE_FLAG_RI );

    while( ex_remaining_ms( deadline ) > 0 ){
        uint64_t started = ex_now_ms();

        rc = c_serial_read_data_timeout( port, NULL, NULL, &lines, ex_remaining_ms( deadline ) );
        if( rc == CSERIAL_ERROR_TIMEOUT )
            break;
        if( rc != CSERIAL_OK )
            return rc;
        if( lines.unsupported ){
            print_lines( "watch", &lines );
            break;
        }
        print_lines( "changed", &lines );

        /* Guard against spinning if the port keeps returning at once */
        if( ex_now_ms() - started < 5 )
            ex_sleep_ms( 5 );
    }

    return CSERIAL_OK;
}

int main( int argc, char** argv ){
    c_serial_port_type* port;
    int rc;

    if( argc < 3 || ( strcmp( argv[ 2 ], "reset" ) != 0 && strcmp( argv[ 2 ], "watch" ) != 0 ) ){
        fprintf( stderr, "usage: %s <port> reset|watch\n", argv[ 0 ] );
        return 1;
    }

    rc = c_serial_new( &port, NULL );
    if( rc != CSERIAL_OK ){
        ex_print_error( "c_serial_new", NULL, rc );
        return 1;
    }

    rc = c_serial_set_port_name( port, argv[ 1 ] );
    if( rc == CSERIAL_OK )
        rc = c_serial_open( port );
    if( rc != CSERIAL_OK ){
        ex_print_error( "open", port, rc );
        c_serial_free( port );
        return 1;
    }

    if( strcmp( argv[ 2 ], "reset" ) == 0 ){
        rc = pulse_reset( port );
        if( rc == CSERIAL_OK )
            printf( "reset pulse sent\n" );
        else
            printf( "cannot set DTR/RTS, the port probably has no modem lines\n" );
    }else{
        rc = watch_lines( port, 10 );
    }

    if( rc != CSERIAL_OK )
        ex_print_error( argv[ 2 ], port, rc );

    c_serial_free( port );
    return rc == CSERIAL_OK ? 0 : 1;
}
