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
 * macOS and iOS part of the POSIX backend (see c_serial_posix.h):
 *  - select() instead of poll(), which does not work on Darwin devices;
 *  - IOSSIOSPEED for arbitrary baud rates (macOS only, iOS has no IOKit);
 *  - a pthread_mutex_timedlock() replacement, Darwin does not have one;
 *  - a tcsetattr() wrapper that copes with IOSSIOSPEED rates.
 * Serial devices appear twice on macOS: /dev/tty.* (dial-in, opening waits
 * for DCD unless O_NONBLOCK) and /dev/cu.* (call-out, for applications).
 */

/* Must precede every system header.  Without it select() fails for
 * descriptors >= FD_SETSIZE (1024); with it fd_set may be allocated as large
 * as the descriptor requires. */
#define _DARWIN_UNLIMITED_SELECT 1

#include "c_serial_posix.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>

/* IOSSIOSPEED: ioctl that sets any baud rate, bypassing struct termios */
#ifdef CSERIAL_PLATFORM_MACOSX
#include <IOKit/serial/ioss.h>
#endif

/* Darwin lacks pthread_mutex_timedlock(): try to lock every millisecond until
 * abs_timeout (CLOCK_REALTIME) has passed.  Unlike the real one, won't return
 * EOWNERDEAD or EDEADLK. */
int cserial_posix_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abs_timeout) {
	int rv;

	while ((rv = pthread_mutex_trylock(mutex)) == EBUSY) {
		struct timespec now;
		struct timespec pause_time;

		clock_gettime(CLOCK_REALTIME, &now);
		if (now.tv_sec > abs_timeout->tv_sec ||
			(now.tv_sec == abs_timeout->tv_sec && now.tv_nsec >= abs_timeout->tv_nsec)) {
			return ETIMEDOUT;
		}

		pause_time.tv_sec = 0;
		pause_time.tv_nsec = 1000000;
		nanosleep(&pause_time, NULL);
	}

	return rv;
}

/* Darwin's poll() does not support character devices, so use select()
 * with heap-allocated sets sized for the descriptor.  The result is
 * translated to poll() terms: POLLIN/POLLOUT, and POLLNVAL for a closed
 * descriptor (select() fails with EBADF). */
int cserial_posix_wait_fd(int fd, short events, int timeout_msec, short* revents) {
	/* An fd_set is a bit array of fd_mask words (NFDBITS bits each); it needs
	 * enough words to hold bit number fd */
	size_t set_size = howmany(fd + 1, NFDBITS) * sizeof(fd_mask);
	fd_set* read_set = NULL;
	fd_set* write_set = NULL;
	struct timeval tv;
	int rc;

	*revents = 0;
	if (events & POLLIN) {
		read_set = (fd_set*)calloc(1, set_size);
		if (read_set == NULL) {
			errno = ENOMEM;
			return -1;
		}
		FD_SET(fd, read_set);
	}
	if (events & POLLOUT) {
		write_set = (fd_set*)calloc(1, set_size);
		if (write_set == NULL) {
			free(read_set);
			errno = ENOMEM;
			return -1;
		}
		FD_SET(fd, write_set);
	}

	tv.tv_sec = timeout_msec / 1000;
	tv.tv_usec = (timeout_msec % 1000) * 1000;
	rc = select(fd + 1, read_set, write_set, NULL, timeout_msec < 0 ? NULL : &tv);
	if (rc < 0 && errno == EBADF) {
		*revents = POLLNVAL;
		rc = 1;
	} else if (rc > 0) {
		if (read_set && FD_ISSET(fd, read_set))
			*revents |= POLLIN;
		if (write_set && FD_ISSET(fd, write_set))
			*revents |= POLLOUT;
	}

	free(read_set);
	free(write_set);
	return rc;
}

int cserial_posix_tcsetattr(c_serial_port_type* port, struct termios* io) {
	if (tcsetattr(port->port, TCSANOW, io) == 0)
		return 0;

	/* After IOSSIOSPEED, tcgetattr() reports the custom rate in the speed
	 * fields and many drivers reject it when it is written back.  Write a
	 * placeholder speed instead; cserial_posix_set_termios() then restores
	 * the real rate with IOSSIOSPEED. */
	if (errno != EINVAL || !port->os.custom_baud_active)
		return -1;

	cfsetispeed(io, B9600);
	cfsetospeed(io, B9600);
	return tcsetattr(port->port, TCSANOW, io);
}

int cserial_posix_set_custom_baud_rate(c_serial_port_type* port, int baud) {
#ifdef CSERIAL_PLATFORM_MACOSX
	/* The rate is kept by the driver outside of termios and lost on every
	 * tcsetattr(), hence custom_baud_active */
	speed_t speed = (speed_t)baud;

	if (ioctl(port->port, IOSSIOSPEED, &speed) < 0) {
		port->last_errnum = errno;
		CSERIALDBG("IOSSIOSPEED ioctl failed\n");
		return CSERIAL_ERROR_GENERIC;
	}

	port->os.custom_baud_active = 1;
	return CSERIAL_OK;
#else
	/* iOS has no IOKit headers; speed_t is the plain baud value */
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (cfsetospeed(&io, (speed_t)baud) < 0 || cfsetispeed(&io, (speed_t)baud) < 0 ||
		cserial_posix_set_termios(port, &io) < 0) {
		port->last_errnum = errno;
		return CSERIAL_ERROR_GENERIC;
	}

	return CSERIAL_OK;
#endif
}

/* There is no ioctl to read an IOSSIOSPEED rate back: report the rate that
 * was set, or let the caller decode termios */
int cserial_posix_get_custom_baud_rate(c_serial_port_type* port, int* baud) {
	if (!port->os.custom_baud_active)
		return 0;

	*baud = port->baud_rate;
	return 1;
}

/* Darwin has no driver RS485 mode: CSERIAL_RTS_BEST_AVAILABLE falls back to
 * software RTS, CSERIAL_RTS_HARDWARE fails */
int cserial_os_set_rts_hardware(c_serial_port_type* port) {
	(void)port;
	return CSERIAL_RTS_TYPE_NOT_AVAILABLE;
}

int cserial_os_clear_rts_hardware(c_serial_port_type* port) {
	(void)port;
	return CSERIAL_OK;
}
