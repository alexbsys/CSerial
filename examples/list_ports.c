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
 * Enumerating serial ports.
 *
 *   list_ports            print the ports found on the system
 *   list_ports --probe    also try to open each of them
 *
 * Things to keep in mind:
 *  - NULL from c_serial_get_serial_ports_list() means out of memory.  An
 *    empty list is a normal result: no hardware, or a sandboxed iOS/Android
 *    app that cannot see /dev.
 *  - The list is a snapshot.  A USB adapter can be unplugged (or a new one
 *    plugged in) right after the call.
 *  - Being listed does not mean the port can be opened: it may be in use
 *    or the user may lack permissions (on Linux: the "dialout" group).
 *  - Opening a port has side effects on the device: DTR/RTS are asserted,
 *    which resets e.g. most Arduino boards.  That is why probing is opt-in.
 */

#include <stdio.h>
#include <string.h>

#include "example_util.h"

static void probe( const char* name ){
    c_serial_port_type* port;
    int rc;

    rc = c_serial_new( &port, NULL );
    if( rc != CSERIAL_OK ){
        ex_print_error( "c_serial_new", NULL, rc );
        return;
    }

    rc = c_serial_set_port_name( port, name );
    if( rc == CSERIAL_OK )
        rc = c_serial_open( port );

    if( rc == CSERIAL_OK )
        printf( "    can be opened\n" );
    else
        ex_print_error( "    cannot be opened", port, rc );

    c_serial_free( port );
}

int main( int argc, char** argv ){
    int do_probe = argc > 1 && strcmp( argv[ 1 ], "--probe" ) == 0;
    const char** ports;
    int count;

    ports = c_serial_get_serial_ports_list();
    if( ports == NULL ){
        fprintf( stderr, "Out of memory\n" );
        return 1;
    }

    for( count = 0; ports[ count ] != NULL; count++ ){
        printf( "%s\n", ports[ count ] );
        if( do_probe )
            probe( ports[ count ] );
    }

    if( count == 0 )
        printf( "No serial ports found\n" );

    c_serial_free_serial_ports_list( ports );
    return 0;
}
