/*
   Copyright 2016 rm5248
   updated 2021 alexb@clever.team

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
 * Platform-independent part of the API: argument validation, cached
 * settings and the open/read/write/cancel sequences.  Everything that
 * touches the OS lives in the cserial_os_* backend.
 */

#include "c_serial_internal.h"

#include <stdlib.h>
#include <string.h>

/* c_serial_read_cancel() re-signals the reader at least this often (ms),
 * in case the reader had not reached its wait the first time */
#define CSERIAL_CANCEL_POLL_SLICE_MS 50

int64_t cserial_internal_remaining_ms(int timeout_msec, uint64_t start_timestamp) {
	uint64_t elapsed;

	if (timeout_msec < 0)
		return -1;

	elapsed = cserial_os_tick_count() - start_timestamp;
	if (elapsed >= (uint64_t)timeout_msec)
		return 0;

	return (int64_t)((uint64_t)timeout_msec - elapsed);
}

char* cserial_internal_strdup(const char* str) {
	size_t len = strlen(str) + 1;
	char* copy = (char*)malloc(len);
	if (copy != NULL)
		memcpy(copy, str, len);
	return copy;
}

/*
 * Local Methods
 */

/* Software RS485 (CSERIAL_RTS_SOFTWARE, or CSERIAL_RTS_BEST_AVAILABLE without
 * driver support): RTS is raised before every write to enable the
 * transmitter, and dropped after the data has been sent to release the bus.
 * Errors are ignored, a port without control lines (pty) just writes. */
static void c_serial_set_rts_if_required(c_serial_port_type* port) {
	c_serial_control_lines_type lines;

	if (port->rs485_is_software) {
		if (c_serial_get_control_lines(port, &lines) != CSERIAL_OK)
			return;
		lines.rts = 1;
		c_serial_set_control_line(port, &lines, 0);
	}
}

static void c_serial_clear_rts_if_required(c_serial_port_type* port) {
	c_serial_control_lines_type lines;

	if (port->rs485_is_software) {
		if (c_serial_get_control_lines(port, &lines) != CSERIAL_OK)
			return;
		lines.rts = 0;
		/* Wait until the last byte has left the UART, otherwise its tail
		 * would be cut off by turning the transmitter off */
		c_serial_flush(port);
		c_serial_set_control_line(port, &lines, 0);
	}
}

/* Apply port->rs485 to an open port: switch the driver RS485 mode on or off
 * and decide whether the writes toggle RTS themselves (rs485_is_software) */
static int set_rts_settings(c_serial_port_type* port) {
	int retval = CSERIAL_ERROR_GENERIC;

	if( port->rs485 == CSERIAL_RTS_NONE ){
		port->rs485_is_software = 0;
		retval = cserial_os_clear_rts_hardware( port );
	}else if( port->rs485 == CSERIAL_RTS_HARDWARE ){
		port->rs485_is_software = 0;
		retval = cserial_os_set_rts_hardware( port );
	}else if( port->rs485 == CSERIAL_RTS_SOFTWARE ){
		/* The driver must not toggle RTS at the same time */
		port->rs485_is_software = 1;
		retval = cserial_os_clear_rts_hardware( port );
	}else if( port->rs485 == CSERIAL_RTS_BEST_AVAILABLE ){
		port->rs485_is_software = 0;
		retval = cserial_os_set_rts_hardware( port );
		if( retval != CSERIAL_OK ){
			port->rs485_is_software = 1;
			retval = CSERIAL_OK;
		}
	}

	return retval;
}

/**
 * Check to make sure that the RTS handling can be set.
 * If RS485 handling is set, hardware flow control cannot be used: both
 * would drive the same RTS line (RTS/CTS handshake vs transmitter enable).
 */
static int check_rts_handling( 
	c_serial_port_type* port){
	if( port->flow == CSERIAL_FLOW_HARDWARE &&
		port->rs485 != CSERIAL_RTS_NONE ){
		return CSERIAL_ERROR_INVALID_FLOW;
	}

	return CSERIAL_OK;
}

/**
 * Set flow control and the RTS handling.  Always applied together since both
 * use the RTS line.
 */
static int set_flow_control( c_serial_port_type* desc ) {
	int rc;

	rc = check_rts_handling( desc );
	if( rc != CSERIAL_OK )
		return rc;

	rc = cserial_os_set_flow_control( desc, desc->flow );
	if( rc != CSERIAL_OK )
		return rc;

	return set_rts_settings( desc );
}

/*
 * Method Implementations
 */

int c_serial_new( c_serial_port_type** port, c_serial_errnum_t* errnum ) {
	c_serial_port_type* new_port;

	if( port == NULL ) {
		return CSERIAL_ERROR_CANT_CREATE;
	}
	*port = NULL;

	new_port = (c_serial_port_type*)malloc( sizeof( struct c_serial_port ) );
	if( new_port == NULL ) {
		return CSERIAL_ERROR_CANT_CREATE;
	}

	/* Clear our memory and set some sane defaults */
	memset( new_port, 0, sizeof( struct c_serial_port ) );
	new_port->baud_rate = 9600;
	new_port->data_bits = CSERIAL_BITS_8;
	new_port->stop_bits = CSERIAL_STOP_BITS_1;
	new_port->parity = CSERIAL_PARITY_NONE;
	new_port->flow = CSERIAL_FLOW_NONE;
	new_port->rs485 = CSERIAL_RTS_NONE;
	new_port->rs485_is_software = 0;

	if( cserial_os_init( new_port, errnum ) != CSERIAL_OK ) {
		free( new_port );
		return CSERIAL_ERROR_CANT_CREATE;
	}

	*port = new_port;

	return CSERIAL_OK;
}

void c_serial_free( c_serial_port_type* port ) {
	if( port == NULL ) 
		return;

	c_serial_close( port );
	cserial_os_destroy( port );

	free( port->port_name );
	port->port_name = NULL;

	free( port );
}

/* is_closing stops a reader from starting a new read while the port is going
 * away (see c_serial_read_data_timeout()); the running read is cancelled and
 * waited for before the descriptor is closed, so it never uses a freed or
 * reused handle. */
void c_serial_close( c_serial_port_type* port ) {
	if( port == NULL ) {
		return;
	}

	cserial_atomic_exchange( &port->is_closing, 1 );
	c_serial_read_cancel( port, CSERIAL_TIMEOUT_INFINITE );
	cserial_os_close( port );
	cserial_atomic_exchange( &port->is_closing, 0 );
}

int c_serial_open( c_serial_port_type* port ) {
	return c_serial_open_keep_settings( port, 0 );
}

int c_serial_open_keep_settings( 
	c_serial_port_type* port, 
	int keep_settings ) {

	int retval;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if( port->is_open ) 
		return CSERIAL_ERROR_ALREADY_OPEN;
	if( port->port_name == NULL ) 
		return CSERIAL_ERROR_NO_PORT;
	retval = check_rts_handling( port );
	if( retval != CSERIAL_OK ){
		return retval;
	}

	retval = cserial_os_open( port );
	if( retval != CSERIAL_OK )
		return retval;

	port->is_open = 1;

	/* Apply the cached settings (raw mode first, it resets the whole line
	 * discipline).  If there are any errors, close the port and bail out.
	 * keep_settings leaves whatever the OS or a previous user configured.
	 */
	do {
		if (keep_settings)
			break;

		retval = cserial_os_set_raw_input(port);
		if (retval)
			break;
		retval = cserial_os_set_data_bits(port, port->data_bits);
		if (retval)
			break;
		retval = cserial_os_set_stop_bits(port, port->stop_bits);
		if (retval)
			break;
		retval = cserial_os_set_parity(port, port->parity);
		if (retval)
			break;
		retval = set_flow_control(port);
		if (retval)
			break;
		/* Baud rate last: non-standard rates use an ioctl which
		 * could be overwritten by subsequent tcsetattr calls. */
		retval = cserial_os_set_baud_rate(port, port->baud_rate);
	} while (0);

	if (retval != CSERIAL_OK)
		c_serial_close(port);

	return retval;
}

int c_serial_is_open( c_serial_port_type* port ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	return port->is_open;
}

int c_serial_set_port_name( c_serial_port_type* port,
							const char* port_name ) {
	size_t name_len;
	size_t prefix_len;
	char* new_name;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (port_name == NULL || port_name[0] == '\0')
		return CSERIAL_ERROR_INVALID_PARAM;

	name_len = strlen( port_name );
	if( name_len > cserial_os_port_name_max_length )
		return CSERIAL_ERROR_NAME_TOO_LONG;

	prefix_len = strlen( cserial_os_port_name_prefix );
	new_name = (char*)malloc( prefix_len + name_len + 1 );
	if (!new_name)
		return CSERIAL_ERROR_NO_MEMORY;

	memcpy( new_name, cserial_os_port_name_prefix, prefix_len );
	memcpy( new_name + prefix_len, port_name, name_len + 1 );

	free( port->port_name );
	port->port_name = new_name;

	return CSERIAL_OK;
}

const char* c_serial_get_port_name( 
	c_serial_port_type* port) {

	if( port == NULL ) 
		return NULL;

	return port->port_name;
}

int c_serial_set_baud_rate(
	c_serial_port_type* port,
	int baud ) {

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (!cserial_os_is_valid_baud_rate(baud))
		return CSERIAL_ERROR_INVALID_PARAM;

	port->baud_rate = baud;

	if( port->is_open ) {
		return cserial_os_set_baud_rate( port, port->baud_rate );
	}

	return CSERIAL_OK;
}

int c_serial_get_baud_rate(c_serial_port_type* port ) {
	int baud;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if( !port->is_open )
		return port->baud_rate;

	baud = cserial_os_get_baud_rate( port );
	if( baud >= 0 )
		port->baud_rate = baud;
	return baud;
}

int c_serial_set_data_bits( c_serial_port_type* port,
							enum CSerial_Data_Bits bits ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (bits < CSERIAL_BITS_5 || bits > CSERIAL_BITS_8)
		return CSERIAL_ERROR_INVALID_PARAM;

	port->data_bits = bits;
	if( port->is_open ) {
		return cserial_os_set_data_bits( port, port->data_bits );
	}

	return CSERIAL_OK;
}

enum CSerial_Data_Bits c_serial_get_data_bits( c_serial_port_type* port ) {
	if (port == NULL)
		return (enum CSerial_Data_Bits)CSERIAL_ERROR_INVALID_PORT;

	if (!port->is_open)
		return port->data_bits;

	return (enum CSerial_Data_Bits)cserial_os_get_data_bits( port );
}

int c_serial_set_stop_bits( c_serial_port_type* port,
							enum CSerial_Stop_Bits bits ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (bits != CSERIAL_STOP_BITS_1 && bits != CSERIAL_STOP_BITS_2)
		return CSERIAL_ERROR_INVALID_PARAM;

	port->stop_bits = bits;
	if( port->is_open ) {
		return cserial_os_set_stop_bits( port, port->stop_bits );
	}

	return CSERIAL_OK;
}

enum CSerial_Stop_Bits c_serial_get_stop_bits( c_serial_port_type* port ) {
	if (port == NULL)
		return (enum CSerial_Stop_Bits)CSERIAL_ERROR_INVALID_PORT;

	if (!port->is_open)
		return port->stop_bits;

	return (enum CSerial_Stop_Bits)cserial_os_get_stop_bits( port );
}

int c_serial_set_parity( c_serial_port_type* port,
						 enum CSerial_Parity parity ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (parity != CSERIAL_PARITY_NONE && parity != CSERIAL_PARITY_ODD && parity != CSERIAL_PARITY_EVEN)
		return CSERIAL_ERROR_INVALID_PARAM;

	port->parity = parity;
	if( port->is_open ) {
		return cserial_os_set_parity( port, port->parity );
	}

	return CSERIAL_OK;
}

enum CSerial_Parity c_serial_get_parity( c_serial_port_type* port ) {
	if (port == NULL)
		return (enum CSerial_Parity)CSERIAL_ERROR_INVALID_PORT;

	if (!port->is_open)
		return port->parity;

	return (enum CSerial_Parity)cserial_os_get_parity( port );
}

int c_serial_set_flow_control( c_serial_port_type* port,
							   enum CSerial_Flow_Control control ) {
	int ok = CSERIAL_OK;
	enum CSerial_Flow_Control old;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (control != CSERIAL_FLOW_NONE && control != CSERIAL_FLOW_HARDWARE && control != CSERIAL_FLOW_SOFTWARE)
		return CSERIAL_ERROR_INVALID_PARAM;

	old = port->flow;
	port->flow = control;

	ok = check_rts_handling( port );
	if( ok != CSERIAL_OK ){
		port->flow = old;
		CSERIALDBG( "Unable to set flow control: invalid flow "
				   "control has been specified.  Ignoring." );
		return ok;
	}

	if( port->is_open ) {
		ok = set_flow_control( port );
	}

	return ok;
}

enum CSerial_Flow_Control c_serial_get_flow_control( 
	c_serial_port_type* port) {

	if (port == NULL)
		return (enum CSerial_Flow_Control)CSERIAL_ERROR_INVALID_PORT;

	if (!port->is_open)
		return port->flow;

	return (enum CSerial_Flow_Control)cserial_os_get_flow_control( port );
}

static int c_serial_write_internal(c_serial_port_type* port,
								   const void* data,
								   int* length,
								   int timeout_msec) {
	int ret_code;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (length == NULL || *length < 0 || (data == NULL && *length > 0))
		return CSERIAL_ERROR_INVALID_PARAM;

	if (!port->is_open)
		return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;

	if (*length == 0)
		return CSERIAL_OK;

	c_serial_set_rts_if_required( port );
	ret_code = cserial_os_write( port, data, length, timeout_msec );
	c_serial_clear_rts_if_required( port );

	return ret_code;
}

int c_serial_write_data(c_serial_port_type* port,
						 const void *data,
						 int* length ) {
	return c_serial_write_internal(port, data, length, CSERIAL_TIMEOUT_INFINITE);
}

int c_serial_write_data_timeout(
	c_serial_port_type* port,
	const void* data,
	int* length,
	int timeout_msec) {
	return c_serial_write_internal(port, data, length, timeout_msec);
}

int c_serial_read_data_timeout(
	c_serial_port_type* port,
	void* data,
	int* data_length,
	c_serial_control_lines_type* lines,
	int timeout_msec) {

	int ret_code;
	uint64_t start_timestamp = cserial_os_tick_count();

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (data != NULL && (data_length == NULL || *data_length <= 0))
		return CSERIAL_ERROR_INCORRECT_READ_PARAMS;

	if (data == NULL && lines == NULL)
		return CSERIAL_ERROR_INCORRECT_READ_PARAMS;

	/* Reset before announcing the reader, so that a cancel issued
	 * after is_read_active becomes visible is never lost. */
	cserial_os_cancel_read_reset(port);
	cserial_atomic_exchange(&port->is_read_active, 1);

	/* Checked after announcing the reader: either c_serial_close() sees
	 * is_read_active and cancels us, or we see is_closing here. */
	if (cserial_atomic_read(&port->is_closing) || !port->is_open) {
		cserial_atomic_exchange(&port->is_read_active, 0);
		if (data_length != NULL)
			*data_length = 0;
		return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
	}

	ret_code = cserial_os_read(port, data, data_length, lines, timeout_msec, start_timestamp);

	cserial_atomic_exchange(&port->is_read_active, 0);
	return ret_code;
}


int c_serial_read_data( c_serial_port_type* port,
						void* data,
						int* data_length,
						c_serial_control_lines_type* lines ) {
	return c_serial_read_data_timeout(port, data, data_length, lines, CSERIAL_TIMEOUT_INFINITE);
}

int c_serial_read_cancel(c_serial_port_type* port, int timeout_msec) {

	int ret_code = CSERIAL_OK;
	uint64_t start_timestamp = cserial_os_tick_count();

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	while (cserial_atomic_read(&port->is_read_active)) {
		int64_t remaining;
		int64_t slice;

		cserial_os_cancel_read_signal(port);

		remaining = cserial_internal_remaining_ms(timeout_msec, start_timestamp);
		if (remaining == 0) {
			ret_code = CSERIAL_ERROR_TIMEOUT;
			break;
		}

		/* Wait in slices and re-signal: the reader may still be starting up */
		slice = (remaining < 0 || remaining > CSERIAL_CANCEL_POLL_SLICE_MS) ? CSERIAL_CANCEL_POLL_SLICE_MS : remaining;
		cserial_os_wait_for_reader(port, slice);
	}

	if (ret_code == CSERIAL_OK)
		cserial_os_cancel_read_reset(port);

	return ret_code;
}

int c_serial_get_native_handle( c_serial_port_type* port, c_serial_handle_t* out_handle) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (!port->is_open)
		return CSERIAL_ERROR_NO_PORT;

	if (out_handle)
		*out_handle = port->port;

	return CSERIAL_OK;
}

int c_serial_get_poll_handle(c_serial_port_type* port, c_serial_handle_t* out_handle){
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (!port->is_open)
		return CSERIAL_ERROR_NO_PORT;

	if (out_handle)
		*out_handle = cserial_os_poll_handle(port);

	return CSERIAL_OK;
}

int c_serial_set_control_line( c_serial_port_type* port,
							   c_serial_control_lines_type* lines,
							   int return_state ) {
	int rc;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if( lines == NULL ) 
		return CSERIAL_ERROR_INVALID_PARAM;

	rc = cserial_os_set_control_lines( port, lines );
	if( rc != CSERIAL_OK )
		return rc;

	if( return_state ) {
		return c_serial_get_control_lines( port, lines );
	}

	return CSERIAL_OK;
}

int c_serial_get_control_lines( c_serial_port_type* port,
								c_serial_control_lines_type* lines ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if( lines == NULL ) 
		return CSERIAL_ERROR_INVALID_PARAM;

	memset( lines, 0, sizeof( c_serial_control_lines_type ) );

	return cserial_os_get_control_lines( port, lines );
}

int c_serial_get_available( c_serial_port_type* port,
							int* available ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (available == NULL)
		return CSERIAL_ERROR_INVALID_PARAM;

	return cserial_os_get_available( port, available );
}

int c_serial_set_serial_line_change_flags( c_serial_port_type* port,
		int flags ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	port->line_flags = flags;

	return CSERIAL_OK;
}

int c_serial_get_serial_line_change_flags( c_serial_port_type* port ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	return port->line_flags;
}

void c_serial_set_user_data( c_serial_port_type* port, void* data ) {
	if (port == NULL)
		return;

	port->user_data = data;
}

void* c_serial_get_user_data( c_serial_port_type* port ) {
	if(port == NULL)
		return NULL;

	return port->user_data;
}

const char* c_serial_get_error_string( int errnum ) {
	switch( errnum ) {
	case CSERIAL_OK:
		return "OK";
	case CSERIAL_ERROR_GENERIC:
		return "Generic error";
	case CSERIAL_ERROR_INVALID_PORT:
		return "Invalid port(was NULL)";
	case CSERIAL_ERROR_NOT_A_SERIAL_PORT:
		return "Attempting to open something that is not a serial port";
	case CSERIAL_ERROR_NO_SUCH_SERIAL_PORT:
		return "No such serial port";
	case CSERIAL_ERROR_INCORRECT_READ_PARAMS:
		return "Invalid parameters to read from serial port";
	case CSERIAL_ERROR_CANT_CREATE:
		return "Unable to create serial port";
	case CSERIAL_ERROR_NO_PORT:
		return "No port name defined to open";
	case CSERIAL_ERROR_ALREADY_OPEN:
		return "Port already open";
	case CSERIAL_ERROR_NAME_TOO_LONG:
		return "Port name is too long";
	case CSERIAL_ERROR_INVALID_FLOW:
		return "Flow control conflicts with RTS handling";
	case CSERIAL_RTS_TYPE_NOT_AVAILABLE:
		return "RTS handling type is not available";
	case CSERIAL_ERROR_TIMEOUT:
		return "Timeout";
	case CSERIAL_ERROR_CANCELLED:
		return "Operation cancelled";
	case CSERIAL_ERROR_NO_MEMORY:
		return "No memory";
	case CSERIAL_ERROR_INVALID_PARAM:
		return "Invalid parameter";
	default:
		return "Unknown error";
	}
}

c_serial_errnum_t c_serial_get_last_native_errnum( c_serial_port_type* port ) {
	if (port == NULL)
		return (c_serial_errnum_t)CSERIAL_ERROR_INVALID_PORT;

	return port->last_errnum;
}

const char** c_serial_get_serial_ports_list( void ) {
	char** port_names;
	int count;

	port_names = (char**)calloc( CSERIAL_MAX_PORTS + 1, sizeof( char* ) );
	if (!port_names)
		return NULL;

	count = cserial_os_list_ports( port_names, CSERIAL_MAX_PORTS );
	if( count < 0 ) {
		c_serial_free_serial_ports_list( (const char**)port_names );
		return NULL;
	}

	port_names[count] = NULL;

	return (const char**)port_names;
}

void c_serial_free_serial_ports_list( const char** port_list ) {
	int x;

	if( port_list == NULL )
		return;

	for( x = 0; x <= CSERIAL_MAX_PORTS; x++ ) {
		if( port_list[x] == NULL ) {
			break;
		}
		free( (void*)port_list[x] );
	}

	free( (void*)port_list );
}

int c_serial_set_rts_control( c_serial_port_type* port,
							  enum CSerial_RTS_Handling handling ){
	int ok = CSERIAL_OK;
	enum CSerial_RTS_Handling old;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if (handling < CSERIAL_RTS_NONE || handling > CSERIAL_RTS_BEST_AVAILABLE)
		return CSERIAL_ERROR_INVALID_PARAM;

	old = port->rs485;
	port->rs485 = handling;

	ok = check_rts_handling( port );
	if( ok != CSERIAL_OK ){
		port->rs485 = old;
		CSERIALDBG( "Unable to set RTS handling: invalid flow "
				   "control has been specified.  Ignoring." );
		return ok;
	}

	if( port->is_open ) {
		ok = set_flow_control( port );
		if( ok != CSERIAL_OK ){
			/* Keep the previous mode, a rejected one would make every
			 * later c_serial_open() fail */
			port->rs485 = old;
			set_rts_settings( port );
		}
	}

	return ok;
}

enum CSerial_RTS_Handling c_serial_get_rts_control( c_serial_port_type* port ){
	if (port == NULL)
		return (enum CSerial_RTS_Handling)CSERIAL_ERROR_INVALID_PORT;

	return port->rs485;
}

int c_serial_flush( c_serial_port_type* port ){
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	return cserial_os_flush( port );
}
