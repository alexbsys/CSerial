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
 * Internal interface between the platform-independent API (c_serial.c) and
 * the OS backends (c_serial_win.c, c_serial_posix.c).
 */
#ifndef C_SERIAL_INTERNAL_H
#define C_SERIAL_INTERNAL_H

#include "config.h"
#include "c_serial_pdetect.h"
#include "c_serial_atomic.h"

#include <stddef.h>
#include <stdint.h>

#include <cserial/c_serial.h>

#ifdef CSERIAL_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#endif

#if defined(CSERIAL_DEBUG_PRINT) && CSERIAL_DEBUG_PRINT==1
#include <stdio.h>
#define CSERIALDBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define CSERIALDBG(...) do {} while (0)
#endif

/*
 * State owned by the OS backend.  c_serial.c never touches these fields.
 */
struct c_serial_os_state {
#ifdef CSERIAL_PLATFORM_WINDOWS
	/* Held by a reader for the whole read and by cserial_os_close() while
	 * closing the handle.  Writes do not take it. */
	HANDLE mutex;
	/* Manual-reset event: set to cancel the running read, which waits on it
	 * together with the I/O event */
	HANDLE cancel_read_event;
	/* Overlapped I/O state for reads (also used by WaitCommEvent) and writes.
	 * Separate structures let one thread read while another one writes. */
	OVERLAPPED read_overlap;
	OVERLAPPED write_overlap;
	/* Last DTR/RTS values set through cserial_os_set_control_lines():
	 * Windows cannot read the state of the output lines back */
	int dtr;
	int rts;
#else
	/* Held by a reader during each poll slice (it is released between the
	 * slices so that cancel and close can get in) and by cserial_os_close() */
	pthread_mutex_t mutex;
	/* 1 = the running read must return CSERIAL_ERROR_CANCELLED; the reader
	 * checks it once per poll slice */
	atomic_long_type cancel_read;
	/* The baud rate is not a standard Bxxx value and was set with
	 * termios2 (Linux) or IOSSIOSPEED (macOS).  A plain tcsetattr() would
	 * reset it, so every termios update re-applies port->baud_rate. */
	int custom_baud_active;
#endif
};

/* \cond */
struct c_serial_port {
	/* OS handle of the open port: a file descriptor on POSIX, a HANDLE opened
	 * with FILE_FLAG_OVERLAPPED on Windows.  -1 / INVALID_HANDLE_VALUE while
	 * closed. */
	c_serial_handle_t port;
	/* errno / GetLastError() of the last failed system call, returned by
	 * c_serial_get_last_native_errnum().  Not cleared on success. */
	c_serial_errnum_t last_errnum;
	/* Name passed to open(): cserial_os_port_name_prefix + user name, e.g.
	 * "/dev/ttyUSB0" or "\\.\COM3".  malloc'ed, NULL until set. */
	char* port_name;

	/* Requested line settings.  Setters store them here and, if the port is
	 * open, apply them at once; c_serial_open() applies all of them.  While
	 * the port is open the getters ask the driver instead. */
	int baud_rate;
	enum CSerial_Data_Bits data_bits;
	enum CSerial_Stop_Bits stop_bits;
	enum CSerial_Parity parity;
	enum CSerial_Flow_Control flow;

	/* RTS handling for RS485 transceivers as requested by the user.  It cannot
	 * be combined with CSERIAL_FLOW_HARDWARE, which also drives RTS. */
	enum CSerial_RTS_Handling rs485;
	/* 1 = the library raises RTS around each write itself: either
	 * CSERIAL_RTS_SOFTWARE was requested, or CSERIAL_RTS_BEST_AVAILABLE found
	 * no driver support.  0 = the driver handles RTS, or nobody does. */
	int rs485_is_software;

	/* Opaque pointer for the application (c_serial_set_user_data) */
	void* user_data;
	/* 1 between a successful cserial_os_open() and cserial_os_close().
	 * Cleared by cserial_os_close() under os.mutex. */
	int is_open;
	/* CSERIAL_LINE_FLAG_* mask: changes of these lines end a read */
	int line_flags;

	/* 1 while a thread is inside c_serial_read_data_timeout().
	 * c_serial_read_cancel() keeps signalling until it drops back to 0. */
	atomic_long_type is_read_active;
	/* Set for the duration of c_serial_close(): new reads fail immediately,
	 * so a reader retrying in a loop cannot keep close waiting forever */
	atomic_long_type is_closing;

	struct c_serial_os_state os;
};
/* \endcond */

/*
 * Helpers shared by all backends (c_serial.c)
 */

/* Milliseconds left until timeout_msec (counted from start_timestamp, a
 * cserial_os_tick_count() value) expires: 0 when it has expired, -1 for an
 * infinite (negative) timeout */
int64_t cserial_internal_remaining_ms(int timeout_msec, uint64_t start_timestamp);
/* strdup() replacement (not in C89/MSVC without POSIX names); NULL if out of memory */
char* cserial_internal_strdup(const char* str);

/*
 * OS backend interface
 *
 * Unless stated otherwise the functions return CSERIAL_OK or a CSERIAL_ERROR_*
 * code and store the OS error in port->last_errnum.  c_serial.c has already
 * checked the arguments and, for functions on an open port, that the port is
 * open.
 */

/* What c_serial_set_port_name() prepends to the user's name: "\\.\" on
 * Windows (so that COM10 and above open), "" on POSIX */
extern const char cserial_os_port_name_prefix[];
/* Longest name the user may pass, without the prefix: 6 on Windows
 * ("COM256"), 255 on POSIX.  Longer names give CSERIAL_ERROR_NAME_TOO_LONG. */
extern const size_t cserial_os_port_name_max_length;

/* Monotonic milliseconds for timeouts; unaffected by changes of the wall clock */
uint64_t cserial_os_tick_count(void);
/* Whether the OS accepts this value at all (> 0 on Windows, >= 0 on POSIX
 * where 0 means hang up).  Whether the driver supports it is only known
 * when it is applied. */
int cserial_os_is_valid_baud_rate(int baud);

/* Create the mutex/events of a new port and set port->port to the closed
 * value.  On failure everything created so far is released and the OS error
 * is stored in *errnum (if not NULL). */
int cserial_os_init(c_serial_port_type* port, c_serial_errnum_t* errnum);
/* Release what cserial_os_init() created; the port is already closed */
void cserial_os_destroy(c_serial_port_type* port);

/* Open port->port_name into port->port.  Returns
 * CSERIAL_ERROR_NO_SUCH_SERIAL_PORT if it does not exist and
 * CSERIAL_ERROR_NOT_A_SERIAL_PORT if it is not a serial device (the handle is
 * then closed again).  Does not change any line setting. */
int cserial_os_open(c_serial_port_type* port);
/* Under os.mutex: clear is_open, close the handle and reset it to the closed
 * value.  The caller has cancelled any read; a closed port is a no-op. */
void cserial_os_close(c_serial_port_type* port);

/*
 * Settings of an open port.  The setters change the driver configuration; the
 * getters return the value reported by the driver (an enum value or a baud
 * rate), or a negative error code.
 */

/* Put the port into raw binary mode: no echo, no line editing, no character
 * translation, no flow control, reads that return whatever is buffered */
int cserial_os_set_raw_input(c_serial_port_type* port);
int cserial_os_set_baud_rate(c_serial_port_type* port, int baud);
int cserial_os_get_baud_rate(c_serial_port_type* port);
int cserial_os_set_data_bits(c_serial_port_type* port, enum CSerial_Data_Bits bits);
int cserial_os_get_data_bits(c_serial_port_type* port);
int cserial_os_set_stop_bits(c_serial_port_type* port, enum CSerial_Stop_Bits bits);
int cserial_os_get_stop_bits(c_serial_port_type* port);
int cserial_os_set_parity(c_serial_port_type* port, enum CSerial_Parity parity);
int cserial_os_get_parity(c_serial_port_type* port);
int cserial_os_set_flow_control(c_serial_port_type* port, enum CSerial_Flow_Control flow);
int cserial_os_get_flow_control(c_serial_port_type* port);
/* Let the driver raise RTS while transmitting (RS485): TIOCSRS485 on Linux,
 * RTS_CONTROL_TOGGLE on Windows.  Returns CSERIAL_RTS_TYPE_NOT_AVAILABLE
 * where the port or the platform has no such mode, so that
 * CSERIAL_RTS_BEST_AVAILABLE falls back to software RTS. */
int cserial_os_set_rts_hardware(c_serial_port_type* port);
/* Turn that driver mode off again, so that RTS can be set by hand.  Must
 * succeed on ports that never supported it and must not touch the RTS
 * handshake of hardware flow control. */
int cserial_os_clear_rts_hardware(c_serial_port_type* port);

/* Write up to *length bytes, waiting at most timeout_msec (< 0: forever) for
 * the port to accept data.  *length is set to the bytes written, also on
 * CSERIAL_ERROR_TIMEOUT; a short write is not an error. */
int cserial_os_write(c_serial_port_type* port, const void* data, int* length, int timeout_msec);
/* Wait for data (if data != NULL) and/or a change of a line in
 * port->line_flags, then read what is buffered (at most *data_length bytes).
 * Returns CSERIAL_ERROR_TIMEOUT when timeout_msec, counted from
 * start_timestamp, expires, CSERIAL_ERROR_CANCELLED when cancelled, and
 * CSERIAL_ERROR_NO_SUCH_SERIAL_PORT when the port was closed meanwhile.
 * *data_length is the number of bytes read (0 on errors); lines, if not
 * NULL, receives the line state on success (POSIX sets lines->unsupported
 * for ports without modem lines instead of failing). */
int cserial_os_read(c_serial_port_type* port, void* data, int* data_length,
	c_serial_control_lines_type* lines, int timeout_msec, uint64_t start_timestamp);

/*
 * Read cancellation, driven by c_serial_read_cancel():
 *   while (is_read_active) { signal(); wait_for_reader(slice); }  reset();
 */

/* Ask the running read to return CSERIAL_ERROR_CANCELLED as soon as possible
 * (sets os.cancel_read / os.cancel_read_event).  Callable from any thread. */
void cserial_os_cancel_read_signal(c_serial_port_type* port);
/* Clear the cancel request.  Called after a cancel has completed, and by
 * every new read before it starts, so that a cancel only affects the read
 * that was running when it was issued. */
void cserial_os_cancel_read_reset(c_serial_port_type* port);
/* Wait up to timeout_msec for the reader to release os.mutex, i.e. to leave
 * the blocking part of the read, and release it again at once.  Returns
 * early if the mutex is free; the caller re-checks is_read_active. */
void cserial_os_wait_for_reader(c_serial_port_type* port, int64_t timeout_msec);

/* Set DTR and RTS from lines->dtr/lines->rts; the other fields are ignored */
int cserial_os_set_control_lines(c_serial_port_type* port, const c_serial_control_lines_type* lines);
/* Fill all line fields from the driver.  Fails on ports without modem lines
 * (ptys, many USB adapters).  On Windows dtr/rts come from os.dtr/os.rts. */
int cserial_os_get_control_lines(c_serial_port_type* port, c_serial_control_lines_type* lines);
/* Number of received bytes that a read would return immediately */
int cserial_os_get_available(c_serial_port_type* port, int* available);
/* Block until all written data has been sent; input is not discarded */
int cserial_os_flush(c_serial_port_type* port);
/* Handle for the application's own poll()/WaitForMultipleObjects() loop
 * (c_serial_get_poll_handle).  POSIX: the port descriptor itself.  Windows:
 * the auto-reset event of read_overlap, signalled when the overlapped
 * read or WaitCommEvent of c_serial_read_data completes. */
c_serial_handle_t cserial_os_poll_handle(c_serial_port_type* port);

/* Store up to max malloc'ed names of the serial ports present in names[] and
 * return their number, or < 0 on error (c_serial.c then frees names[]).
 * Names are what c_serial_set_port_name() accepts: "COM3" on Windows,
 * "/dev/ttyUSB0" on POSIX.  Ports are not opened on Windows; on POSIX only
 * likely tty devices are briefly opened to check them. */
int cserial_os_list_ports(char** names, int max);

#endif /* C_SERIAL_INTERNAL_H */
