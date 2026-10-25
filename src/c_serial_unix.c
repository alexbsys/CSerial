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
 * Other POSIX systems (BSD, Solaris, Cygwin) part of the POSIX backend (see
 * c_serial_posix.h): plain POSIX calls only.  Arbitrary baud rates work
 * where speed_t is the rate itself (the BSDs); there is no RS485 mode.
 */

#include "c_serial_posix.h"

#include <errno.h>
#include <poll.h>

int cserial_posix_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abs_timeout) {
	return pthread_mutex_timedlock(mutex, abs_timeout);
}

int cserial_posix_wait_fd(int fd, short events, int timeout_msec, short* revents) {
	struct pollfd pfd;
	int rc;

	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	rc = poll(&pfd, 1, timeout_msec);
	*revents = pfd.revents;
	return rc;
}

int cserial_posix_tcsetattr(c_serial_port_type* port, struct termios* io) {
	return tcsetattr(port->port, TCSANOW, io);
}

int cserial_posix_set_custom_baud_rate(c_serial_port_type* port, int baud) {
	/* On the BSDs speed_t is the plain baud value */
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (cfsetospeed(&io, (speed_t)baud) < 0 || cfsetispeed(&io, (speed_t)baud) < 0 ||
		cserial_posix_set_termios(port, &io) < 0) {
		port->last_errnum = errno;
		return CSERIAL_ERROR_GENERIC;
	}

	return CSERIAL_OK;
}

/* The rate is in struct termios, cserial_os_get_baud_rate() decodes it */
int cserial_posix_get_custom_baud_rate(c_serial_port_type* port, int* baud) {
	(void)port;
	(void)baud;
	return 0;
}

/* No driver RS485 mode: CSERIAL_RTS_BEST_AVAILABLE falls back to software
 * RTS, CSERIAL_RTS_HARDWARE fails */
int cserial_os_set_rts_hardware(c_serial_port_type* port) {
	(void)port;
	return CSERIAL_RTS_TYPE_NOT_AVAILABLE;
}

int cserial_os_clear_rts_hardware(c_serial_port_type* port) {
	(void)port;
	return CSERIAL_OK;
}
