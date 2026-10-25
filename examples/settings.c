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
 * Port settings and the errors they can produce:
 *
 *   settings              rules that hold without any hardware
 *   settings <port>       plus non-standard baud rates, 7E1, RS485 and
 *                         reusing the settings made by another program
 *
 * Things to keep in mind:
 *  - Setters work on a closed port too: the values are cached and applied
 *    by c_serial_open().  Getters return the cached values while closed and
 *    the values reported by the driver while open.
 *  - On an open port a setter applies the value immediately.  Read it back
 *    if it matters: drivers may silently round a baud rate.
 *  - Non-standard baud rates (250000 for DMX/3D printers, 1000000 ...)
 *    depend on the OS and the adapter.  If the driver rejects one at open
 *    time, c_serial_open() fails and the port stays closed.
 *  - Hardware flow control and RS485 RTS handling both drive RTS, so they
 *    exclude each other (CSERIAL_ERROR_INVALID_FLOW); the rejected setting
 *    is not stored.
 *  - c_serial_open_keep_settings( port, 1 ) leaves the line settings as they
 *    are, e.g. configured by `stty` or by a previous program.
 */

#include <stdio.h>
#include <stdlib.h>

#include "example_util.h"

#define CHECK( expr, expected ) check( #expr, ( expr ), ( expected ) )

static int failures = 0;

static void check( const char* what, int rc, int expected ){
    printf( "%-62s -> %s\n", what, c_serial_get_error_string( rc ) );
    if( rc != expected ){
        printf( "    UNEXPECTED, wanted %s\n", c_serial_get_error_string( expected ) );
        failures++;
    }
}

static void offline_rules( void ){
    c_serial_port_type* port;

    if( c_serial_new( &port, NULL ) != CSERIAL_OK )
        exit( 1 );

    printf( "--- closed port\n" );
    CHECK( c_serial_open( port ), CSERIAL_ERROR_NO_PORT );
    CHECK( c_serial_set_port_name( port, "" ), CSERIAL_ERROR_INVALID_PARAM );
#ifdef _WIN32
    /* Windows names are COMx, at most 6 characters; "\\.\" is prepended */
    CHECK( c_serial_set_port_name( port, "COM1234" ), CSERIAL_ERROR_NAME_TOO_LONG );
    CHECK( c_serial_set_port_name( port, "COM250" ), CSERIAL_OK );
#else
    CHECK( c_serial_set_port_name( port, "/dev/does-not-exist" ), CSERIAL_OK );
#endif
    printf( "port name is now \"%s\"\n", c_serial_get_port_name( port ) );
    CHECK( c_serial_open( port ), CSERIAL_ERROR_NO_SUCH_SERIAL_PORT );

    CHECK( c_serial_set_baud_rate( port, -9600 ), CSERIAL_ERROR_INVALID_PARAM );
    CHECK( c_serial_set_data_bits( port, (enum CSerial_Data_Bits)9 ), CSERIAL_ERROR_INVALID_PARAM );
    CHECK( c_serial_set_parity( port, (enum CSerial_Parity)7 ), CSERIAL_ERROR_INVALID_PARAM );

    CHECK( c_serial_set_baud_rate( port, 57600 ), CSERIAL_OK );
    printf( "cached baud rate: %d\n", c_serial_get_baud_rate( port ) );

    CHECK( c_serial_set_flow_control( port, CSERIAL_FLOW_HARDWARE ), CSERIAL_OK );
    CHECK( c_serial_set_rts_control( port, CSERIAL_RTS_SOFTWARE ), CSERIAL_ERROR_INVALID_FLOW );
    printf( "RTS control stayed %d (CSERIAL_RTS_NONE)\n", c_serial_get_rts_control( port ) );
    CHECK( c_serial_set_flow_control( port, CSERIAL_FLOW_NONE ), CSERIAL_OK );
    CHECK( c_serial_set_rts_control( port, CSERIAL_RTS_SOFTWARE ), CSERIAL_OK );

    {
        char buf[ 4 ];
        int len = sizeof( buf );
        CHECK( c_serial_read_data_timeout( port, buf, &len, NULL, 0 ), CSERIAL_ERROR_NO_SUCH_SERIAL_PORT );
        len = 0;
        CHECK( c_serial_read_data_timeout( port, buf, &len, NULL, 0 ), CSERIAL_ERROR_INCORRECT_READ_PARAMS );
    }

    c_serial_free( port );
}

static void baud_rate_fallback( c_serial_port_type* port ){
    static const int wanted[] = { 250000, 1000000, 230400, 115200 };
    size_t i;

    printf( "--- non-standard baud rates\n" );
    for( i = 0; i < sizeof( wanted ) / sizeof( wanted[ 0 ] ); i++ ){
        int rc = c_serial_set_baud_rate( port, wanted[ i ] );
        int actual = c_serial_get_baud_rate( port );

        if( rc == CSERIAL_OK && actual == wanted[ i ] ){
            printf( "%d baud: OK\n", wanted[ i ] );
        }else if( rc == CSERIAL_OK ){
            printf( "%d baud: accepted, but the driver reports %d\n", wanted[ i ], actual );
        }else{
            ex_print_error( "    rejected", port, rc );
        }
    }
}

static void online_rules( const char* name ){
    c_serial_port_type* port;
    int rc;

    if( c_serial_new( &port, NULL ) != CSERIAL_OK )
        exit( 1 );

    printf( "--- %s\n", name );
    CHECK( c_serial_set_port_name( port, name ), CSERIAL_OK );

    /* 7 data bits, even parity, 1 stop bit: many industrial devices */
    c_serial_set_baud_rate( port, 9600 );
    c_serial_set_data_bits( port, CSERIAL_BITS_7 );
    c_serial_set_parity( port, CSERIAL_PARITY_EVEN );
    c_serial_set_stop_bits( port, CSERIAL_STOP_BITS_1 );

    rc = c_serial_open( port );
    if( rc != CSERIAL_OK ){
        /* e.g. a pty or a USB CDC device that only supports 8N1 */
        ex_print_error( "open with 7E1", port, rc );
        c_serial_set_data_bits( port, CSERIAL_BITS_8 );
        c_serial_set_parity( port, CSERIAL_PARITY_NONE );
        rc = c_serial_open( port );
        if( rc != CSERIAL_OK ){
            ex_print_error( "open with 8N1", port, rc );
            c_serial_free( port );
            failures++;
            return;
        }
    }
    printf( "opened: %d baud, %d data bits, parity %d, %d stop bits\n",
        c_serial_get_baud_rate( port ), c_serial_get_data_bits( port ),
        c_serial_get_parity( port ), c_serial_get_stop_bits( port ) );

    CHECK( c_serial_open( port ), CSERIAL_ERROR_ALREADY_OPEN );

    baud_rate_fallback( port );

    /* RS485 transceiver: drive RTS while transmitting.  BEST_AVAILABLE uses
     * the kernel RS485 mode where the driver has it and toggles RTS in
     * software otherwise, so it never fails at open. */
    CHECK( c_serial_set_rts_control( port, CSERIAL_RTS_BEST_AVAILABLE ), CSERIAL_OK );
    CHECK( c_serial_set_rts_control( port, CSERIAL_RTS_NONE ), CSERIAL_OK );

    /* Reopen without touching the line settings */
    c_serial_close( port );
    c_serial_set_baud_rate( port, 4800 );    /* cached only, not applied */
    rc = c_serial_open_keep_settings( port, 1 );
    if( rc == CSERIAL_OK )
        printf( "reopened keeping the driver settings: %d baud\n", c_serial_get_baud_rate( port ) );
    else
        ex_print_error( "open_keep_settings", port, rc );

    c_serial_free( port );
}

int main( int argc, char** argv ){
    offline_rules();
    if( argc > 1 )
        online_rules( argv[ 1 ] );

    printf( "--- %d unexpected results\n", failures );
    return failures == 0 ? 0 : 1;
}
