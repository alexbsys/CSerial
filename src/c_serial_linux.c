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
 * Linux and Android part of the POSIX backend (see c_serial_posix.h):
 *  - arbitrary baud rates (250000, 1000000, ...) through the termios2 ioctls,
 *    because struct termios can only express the fixed Bxxx speeds;
 *  - RS485 mode, where the driver raises RTS while transmitting
 *    (TIOCSRS485, available when CMake found <linux/serial.h>);
 *  - plain poll(), pthread_mutex_timedlock() and tcsetattr(), which work as
 *    specified on Linux.
 */

#include "c_serial_posix.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>

#ifdef HAVE_LINUX_SERIAL
#include <linux/serial.h>
#endif

/*
 * termios2 / BOTHER is only used on architectures with the asm-generic
 * termbits layout (NCCS=19, 'T' ioctls).  MIPS, PowerPC, SPARC and Alpha use
 * a different struct, and a mismatched struct would let the kernel write
 * past it; there only the standard Bxxx rates are available.
 */
#if defined(__i386__) || defined(__x86_64__) || defined(__arm__) || defined(__aarch64__) || \
    defined(__riscv) || defined(__loongarch__) || defined(__s390__)
/* Defined when this file can set and read arbitrary baud rates */
#define CSERIAL_HAVE_TERMIOS2

/*
 * The kernel's struct termios2 (from <asm/termbits.h>, which cannot be
 * included together with the libc <termios.h>).  It is struct termios plus
 * two integer speed fields.  Field meanings, as in termios(3):
 */
struct c_serial_termios2 {
	/* Input processing: IGNPAR, ICRNL (CR -> NL), IXON/IXOFF (XON/XOFF flow
	 * control), ISTRIP, ... */
	unsigned int c_iflag;
	/* Output processing: OPOST, ONLCR (NL -> CR NL), ... */
	unsigned int c_oflag;
	/* Hardware control: the baud rate bits (CBAUD), character size (CSIZE),
	 * CSTOPB (2 stop bits), PARENB/PARODD (parity), CRTSCTS (RTS/CTS flow
	 * control), CLOCAL (ignore modem lines), CREAD (enable receiver) */
	unsigned int c_cflag;
	/* Terminal behaviour: ICANON (line editing), ECHO, ISIG (Ctrl+C sends
	 * a signal), ... */
	unsigned int c_lflag;
	/* Line discipline number: 0 = N_TTY, the normal terminal discipline */
	unsigned char c_line;
	/* Control characters, indexed by VINTR, VEOF, VSTART/VSTOP (XON/XOFF
	 * bytes), ... and the read timing values VMIN (minimum bytes) and
	 * VTIME (timeout in 1/10 s).  19 entries (the kernel's NCCS). */
	unsigned char c_cc[19];
	/* Input and output baud rate in bits per second, used when the CBAUD
	 * bits of c_cflag are BOTHER */
	unsigned int c_ispeed;
	unsigned int c_ospeed;
};

/* Value of the CBAUD bits meaning "the speed is in c_ispeed/c_ospeed"
 * instead of one of the Bxxx codes.  Octal, as in <asm/termbits.h>. */
#ifndef BOTHER
#define BOTHER  0010000
#endif
/* Mask of the c_cflag bits that hold the baud rate code (B50 ... B4000000,
 * or BOTHER) */
#ifndef CBAUD
#define CBAUD   0010017
#endif

/* ioctl: read the port settings into a struct termios2 (TCGETS2) */
#define CSERIAL_TCGETS2  _IOR('T', 0x2A, struct c_serial_termios2)
/* ioctl: apply a struct termios2 once all pending output has been sent
 * (TCSETSW2; "W" = wait, like tcsetattr(TCSADRAIN)) */
#define CSERIAL_TCSETSW2 _IOW('T', 0x2C, struct c_serial_termios2)
#endif /* termios2 architectures */

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
#ifdef CSERIAL_HAVE_TERMIOS2
	/* Same mechanism pyserial uses for non-standard baud rates:
	 * read termios2, replace the Bxxx code by BOTHER plus the plain number */
	struct c_serial_termios2 tio;

	if (ioctl(port->port, CSERIAL_TCGETS2, &tio) < 0) {
		port->last_errnum = errno;
		CSERIALDBG("TCGETS2 ioctl failed\n");
		return CSERIAL_ERROR_GENERIC;
	}

	tio.c_cflag &= ~CBAUD;
	tio.c_cflag |= BOTHER;
	tio.c_ispeed = (unsigned int)baud;
	tio.c_ospeed = (unsigned int)baud;

	/* The driver picks the closest divisor it can; drivers that cannot do
	 * the rate at all fail here with EINVAL */
	if (ioctl(port->port, CSERIAL_TCSETSW2, &tio) < 0) {
		port->last_errnum = errno;
		CSERIALDBG("TCSETSW2 ioctl failed\n");
		return CSERIAL_ERROR_GENERIC;
	}

	/* The next tcsetattr() would put the old Bxxx code back */
	port->os.custom_baud_active = 1;
	return CSERIAL_OK;
#else
	port->last_errnum = EINVAL;
	return CSERIAL_ERROR_GENERIC;
#endif
}

int cserial_posix_get_custom_baud_rate(c_serial_port_type* port, int* baud) {
#ifdef CSERIAL_HAVE_TERMIOS2
	/* c_ospeed holds the actual rate for both BOTHER and Bxxx settings */
	struct c_serial_termios2 tio;

	if (ioctl(port->port, CSERIAL_TCGETS2, &tio) == 0) {
		*baud = (int)tio.c_ospeed;
		return 1;
	}
#endif
	return 0;
}

/*
 * RS485: with SER_RS485_ENABLED the driver raises RTS before sending and
 * drops it after the last bit, which switches the direction of a half-duplex
 * RS485 transceiver.  Only the enable flag is changed; RTS polarity and
 * delays stay as configured by the driver or the device tree.
 */

int cserial_os_set_rts_hardware(c_serial_port_type* port) {
#ifdef HAVE_LINUX_SERIAL
	struct serial_rs485 rs485conf;

	memset(&rs485conf, 0, sizeof(rs485conf));
	if (ioctl(port->port, TIOCGRS485, &rs485conf) < 0) {
		port->last_errnum = errno;
		/* ENOTTY/EINVAL: the driver has no RS485 support */
		if (errno == ENOTTY || errno == EINVAL)
			return CSERIAL_RTS_TYPE_NOT_AVAILABLE;
		return CSERIAL_ERROR_GENERIC;
	}

	rs485conf.flags |= SER_RS485_ENABLED;

	if (ioctl(port->port, TIOCSRS485, &rs485conf) < 0) {
		port->last_errnum = errno;
		return CSERIAL_RTS_TYPE_NOT_AVAILABLE;
	}
	return CSERIAL_OK;
#else
	/* Kernel headers without RS485 support */
	(void)port;
	return CSERIAL_RTS_TYPE_NOT_AVAILABLE;
#endif
}

int cserial_os_clear_rts_hardware(c_serial_port_type* port) {
#ifdef HAVE_LINUX_SERIAL
	struct serial_rs485 rs485conf;

	memset(&rs485conf, 0, sizeof(rs485conf));
	if (ioctl(port->port, TIOCGRS485, &rs485conf) < 0) {
		/* ENOTTY/EINVAL: the driver has no RS485 support (most USB
		 * adapters, ptys, e.g. an FTDI RS485 cable), so it is not on */
		if (errno == ENOTTY || errno == EINVAL)
			return CSERIAL_OK;
		port->last_errnum = errno;
		return CSERIAL_ERROR_GENERIC;
	}

	if (rs485conf.flags & SER_RS485_ENABLED) {
		rs485conf.flags &= ~(SER_RS485_ENABLED);

		if (ioctl(port->port, TIOCSRS485, &rs485conf) < 0) {
			port->last_errnum = errno;
			return CSERIAL_RTS_TYPE_NOT_AVAILABLE;
		}
	}
#endif
	return CSERIAL_OK;
}
