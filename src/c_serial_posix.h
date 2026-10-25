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
 * Interface between the common POSIX backend (c_serial_posix.c) and the
 * per-OS parts: c_serial_linux.c (Linux, Android), c_serial_darwin.c
 * (macOS, iOS) and c_serial_unix.c (BSD and other POSIX systems).
 *
 * c_serial_posix.c implements the cserial_os_* interface with standard POSIX
 * calls.  Where systems differ or a standard call is missing or broken, it
 * calls the hooks below, and every per-OS file provides all of them, plus
 * cserial_os_set_rts_hardware() / cserial_os_clear_rts_hardware().
 */
#ifndef C_SERIAL_POSIX_H
#define C_SERIAL_POSIX_H

#include "c_serial_internal.h"

#include <termios.h>
#include <time.h>

/*
 * Provided by c_serial_posix.c
 */

/* tcgetattr() of the open port; returns <0 and sets port->last_errnum on failure */
int cserial_posix_get_termios(c_serial_port_type* port, struct termios* io);
/* Apply io to the port, then re-apply a custom baud rate if one is active
 * (tcsetattr() would otherwise reset it); returns <0 with
 * port->last_errnum set on failure */
int cserial_posix_set_termios(c_serial_port_type* port, struct termios* io);

/*
 * Provided by the per-OS file
 */

/* pthread_mutex_timedlock(): lock, waiting until abs_timeout (CLOCK_REALTIME)
 * at the latest.  0 on success, ETIMEDOUT or another error number otherwise.
 * Darwin has no such function and gets a polling replacement. */
int cserial_posix_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abs_timeout);

/* Wait until fd is readable (events = POLLIN) and/or writable (POLLOUT), at
 * most timeout_msec milliseconds (<0: forever, 0: just check).  Returns <0
 * on error (errno set), 0 on timeout, >0 with *revents set to what happened
 * (POLLIN, POLLOUT, POLLHUP, POLLERR, POLLNVAL as with poll()).  Uses poll(),
 * or select() on Darwin, whose poll() does not support devices. */
int cserial_posix_wait_fd(int fd, short events, int timeout_msec, short* revents);

/* tcsetattr(TCSANOW) for the port, returns <0 with errno set on failure.
 * Darwin retries with a placeholder speed while a custom rate is active. */
int cserial_posix_tcsetattr(c_serial_port_type* port, struct termios* io);

/* Apply a baud rate that has no Bxxx constant (e.g. 250000).  Sets
 * port->os.custom_baud_active if the rate is kept outside of struct termios
 * and therefore has to be re-applied after every tcsetattr(). */
int cserial_posix_set_custom_baud_rate(c_serial_port_type* port, int baud);

/* If the OS can report the current rate outside of struct termios (termios2
 * on Linux, the remembered IOSSIOSPEED value on macOS), store it in *baud and
 * return 1; otherwise return 0 and the caller decodes the Bxxx code. */
int cserial_posix_get_custom_baud_rate(c_serial_port_type* port, int* baud);

#endif /* C_SERIAL_POSIX_H */
