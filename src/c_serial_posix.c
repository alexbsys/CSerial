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
 * POSIX backend: termios, ioctl and poll-style I/O shared by all
 * POSIX systems.  OS-specific pieces are in c_serial_posix.h.
 *
 * Settings are changed by reading the port's struct termios, modifying the
 * relevant flags and writing it back:
 *   c_iflag  input processing      (IXON/IXOFF software flow, IGNPAR, ICRNL)
 *   c_oflag  output processing     (OPOST)
 *   c_cflag  hardware control      (CSIZE data bits, CSTOPB, PARENB/PARODD,
 *                                   CRTSCTS hardware flow, CLOCAL, CREAD)
 *   c_lflag  terminal behaviour    (ICANON, ECHO, ISIG)
 *   c_cc[]   control characters and the VMIN/VTIME read timing
 * Modem lines (DTR, RTS, CTS, ...) are read and set with the TIOCMGET and
 * TIOCMSET ioctls as a bit mask of TIOCM_* flags.
 */

#include "c_serial_posix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* open() flag that closes the descriptor in child processes after exec().
 * Very old systems lack it; open() then sets FD_CLOEXEC with fcntl(). */
#ifdef O_CLOEXEC
#define CSERIAL_O_CLOEXEC O_CLOEXEC
#else
#define CSERIAL_O_CLOEXEC 0
#endif

/* c_cflag bit for RTS/CTS hardware flow control: CRTSCTS on Linux, macOS and
 * the BSDs, CNEW_RTSCTS on older Solaris */
#ifdef CRTSCTS
#define HW_FLOW CRTSCTS
#elif defined(CNEW_RTSCTS)
#define HW_FLOW CNEW_RTSCTS
#endif

/* A blocking read waits in slices of this many milliseconds and releases the
 * port mutex in between, so that cancel and close are noticed this fast */
#define CSERIAL_READ_POLL_SLICE_MS 30

const char cserial_os_port_name_prefix[] = "";
/* Keeps "/dev/..." paths within the usual NAME_MAX of 255 */
const size_t cserial_os_port_name_max_length = 255;

/*
 * Baud rates that struct termios can express directly.  termios stores a
 * speed as a speed_t code (B9600 ...): on Linux an arbitrary small number,
 * on macOS and the BSDs the rate itself.  Rates not in this table go through
 * cserial_posix_set_custom_baud_rate().  The #ifdefs skip codes that the
 * system does not define.
 */
struct c_serial_speed_entry {
	int baud;       /* bits per second */
	speed_t code;   /* Bxxx constant for cfsetospeed()/cfsetispeed() */
};

static const struct c_serial_speed_entry c_serial_speeds[] = {
	{ 0, B0 }, { 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 }, { 150, B150 },
	{ 200, B200 }, { 300, B300 }, { 600, B600 }, { 1200, B1200 }, { 1800, B1800 },
	{ 2400, B2400 }, { 4800, B4800 },
#ifdef B7200
	{ 7200, B7200 },
#endif
	{ 9600, B9600 },
#ifdef B14400
	{ 14400, B14400 },
#endif
	{ 19200, B19200 },
#ifdef B28800
	{ 28800, B28800 },
#endif
	{ 38400, B38400 },
#ifdef B57600
	{ 57600, B57600 },
#endif
#ifdef B76800
	{ 76800, B76800 },
#endif
#ifdef B115200
	{ 115200, B115200 },
#endif
#ifdef B230400
	{ 230400, B230400 },
#endif
#ifdef B460800
	{ 460800, B460800 },
#endif
#ifdef B500000
	{ 500000, B500000 },
#endif
#ifdef B576000
	{ 576000, B576000 },
#endif
#ifdef B921600
	{ 921600, B921600 },
#endif
#ifdef B1000000
	{ 1000000, B1000000 },
#endif
#ifdef B1152000
	{ 1152000, B1152000 },
#endif
#ifdef B1500000
	{ 1500000, B1500000 },
#endif
#ifdef B2000000
	{ 2000000, B2000000 },
#endif
#ifdef B2500000
	{ 2500000, B2500000 },
#endif
#ifdef B3000000
	{ 3000000, B3000000 },
#endif
#ifdef B3500000
	{ 3500000, B3500000 },
#endif
#ifdef B4000000
	{ 4000000, B4000000 },
#endif
};

/* Bits per second -> Bxxx code; returns 0 if there is no standard code */
static int baud_to_speed(int baud, speed_t* code) {
	size_t x;
	for (x = 0; x < sizeof(c_serial_speeds) / sizeof(c_serial_speeds[0]); x++) {
		if (c_serial_speeds[x].baud == baud) {
			*code = c_serial_speeds[x].code;
			return 1;
		}
	}
	return 0;
}

/* Bxxx code -> bits per second; returns 0 for an unknown code */
static int speed_to_baud(speed_t code, int* baud) {
	size_t x;
	for (x = 0; x < sizeof(c_serial_speeds) / sizeof(c_serial_speeds[0]); x++) {
		if (c_serial_speeds[x].code == code) {
			*baud = c_serial_speeds[x].baud;
			return 1;
		}
	}
	return 0;
}

/* Lock os.mutex, waiting at most timeout_msec (<0: forever, 0: try once).
 * Returns 0 when locked, ETIMEDOUT or another error number otherwise.
 * pthread_mutex_timedlock() wants an absolute CLOCK_REALTIME deadline,
 * so "now + timeout" is computed here. */
static int lock_with_timeout(c_serial_port_type* port, int64_t timeout_msec) {
	struct timespec abs_timeout;

	if (timeout_msec < 0)
		return pthread_mutex_lock(&port->os.mutex);

	clock_gettime(CLOCK_REALTIME, &abs_timeout);
	abs_timeout.tv_sec += (time_t)(timeout_msec / 1000);
	abs_timeout.tv_nsec += (long)(timeout_msec % 1000) * 1000000L;
	if (abs_timeout.tv_nsec >= 1000000000L) {
		abs_timeout.tv_sec += 1;
		abs_timeout.tv_nsec -= 1000000000L;
	}

	return cserial_posix_mutex_timedlock(&port->os.mutex, &abs_timeout);
}

int cserial_posix_get_termios(c_serial_port_type* port, struct termios* io) {
	memset(io, 0, sizeof(*io));
	if (tcgetattr(port->port, io) < 0) {
		port->last_errnum = errno;
		return -1;
	}
	return 0;
}

int cserial_posix_set_termios(c_serial_port_type* port, struct termios* io) {
	if (cserial_posix_tcsetattr(port, io) < 0) {
		port->last_errnum = errno;
		return -1;
	}

	if (port->os.custom_baud_active &&
		cserial_posix_set_custom_baud_rate(port, port->baud_rate) != CSERIAL_OK) {
		return -1;
	}

	return 0;
}

uint64_t cserial_os_tick_count(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int cserial_os_is_valid_baud_rate(int baud) {
	/* 0 is B0: hang up */
	return baud >= 0;
}

int cserial_os_init(c_serial_port_type* port, c_serial_errnum_t* errnum) {
	int rc;

	port->port = -1;
	rc = pthread_mutex_init(&port->os.mutex, NULL);
	if (rc != 0) {
		if (errnum != NULL)
			*errnum = rc;
		return CSERIAL_ERROR_CANT_CREATE;
	}
	return CSERIAL_OK;
}

void cserial_os_destroy(c_serial_port_type* port) {
	pthread_mutex_destroy(&port->os.mutex);
}

int cserial_os_open(c_serial_port_type* port) {
	int fd;
	int flags;
	struct termios io;

	/* O_NOCTTY: never become the controlling terminal of this process.
	 * O_NONBLOCK: on Darwin, opening /dev/tty.* otherwise blocks until DCD is
	 * asserted; it is cleared again below, reads and writes block normally. */
	fd = open(port->port_name, O_RDWR | O_NOCTTY | O_NONBLOCK | CSERIAL_O_CLOEXEC);
	if (fd < 0) {
		port->last_errnum = errno;
		if (port->last_errnum == ENOENT)
			return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
		return CSERIAL_ERROR_GENERIC;
	}

	if (CSERIAL_O_CLOEXEC == 0)
		fcntl(fd, F_SETFD, FD_CLOEXEC);

	/* Only terminals have termios settings: rejects regular files, /dev/null, ... */
	if (tcgetattr(fd, &io) < 0) {
		port->last_errnum = errno;
		close(fd);
		if (port->last_errnum == ENOTTY || port->last_errnum == EINVAL)
			return CSERIAL_ERROR_NOT_A_SERIAL_PORT;
		return CSERIAL_ERROR_GENERIC;
	}

	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
		port->last_errnum = errno;
		close(fd);
		return CSERIAL_ERROR_GENERIC;
	}

	port->port = fd;
	port->os.custom_baud_active = 0;
	return CSERIAL_OK;
}

void cserial_os_close(c_serial_port_type* port) {
	/* Close under the mutex so that no reader can still be using the descriptor
	 * (a closed descriptor number may be reused by an unrelated file). */
	pthread_mutex_lock(&port->os.mutex);
	port->is_open = 0;
	if (port->port >= 0)
		close(port->port);
	port->port = -1;
	port->os.custom_baud_active = 0;
	pthread_mutex_unlock(&port->os.mutex);
}

int cserial_os_set_raw_input(c_serial_port_type* port) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	/* Full raw mode: disable all input processing, output processing,
	 * canonical mode, echo, and signals.  Equivalent to cfmakeraw() but
	 * explicit so we also clear IXON/IXOFF/ISTRIP which cfmakeraw misses. */
	/* Input: no CR/NL translation, no 8th bit stripping, no XON/XOFF,
	 * BREAK conditions are ignored instead of raising signals */
	io.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
					INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
	io.c_iflag |= IGNBRK;
	/* Output: bytes are sent unchanged (no NL -> CR NL) */
	io.c_oflag &= ~OPOST;
	/* No echo, no line buffering (ICANON), no Ctrl+C/Ctrl+Z signals */
	io.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	/* CLOCAL: ignore DCD, so the port works without a carrier;
	 * CREAD: enable the receiver */
	io.c_cflag |= (CLOCAL | CREAD);
	/* read() returns as soon as at least 1 byte is there, no inter-byte timer */
	io.c_cc[VTIME] = 0;
	io.c_cc[VMIN] = 1;

	if (cserial_posix_set_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_set_baud_rate(c_serial_port_type* port, int baud) {
	struct termios io;
	speed_t code;

	if (!baud_to_speed(baud, &code))
		return cserial_posix_set_custom_baud_rate(port, baud);

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (cfsetospeed(&io, code) < 0 || cfsetispeed(&io, code) < 0) {
		port->last_errnum = errno;
		return CSERIAL_ERROR_GENERIC;
	}

	port->os.custom_baud_active = 0;
	if (cserial_posix_set_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_baud_rate(c_serial_port_type* port) {
	struct termios io;
	speed_t code;
	int baud;

	if (cserial_posix_get_custom_baud_rate(port, &baud))
		return baud;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	/* An unknown code is a plain rate on systems where speed_t is the rate
	 * itself (macOS, BSD), e.g. one set by another program */
	code = cfgetospeed(&io);
	if (!speed_to_baud(code, &baud))
		baud = (int)code;

	return baud;
}

int cserial_os_set_data_bits(c_serial_port_type* port, enum CSerial_Data_Bits bits) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	io.c_cflag &= ~CSIZE;
	switch (bits) {
	case CSERIAL_BITS_5: io.c_cflag |= CS5; break;
	case CSERIAL_BITS_6: io.c_cflag |= CS6; break;
	case CSERIAL_BITS_7: io.c_cflag |= CS7; break;
	default: io.c_cflag |= CS8; break;
	}

	if (cserial_posix_set_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_data_bits(c_serial_port_type* port) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	switch (io.c_cflag & CSIZE) {
	case CS5: return CSERIAL_BITS_5;
	case CS6: return CSERIAL_BITS_6;
	case CS7: return CSERIAL_BITS_7;
	case CS8: return CSERIAL_BITS_8;
	}

	return CSERIAL_ERROR_GENERIC;
}

int cserial_os_set_stop_bits(c_serial_port_type* port, enum CSerial_Stop_Bits bits) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (bits == CSERIAL_STOP_BITS_2)
		io.c_cflag |= CSTOPB;
	else
		io.c_cflag &= ~CSTOPB;

	if (cserial_posix_set_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_stop_bits(c_serial_port_type* port) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	return (io.c_cflag & CSTOPB) ? CSERIAL_STOP_BITS_2 : CSERIAL_STOP_BITS_1;
}

int cserial_os_set_parity(c_serial_port_type* port, enum CSerial_Parity parity) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	/* PARENB adds a parity bit to every character, PARODD makes it odd
	 * instead of even.  Received parity is only verified with INPCK, which
	 * is not set, so bad bytes are passed on.  IGNPAR drops received bytes
	 * with framing or parity errors; it is set only without parity. */
	io.c_iflag &= ~IGNPAR;
	io.c_cflag &= ~(PARODD | PARENB);
	if (parity == CSERIAL_PARITY_NONE)
		io.c_iflag |= IGNPAR;
	else if (parity == CSERIAL_PARITY_ODD)
		io.c_cflag |= (PARENB | PARODD);
	else if (parity == CSERIAL_PARITY_EVEN)
		io.c_cflag |= PARENB;

	if (cserial_posix_set_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_parity(c_serial_port_type* port) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (!(io.c_cflag & PARENB))
		return CSERIAL_PARITY_NONE;
	return (io.c_cflag & PARODD) ? CSERIAL_PARITY_ODD : CSERIAL_PARITY_EVEN;
}

int cserial_os_set_flow_control(c_serial_port_type* port, enum CSerial_Flow_Control flow) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	/* Hardware: the driver stops sending while CTS is low and drops RTS when
	 * its input buffer fills up.  Software: XOFF (Ctrl+S) / XON (Ctrl+Q) bytes
	 * pause and resume the other side (IXON for our output, IXOFF for our
	 * input, IXANY: any byte resumes output). */
	io.c_iflag &= ~(IXON | IXOFF | IXANY);
	io.c_cflag &= ~HW_FLOW;
	if (flow == CSERIAL_FLOW_HARDWARE)
		io.c_cflag |= HW_FLOW;
	else if (flow == CSERIAL_FLOW_SOFTWARE)
		io.c_iflag |= (IXON | IXOFF | IXANY);

	if (cserial_posix_set_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_flow_control(c_serial_port_type* port) {
	struct termios io;

	if (cserial_posix_get_termios(port, &io) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (io.c_cflag & HW_FLOW)
		return CSERIAL_FLOW_HARDWARE;
	if (io.c_iflag & (IXON | IXOFF))
		return CSERIAL_FLOW_SOFTWARE;

	return CSERIAL_FLOW_NONE;
}

int cserial_os_write(c_serial_port_type* port, const void* data, int* length, int timeout_msec) {
	ssize_t bytes_written;

	/* With a timeout, first wait until the driver can take data (its output
	 * buffer may be full, e.g. while flow control holds us back).  write()
	 * then takes what fits without blocking for long; the caller handles a
	 * short count.  Without a timeout write() simply blocks. */
	if (timeout_msec >= 0) {
		short revents = 0;
		int rc;
		do {
			rc = cserial_posix_wait_fd(port->port, POLLOUT, timeout_msec, &revents);
		} while (rc < 0 && errno == EINTR);
		if (rc < 0) {
			port->last_errnum = errno;
			*length = 0;
			return CSERIAL_ERROR_GENERIC;
		}
		if (rc == 0) {
			*length = 0;
			return CSERIAL_ERROR_TIMEOUT;
		}
	}

	do {
		bytes_written = write(port->port, data, (size_t)*length);
	} while (bytes_written < 0 && errno == EINTR);

	if (bytes_written < 0) {
		port->last_errnum = errno;
		CSERIALDBG("Unable to write data to serial port");
		*length = 0;
		return CSERIAL_ERROR_GENERIC;
	}

	*length = (int)bytes_written;
	return CSERIAL_OK;
}

/* Whether a line selected in flags (CSERIAL_LINE_FLAG_*) differs between two
 * TIOCMGET states (TIOCM_* bits) */
static int line_change_matters(int flags, int old_state, int new_state) {
	int changed = old_state ^ new_state;

	return ((flags & CSERIAL_LINE_FLAG_CTS) && (changed & TIOCM_CTS)) ||
		((flags & CSERIAL_LINE_FLAG_CD) && (changed & TIOCM_CD)) ||
		((flags & CSERIAL_LINE_FLAG_DSR) && (changed & TIOCM_DSR)) ||
		((flags & CSERIAL_LINE_FLAG_DTR) && (changed & TIOCM_DTR)) ||
		((flags & CSERIAL_LINE_FLAG_RTS) && (changed & TIOCM_RTS)) ||
		((flags & CSERIAL_LINE_FLAG_RI) && (changed & TIOCM_RI));
}

/*
 * Each pass of the loop: lock the mutex, give up if the port was closed or the
 * read cancelled, wait up to one slice for data, check the modem lines for a
 * change the caller asked for, unlock.  The loop ends with the mutex held;
 * read() is then done without blocking because data is known to be there.
 *
 * Modem lines cannot be waited for with poll(), so they are polled once per
 * slice with TIOCMGET and compared with the state at the start of the read.
 */
int cserial_os_read(c_serial_port_type* port, void* data, int* data_length,
	c_serial_control_lines_type* lines, int timeout_msec, uint64_t start_timestamp) {

	int ret_code = CSERIAL_OK;
	int data_ready = 0;             /* poll() reported data (or EOF) */
	int locked = 0;                 /* os.mutex is held */
	int can_read_control_state = 1; /* TIOCMGET works on this port */
	int have_original_state = 0;
	int original_control_state = 0; /* TIOCM_* bits to compare against */

	/* The mutex is released between poll slices so that cancel/close can get in */
	while (1) {
		int poll_timeout = CSERIAL_READ_POLL_SLICE_MS;
		int poll_status;
		short revents = 0;
		int64_t remaining;

		if (lock_with_timeout(port, cserial_internal_remaining_ms(timeout_msec, start_timestamp)) != 0) {
			ret_code = CSERIAL_ERROR_TIMEOUT;
			break;
		}
		locked = 1;

		if (!port->is_open || port->port < 0) {
			ret_code = CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
			break;
		}

		if (cserial_atomic_read(&port->os.cancel_read)) {
			ret_code = CSERIAL_ERROR_CANCELLED;
			break;
		}

		if (!have_original_state && can_read_control_state) {
			/* Some USB emulated serials (and ptys) may not support lines at all */
			if (ioctl(port->port, TIOCMGET, &original_control_state) < 0)
				can_read_control_state = 0;
			else
				have_original_state = 1;
		}

		/* Waiting only for line changes that can never be seen: return now */
		if (data == NULL && (port->line_flags == 0 || !can_read_control_state))
			break;

		remaining = cserial_internal_remaining_ms(timeout_msec, start_timestamp);
		if (remaining >= 0 && remaining < poll_timeout)
			poll_timeout = (int)remaining;

		poll_status = cserial_posix_wait_fd(port->port, data != NULL ? POLLIN : 0, poll_timeout, &revents);
		if (poll_status < 0) {
			if (errno != EINTR) {
				port->last_errnum = errno;
				CSERIALDBG("Bad value from poll");
				ret_code = CSERIAL_ERROR_GENERIC;
				break;
			}
			revents = 0;
		}

		/* POLLNVAL: the descriptor is not open (any more) */
		if (revents & POLLNVAL) {
			ret_code = CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
			break;
		}

		/* POLLHUP/POLLERR (device unplugged, pty closed): read() below
		 * reports it as end of file (0 bytes) or as an error */
		if (data != NULL && (revents & (POLLIN | POLLHUP | POLLERR))) {
			data_ready = 1;
			break;
		}

		if (data == NULL && (revents & (POLLHUP | POLLERR))) {
			ret_code = CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
			break;
		}

		if (can_read_control_state && port->line_flags != 0 && lines != NULL) {
			int new_control_state = 0;
			if (ioctl(port->port, TIOCMGET, &new_control_state) < 0) {
				port->last_errnum = errno;
				CSERIALDBG("IOCTL call failed");
				ret_code = CSERIAL_ERROR_GENERIC;
				break;
			}

			if (line_change_matters(port->line_flags, original_control_state, new_control_state))
				break;

			original_control_state = new_control_state;
		}

		if (cserial_internal_remaining_ms(timeout_msec, start_timestamp) == 0) {
			ret_code = CSERIAL_ERROR_TIMEOUT;
			break;
		}

		pthread_mutex_unlock(&port->os.mutex);
		locked = 0;
	}

	if (ret_code == CSERIAL_OK && data_ready) {
		ssize_t bytes_read;

		do {
			bytes_read = read(port->port, data, (size_t)*data_length);
		} while (bytes_read < 0 && errno == EINTR);

		if (bytes_read < 0) {
			port->last_errnum = errno;
			CSERIALDBG("Unable to read data");
			ret_code = CSERIAL_ERROR_GENERIC;
			*data_length = 0;
		} else {
			*data_length = (int)bytes_read;
		}
	} else if (data_length != NULL) {
		*data_length = 0;
	}

	if (ret_code == CSERIAL_OK && lines != NULL) {
		memset(lines, 0, sizeof(*lines));
		if (!can_read_control_state || cserial_os_get_control_lines(port, lines) != CSERIAL_OK) {
			memset(lines, 0, sizeof(*lines));
			lines->unsupported = 1;
		}
	}

	if (locked)
		pthread_mutex_unlock(&port->os.mutex);

	return ret_code;
}

void cserial_os_cancel_read_signal(c_serial_port_type* port) {
	cserial_atomic_exchange(&port->os.cancel_read, 1);
}

void cserial_os_cancel_read_reset(c_serial_port_type* port) {
	cserial_atomic_exchange(&port->os.cancel_read, 0);
}

void cserial_os_wait_for_reader(c_serial_port_type* port, int64_t timeout_msec) {
	if (lock_with_timeout(port, timeout_msec) == 0)
		pthread_mutex_unlock(&port->os.mutex);
}

int cserial_os_set_control_lines(c_serial_port_type* port, const c_serial_control_lines_type* lines) {
	int state = 0;

	if (ioctl(port->port, TIOCMGET, &state) < 0) {
		port->last_errnum = errno;
		CSERIALDBG("IOCTL failed when attempting to read control lines");
		return CSERIAL_ERROR_GENERIC;
	}

	if (lines->dtr)
		state |= TIOCM_DTR;
	else
		state &= ~TIOCM_DTR;

	if (lines->rts)
		state |= TIOCM_RTS;
	else
		state &= ~TIOCM_RTS;

	if (ioctl(port->port, TIOCMSET, &state) < 0) {
		port->last_errnum = errno;
		CSERIALDBG("IOCTL failed when attempting to set control lines");
		return CSERIAL_ERROR_GENERIC;
	}

	return CSERIAL_OK;
}

int cserial_os_get_control_lines(c_serial_port_type* port, c_serial_control_lines_type* lines) {
	int state;

	if (ioctl(port->port, TIOCMGET, &state) < 0) {
		port->last_errnum = errno;
		CSERIALDBG("IOCTL failed when attempting to read control lines");
		return CSERIAL_ERROR_GENERIC;
	}

	lines->cd = (state & TIOCM_CD) ? 1 : 0;
	lines->cts = (state & TIOCM_CTS) ? 1 : 0;
	lines->dsr = (state & TIOCM_DSR) ? 1 : 0;
	lines->dtr = (state & TIOCM_DTR) ? 1 : 0;
	lines->rts = (state & TIOCM_RTS) ? 1 : 0;
	lines->ri = (state & TIOCM_RI) ? 1 : 0;

	return CSERIAL_OK;
}

int cserial_os_get_available(c_serial_port_type* port, int* available) {
	if (ioctl(port->port, FIONREAD, available) < 0) {
		port->last_errnum = errno;
		CSERIALDBG("IOCTL failed when attempting to read bytes available");
		return CSERIAL_ERROR_GENERIC;
	}
	return CSERIAL_OK;
}

int cserial_os_flush(c_serial_port_type* port) {
	/* Wait for transmission like FlushFileBuffers(); tcflush() would discard
	 * unsent bytes, losing data before the software RS485 RTS drop. */
	if (tcdrain(port->port) < 0) {
		port->last_errnum = errno;
		return CSERIAL_ERROR_GENERIC;
	}
	return CSERIAL_OK;
}

c_serial_handle_t cserial_os_poll_handle(c_serial_port_type* port) {
	return port->port;
}

/* Only probe names that look like serial devices: opening arbitrary /dev
 * nodes has side effects (e.g. /dev/watchdog arms a hardware reset).
 * tty* covers ttyS (UARTs), ttyUSB, ttyACM, ttyAMA, ...; cu.* are the macOS
 * call-out devices; rfcomm* are Bluetooth serial links. */
static int is_candidate_device_name(const char* name) {
	static const char* const prefixes[] = { "tty", "cu.", "rfcomm" };
	size_t x;

	if (strcmp(name, "tty") == 0)
		return 0;

	/* tty0, tty1, ...: Linux virtual consoles, not serial ports */
	if (strncmp(name, "tty", 3) == 0 && strspn(name + 3, "0123456789") == strlen(name + 3))
		return 0;

	for (x = 0; x < sizeof(prefixes) / sizeof(prefixes[0]); x++) {
		if (strncmp(name, prefixes[x], strlen(prefixes[x])) == 0)
			return 1;
	}
	return 0;
}

/* A /dev entry is reported if its name passes is_candidate_device_name(), it
 * is a character device and isatty() confirms it is a terminal.  Opening
 * with O_NONBLOCK does not wait for DCD and does not touch the settings. */
int cserial_os_list_ports(char** names, int max) {
	struct dirent* entry;
	DIR* dir;
	char device_name[128];
	int count = 0;

	dir = opendir("/dev");
	if (dir == NULL) {
		/* e.g. sandboxed iOS/Android apps: report no ports */
		CSERIALDBG("Unable to open /dev");
		return 0;
	}

	while (count < max && (entry = readdir(dir)) != NULL) {
		struct stat st;
		int fd;
		int is_tty;
		int len;

		if (!is_candidate_device_name(entry->d_name))
			continue;

		len = snprintf(device_name, sizeof(device_name), "/dev/%s", entry->d_name);
		if (len < 0 || (size_t)len >= sizeof(device_name)) {
			CSERIALDBG("Ignoring device in /dev/: name too long");
			continue;
		}

		if (stat(device_name, &st) != 0 || !S_ISCHR(st.st_mode))
			continue;

		fd = open(device_name, O_RDONLY | O_NONBLOCK | O_NOCTTY | CSERIAL_O_CLOEXEC);
		if (fd < 0)
			continue;

		is_tty = isatty(fd);
		close(fd);
		if (!is_tty)
			continue;

		names[count] = cserial_internal_strdup(device_name);
		if (names[count] == NULL) {
			closedir(dir);
			return -1;
		}
		count++;
	}

	closedir(dir);
	return count;
}
