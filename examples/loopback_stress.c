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
 * Bulk transfer with verification, writer and reader in different threads:
 *
 *   loopback_stress <port> [bytes] [baud]            TX wired to RX on one port
 *   loopback_stress <tx port> <rx port> [bytes] [baud] two connected ports
 *                                                    (null-modem cable,
 *                                                    com0com, socat pty pair)
 *
 * Things to keep in mind:
 *  - One thread may read while another one writes the same port.  Do not
 *    read (or write) from two threads at once, and do not close the port
 *    while a write is in progress.
 *  - The transfer is much larger than the driver buffers (typically 4 KiB):
 *    writes block or come back partial, and reads return whatever chunk has
 *    arrived.  Neither side may assume message boundaries.
 *  - Without flow control the receiver must keep up, or the driver drops
 *    bytes on overrun.  Real hardware at high baud rates may need
 *    CSERIAL_FLOW_HARDWARE.
 *  - A stall (cable pulled, flow control blocked forever) is detected by
 *    timeouts, not by an error code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "example_util.h"

#define STALL_TIMEOUT_MS 3000

struct writer_ctx {
    c_serial_port_type* port;
    long total;
    int status;
};

/* Position-dependent pattern that also catches lost or duplicated bytes */
static unsigned char pattern_byte( long pos ){
    return (unsigned char)( ( pos * 7 + ( pos >> 8 ) ) & 0xFF );
}

static void writer_thread( void* arg ){
    struct writer_ctx* ctx = (struct writer_ctx*)arg;
    unsigned char chunk[ 1024 ];
    long sent = 0;

    ctx->status = CSERIAL_OK;
    while( sent < ctx->total ){
        int len = ctx->total - sent < (long)sizeof( chunk ) ? (int)( ctx->total - sent ) : (int)sizeof( chunk );
        int offset = 0;
        int i;

        for( i = 0; i < len; i++ )
            chunk[ i ] = pattern_byte( sent + i );

        /* Partial writes: resend the rest of the chunk */
        while( offset < len ){
            int part = len - offset;
            int rc = c_serial_write_data_timeout( ctx->port, chunk + offset, &part, STALL_TIMEOUT_MS );

            offset += part;
            if( rc != CSERIAL_OK ){
                ctx->status = rc;
                return;
            }
        }
        sent += len;
    }
}

int main( int argc, char** argv ){
    c_serial_port_type* tx;
    c_serial_port_type* rx;
    struct writer_ctx writer;
    ex_thread_t thread;
    int two_ports;
    long total;
    long received = 0;
    int baud;
    int arg;
    int rc;
    uint64_t start;
    unsigned char buf[ 512 ];

    if( argc < 2 ){
        fprintf( stderr, "usage: %s <port> [rx port] [bytes] [baud]\n", argv[ 0 ] );
        return 1;
    }

    /* A second argument that is not a number is the receiving port */
    two_ports = argc > 2 && ( argv[ 2 ][ 0 ] < '0' || argv[ 2 ][ 0 ] > '9' );
    arg = two_ports ? 3 : 2;
    total = argc > arg ? atol( argv[ arg ] ) : 64 * 1024;
    baud = argc > arg + 1 ? atoi( argv[ arg + 1 ] ) : 115200;

    if( c_serial_new( &tx, NULL ) != CSERIAL_OK )
        return 1;
    rc = c_serial_set_port_name( tx, argv[ 1 ] );
    if( rc == CSERIAL_OK )
        rc = c_serial_set_baud_rate( tx, baud );
    if( rc == CSERIAL_OK )
        rc = c_serial_open( tx );
    if( rc != CSERIAL_OK ){
        ex_print_error( "open tx", tx, rc );
        c_serial_free( tx );
        return 1;
    }

    rx = tx;
    if( two_ports ){
        if( c_serial_new( &rx, NULL ) != CSERIAL_OK ){
            c_serial_free( tx );
            return 1;
        }
        rc = c_serial_set_port_name( rx, argv[ 2 ] );
        if( rc == CSERIAL_OK )
            rc = c_serial_set_baud_rate( rx, baud );
        if( rc == CSERIAL_OK )
            rc = c_serial_open( rx );
        if( rc != CSERIAL_OK ){
            ex_print_error( "open rx", rx, rc );
            c_serial_free( rx );
            c_serial_free( tx );
            return 1;
        }
    }

    writer.port = tx;
    writer.total = total;
    writer.status = CSERIAL_OK;
    start = ex_now_ms();
    if( ex_thread_create( &thread, writer_thread, &writer ) != 0 ){
        fprintf( stderr, "cannot create thread\n" );
        return 1;
    }

    rc = CSERIAL_OK;
    while( received < total ){
        int len = sizeof( buf );
        int i;

        rc = c_serial_read_data_timeout( rx, buf, &len, NULL, STALL_TIMEOUT_MS );
        if( rc == CSERIAL_ERROR_TIMEOUT ){
            fprintf( stderr, "stalled: nothing received for %d ms\n", STALL_TIMEOUT_MS );
            break;
        }
        if( rc != CSERIAL_OK ){
            ex_print_error( "read", rx, rc );
            break;
        }
        if( len == 0 ){
            fprintf( stderr, "the other side hung up\n" );
            rc = CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
            break;
        }

        for( i = 0; i < len; i++ ){
            if( buf[ i ] != pattern_byte( received + i ) ){
                fprintf( stderr, "data mismatch at byte %ld\n", received + i );
                rc = CSERIAL_ERROR_GENERIC;
                break;
            }
        }
        if( rc != CSERIAL_OK )
            break;
        received += len;
    }

    /* c_serial_close() only stops readers: never close a port that another
     * thread is writing to.  If the reader gave up early, the writer stops
     * by itself once its write times out. */
    ex_thread_join( thread );

    if( writer.status != CSERIAL_OK && received < total )
        ex_print_error( "write", tx, writer.status );

    {
        uint64_t elapsed = ex_now_ms() - start;
        printf( "%ld of %ld bytes verified in %d ms (%.0f bytes/s)\n", received, total,
            (int)elapsed, elapsed > 0 ? received * 1000.0 / (double)elapsed : 0.0 );
    }

    if( two_ports )
        c_serial_free( rx );
    c_serial_free( tx );
    return received == total ? 0 : 1;
}
