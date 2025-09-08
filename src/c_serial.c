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

#include "config.h"
#include "c_serial_pdetect.h"
#include "c_serial_atomic.h"

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <cserial/c_serial.h>

#if defined(CSERIAL_DEBUG_PRINT) && CSERIAL_DEBUG_PRINT==1
#define CSERIALDBG(p) fprintf(stderr,(p))
#else /*defined(CSERIAL_DEBUG_PRINT) && CSERIAL_DEBUG_PRINT==1*/
#define CSERIALDBG(...)
#endif /*defined(CSERIAL_DEBUG_PRINT) && CSERIAL_DEBUG_PRINT==1*/

/*
 * Includes
 */
#ifdef CSERIAL_PLATFORM_WINDOWS
#include <windows.h>
#include <stdint.h>
#endif /*CSERIAL_PLATFORM_WINDOWS*/

#ifdef CSERIAL_PLATFORM_POSIX_BASED
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <pthread.h>

#ifdef CSERIAL_PLATFORM_ANDROID
#include <sched.h>
#endif /*CSERIAL_PLATFORM_ANDROID*/

#include <sys/ioctl.h>
#include <sys/times.h>
#include <errno.h>
#include <poll.h>

#ifdef HAVE_LINUX_SERIAL
#include <linux/serial.h>
#endif /*HAVE_LINUX_SERIAL*/

#endif /*CSERIAL_PLATFORM_POSIX_BASED*/


/*
 * Platform-specific CSERIAL definitions
 */
#ifdef CSERIAL_PLATFORM_WINDOWS

#define close( x ) CloseHandle( x )
#define SPEED_SWITCH(SPD,io) case SPD: io.BaudRate = CBR_##SPD; break;
#define GET_SPEED_SWITCH(SPD,io) case CBR_##SPD: baud_return = SPD; break;
#define DEFAULT_SPEED_SWITCH(SPD,io) default: io.BaudRate = (SPD); break;
#define DEFAULT_GET_SPEED_SWITCH(SPD,io)  default: baud_return = SPD; break;

typedef DCB serial_io_type;
typedef HANDLE c_serial_mutex_t;

#else /*CSERIAL_PLATFORM_WINDOWS*/

#ifndef ENOMEDIUM
#define ENOMEDIUM ENODEV
#endif

#ifdef CRTSCTS
#define HW_FLOW CRTSCTS
#elif CNEW_RTSCTS
#define HW_FLOW CNEW_RTSCTS
#endif

#define SPEED_SWITCH(SPD,io) case SPD: cfsetospeed( &io, B##SPD ); cfsetispeed( &io, B##SPD ); break;
#define GET_SPEED_SWITCH(SPD,io) case B##SPD: baud_return = (SPD); break;
#define DEFAULT_SPEED_SWITCH(SPD,io) default: cfsetospeed( &io, (SPD) ); cfsetispeed( &io, (SPD) ); break;
#define DEFAULT_GET_SPEED_SWITCH(SPD,io)  default: baud_return = (SPD); break;

typedef struct termios serial_io_type;
typedef pthread_mutex_t c_serial_mutex_t;

#endif /* CSERIAL_PLATFORM_WINDOWS */


#ifdef CSERIAL_PLATFORM_MAC
/*
 * A pthread_mutex_timedlock() impl for OSX/macOS, which lacks the
 * real thing.
 * NOTE: Unlike the real McCoy, won't return EOWNERDEAD, EDEADLK
 *       or EOWNERDEAD
 */
static int macos_pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct
timespec *abs_timeout)
{
    int rv;
    struct timespec remaining, slept, ts;

    remaining = *abs_timeout;
    while ((rv = pthread_mutex_trylock(mutex)) == EBUSY) {
        ts.tv_sec = 0;
        ts.tv_nsec = (remaining.tv_sec > 0 ? 10000000 :
                     (remaining.tv_nsec < 10000000 ? remaining.tv_nsec : 10000000));
        nanosleep(&ts, &slept);
        ts.tv_nsec -= slept.tv_nsec;
        if (ts.tv_nsec <= remaining.tv_nsec) {
            remaining.tv_nsec -= ts.tv_nsec;
        }
        else {
            remaining.tv_sec--;
            remaining.tv_nsec = (1000000 - (ts.tv_nsec - remaining.tv_nsec));
        }
        if (remaining.tv_sec < 0 || (!remaining.tv_sec && remaining.tv_nsec <= 0)) {
            return ETIMEDOUT;
        }
    }

    return rv;
}

#define pthread_mutex_timedlock macos_pthread_mutex_timedlock

#endif /*CSERIAL_PLATFORM_MAC*/


/*
 * Struct Definitions
 */

 /* \cond */
struct c_serial_port {
	c_serial_handle_t port;
	c_serial_mutex_t mutex;
	c_serial_errnum_t last_errnum;
	char* port_name;
	int baud_rate;
	enum CSerial_Data_Bits data_bits;
	enum CSerial_Stop_Bits stop_bits;
	enum CSerial_Parity parity;
	enum CSerial_Flow_Control flow;
	enum CSerial_RTS_Handling rs485;
	int rs485_is_software;
	void* user_data;
	int is_open;
	int line_flags;

	atomic_long_type is_read_active;

#ifdef CSERIAL_PLATFORM_POSIX_BASED
	atomic_long_type cancel_read_event;
#endif /*CSERIAL_PLATFORM_POSIX_BASED*/

#ifdef CSERIAL_PLATFORM_WINDOWS
	/* Windows-specific variables that we need to keep track of */
	int winDTR;
	int winRTS;
	OVERLAPPED read_overlap;
	OVERLAPPED write_overlap;
	HANDLE cancel_read_event;
#endif /*CSERIAL_PLATFORM_WINDOWS*/
};
/* \endcond */


/*
 * Platform specific functions
 */
#ifdef CSERIAL_PLATFORM_WINDOWS
static void c_serial_init_serial_io(serial_io_type* io) {
	memset(io, 0, sizeof(serial_io_type));
}

static int c_serial_set_serial_port_struct(c_serial_port_type* cserial_port, serial_io_type* io) {
	if (!SetCommState(cserial_port->port, io)) {
		cserial_port->last_errnum = GetLastError();
		CSERIALDBG("bad set comm\n");
		return -1;
	}

	return 0;
}

static int c_serial_get_serial_port_struct(c_serial_port_type* cserial_port, serial_io_type* io) {
	io->DCBlength = sizeof(serial_io_type);
	if (!GetCommState(cserial_port->port, io)) {
		cserial_port->last_errnum = GetLastError();
		CSERIALDBG("bad get comm line %d\n", __LINE__);
		return -1;
	}

	return 0;
}

static uint64_t c_serial_get_tick_count() {
	return GetTickCount64();
}

#else /*CSERIAL_PLATFORM_WINDOWS*/
static void c_serial_init_serial_io(serial_io_type* io) {
	memset(io, 0, sizeof(serial_io_type));
}

static int c_serial_set_serial_port_struct(c_serial_port_type* cserial_port, serial_io_type* io) {
	if (tcsetattr(cserial_port->port, TCSANOW, io) < 0) {
		return -1;
	}
	return 0;
}

static int c_serial_get_serial_port_struct(c_serial_port_type* cserial_port, serial_io_type* io) {
	if (tcgetattr(cserial_port->port, io) < 0) {
		return -1;
	}
	return 0;
}



static uint64_t c_serial_get_tick_count() {
	struct tms tm;
	clock_t time = times(&tm);
	long res = sysconf(_SC_CLK_TCK);
	uint64_t milliseconds64 = ((uint64_t)time) * 1000ULL;

	return milliseconds64 / res;
}

#endif /*CSERIAL_PLATFORM_WINDOWS*/




/*
 * Local Methods
 */

static void c_serial_set_rts_if_required(c_serial_port_type* port) {
	c_serial_control_lines_type lines;

	if (port->rs485_is_software) {
		c_serial_get_control_lines(port, &lines);
		lines.rts = 1;
		c_serial_set_control_line(port, &lines, 0);
	}
}

static void c_serial_clear_rts_if_required(c_serial_port_type* port) {
	c_serial_control_lines_type lines;

	if (port->rs485_is_software) {
		c_serial_get_control_lines(port, &lines);
		lines.rts = 0;
		c_serial_flush(port);
		c_serial_set_control_line(port, &lines, 0);
	}
}



static int clear_rts( c_serial_port_type* desc ){
#ifdef CSERIAL_PLATFORM_WINDOWS
	serial_io_type newio;
	
	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(desc, &newio) < 0)
	  return CSERIAL_ERROR_GENERIC;

	newio.fRtsControl = RTS_CONTROL_TOGGLE;

	if (c_serial_set_serial_port_struct(desc, &newio) < 0)
	  return CSERIAL_ERROR_GENERIC;

#elif defined( HAVE_LINUX_SERIAL )
    struct serial_rs485 rs485conf;
    if( ioctl( desc->port, TIOCGRS485, &rs485conf ) < 0 ){
	if( errno == ENOTTY ){
		/* Inappropriate ioctl for device. */
		/* This serial converter does not support RS485.  */
		/* This error can occur with an FTDI RS485 cable. */
		return CSERIAL_OK;
	}
	desc->last_errnum = errno;
        return CSERIAL_ERROR_GENERIC;
    }
    
    if( rs485conf.flags & SER_RS485_ENABLED ){
        rs485conf.flags &= ~(SER_RS485_ENABLED);

        if( ioctl( desc->port, TIOCSRS485, &rs485conf ) < 0 ){
	    desc->last_errnum = errno;
            return CSERIAL_RTS_TYPE_NOT_AVAILABLE;
        }
    }
#endif /* CSERIAL_PLATFORM_WINDOWS */
    return CSERIAL_OK;
}

static int set_rts_hw(c_serial_port_type* port) {
#ifdef CSERIAL_PLATFORM_WINDOWS
	serial_io_type newio;
	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

    newio.fRtsControl = RTS_CONTROL_TOGGLE;

	if (c_serial_set_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#elif defined( HAVE_LINUX_SERIAL )
    struct serial_rs485 rs485conf;
    if( ioctl(port->port, TIOCGRS485, &rs485conf ) < 0 ){
        return CSERIAL_ERROR_GENERIC;
    }
    
    rs485conf.flags |= SER_RS485_ENABLED;

    if( ioctl(port->port, TIOCSRS485, &rs485conf ) < 0 ){
        return CSERIAL_RTS_TYPE_NOT_AVAILABLE;
    }
#endif /* CSERIAL_PLATFORM_WINDOWS */
    return CSERIAL_OK;
}

/* try to set the RTS control at a driver level */
static int set_rts_settings(c_serial_port_type* port) {
    int retval = CSERIAL_ERROR_GENERIC;

    if( port->rs485 == CSERIAL_RTS_NONE ){
       port->rs485_is_software = 0;
       retval = clear_rts( port );
    }else if( port->rs485 == CSERIAL_RTS_HARDWARE ){
       port->rs485_is_software = 0;
       retval = set_rts_hw( port );
    }else if( port->rs485 == CSERIAL_RTS_SOFTWARE ){
       port->rs485_is_software = 1;
    }else if( port->rs485 == CSERIAL_RTS_BEST_AVAILABLE ){
       port->rs485_is_software = 0;
       retval = set_rts_hw( port );
       if( retval != CSERIAL_OK ){
           port->rs485_is_software = 1;
           retval = CSERIAL_OK;
       }
    }

    return retval;
}

static int set_raw_input(c_serial_port_type* port) {
	serial_io_type newio;

	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    newio.fBinary = TRUE;
    newio.fParity = TRUE;
    newio.fOutxCtsFlow = FALSE;
    newio.fOutxDsrFlow = FALSE;
    newio.fDtrControl = DTR_CONTROL_DISABLE;
    newio.fDsrSensitivity = FALSE;
    newio.fOutX = FALSE;
    newio.fInX = FALSE;
    newio.fNull = FALSE;
    newio.fRtsControl = FALSE;

    /* Set timeouts */
    {
        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 0;
        if( SetCommTimeouts( port->port, &timeouts ) == 0 ) {
            port->last_errnum = GetLastError();
			CSERIALDBG( "Unable to set comm timeouts" );
            return 0;
        }
    }
#else /* CSERIAL_PLATFORM_WINDOWS */
    newio.c_iflag |= IGNBRK;
    newio.c_iflag &= ~BRKINT;
    newio.c_iflag &= ~ICRNL;
    newio.c_oflag = 0;
    newio.c_lflag = 0;
    newio.c_cc[VTIME] = 0;
    newio.c_cc[VMIN] = 1;
#endif /* CSERIAL_PLATFORM_WINDOWS */

	if (c_serial_set_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

    return CSERIAL_OK;
}

static int set_baud_rate(c_serial_port_type* desc, int baud_rate) {
	serial_io_type newio;

	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(desc, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

    switch(baud_rate) {
		DEFAULT_SPEED_SWITCH(baud_rate, newio);
#ifndef CSERIAL_PLATFORM_WINDOWS
        /* Note that Windows only supports speeds of 110 and above */
        SPEED_SWITCH(0,newio);
        SPEED_SWITCH(50,newio);
        SPEED_SWITCH(75,newio);
#endif /*CSERIAL_PLATFORM_WINDOWS*/
        SPEED_SWITCH(110,newio);
#ifndef CSERIAL_PLATFORM_WINDOWS
        /* Windows does not support speeds of 134, 150, or 200 */
        SPEED_SWITCH(134,newio);
        SPEED_SWITCH(150,newio);
        SPEED_SWITCH(200,newio);
#endif /*CSERIAL_PLATFORM_WINDOWS*/
        SPEED_SWITCH(300,newio);
        SPEED_SWITCH(600,newio);
        SPEED_SWITCH(1200,newio);
#ifndef CSERIAL_PLATFORM_WINDOWS
        /* Windows does not support 1800 */
        SPEED_SWITCH(1800,newio);
#endif /*CSERIAL_PLATFORM_WINDOWS*/
        SPEED_SWITCH(2400,newio);
        SPEED_SWITCH(4800,newio);
        SPEED_SWITCH(9600,newio);
        SPEED_SWITCH(19200,newio);
        SPEED_SWITCH(38400,newio);
        SPEED_SWITCH(115200,newio);
    }

	if (c_serial_set_serial_port_struct(desc, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

    return CSERIAL_OK;
}

/**
 * @param data_bits The number of data bits
 */
static int set_data_bits(
	c_serial_port_type* port,
    enum CSerial_Data_Bits data_bits ) {
	serial_io_type newio;

	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    newio.ByteSize = data_bits;
#else /*CSERIAL_PLATFORM_WINDOWS*/
    newio.c_cflag &= ~CSIZE;
    if( data_bits == CSERIAL_BITS_8 ) {
        newio.c_cflag |= CS8;
    } else if( data_bits == CSERIAL_BITS_7 ) {
        newio.c_cflag |= CS7;
    } else if( data_bits == CSERIAL_BITS_6 ) {
        newio.c_cflag |= CS6;
    } else if( data_bits == CSERIAL_BITS_5 ) {
        newio.c_cflag |= CS5;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	if (c_serial_set_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

/**
 * @param stop_bits 1 for 1, 2 for 2
 */
static int set_stop_bits( 
	c_serial_port_type* port,
    enum CSerial_Stop_Bits stop_bits ) {
	serial_io_type newio;

	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    if( stop_bits == CSERIAL_STOP_BITS_1 ) {
        newio.StopBits = ONESTOPBIT;
    } else if( stop_bits == CSERIAL_STOP_BITS_2 ) {
        newio.StopBits = TWOSTOPBITS;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    if( stop_bits == CSERIAL_STOP_BITS_1 ) {
        newio.c_cflag &= ~CSTOPB;
    } else if( stop_bits == CSERIAL_STOP_BITS_2 ) {
        newio.c_cflag |= CSTOPB;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	if (c_serial_set_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

    return CSERIAL_OK;
}

/**
 * @param parity 0 for no parity, 1 for odd parity, 2 for even parity
 */
static int set_parity( 
	c_serial_port_type* port, 
	enum CSerial_Parity parity ) {

	serial_io_type newio;

	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    if( parity == CSERIAL_PARITY_NONE ) {
        newio.Parity = NOPARITY;
    } else if( parity == CSERIAL_PARITY_ODD ) {
        newio.Parity = ODDPARITY;
    } else if( parity == CSERIAL_PARITY_EVEN ) {
        newio.Parity = EVENPARITY;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    newio.c_iflag &= ~IGNPAR;
    newio.c_cflag &= ~( PARODD | PARENB );
    if( parity == CSERIAL_PARITY_NONE ) {
        newio.c_iflag |= IGNPAR;
    } else if( parity == CSERIAL_PARITY_ODD ) {
        newio.c_cflag |= PARODD;
    } else if( parity == CSERIAL_PARITY_EVEN ) {
        newio.c_cflag |= PARENB;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	if (c_serial_set_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

    return CSERIAL_OK;
}

/**
 * Check to make sure that the RTS handling can be set.
 * If RS485 handling is set, hardware flow control cannot be used 
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
 * Set flow control and the RTS handling
 *
 * @param flow_control 0 for none, 1 for hardware, 2 for software
 * @param rts_handling RTS handling according to the enum
 */
static int set_flow_control( c_serial_port_type* desc,
                             enum CSerial_Flow_Control flow_control,
                             enum CSerial_RTS_Handling rts_handling ) {
    int rc;
    int status = CSERIAL_OK;
	serial_io_type newio;

	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(desc, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

    rc = check_rts_handling( desc );
    if( rc != CSERIAL_OK ){
        return rc;
    }

#ifdef CSERIAL_PLATFORM_WINDOWS
    if( flow_control == CSERIAL_FLOW_NONE ) {
        newio.fOutxCtsFlow = FALSE;
        newio.fRtsControl = FALSE;
        newio.fOutX = FALSE;
        newio.fInX = FALSE;
    } else if( flow_control == CSERIAL_FLOW_HARDWARE ) {
        newio.fOutxCtsFlow = TRUE;
        newio.fRtsControl = TRUE;
        newio.fOutX = FALSE;
        newio.fInX = FALSE;
    } else if( flow_control == CSERIAL_FLOW_SOFTWARE ) {
        newio.fOutxCtsFlow = FALSE;
        newio.fRtsControl = FALSE;
        newio.fOutX = TRUE;
        newio.fInX = TRUE;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    newio.c_iflag &= ~( IXON | IXOFF | IXANY );
    newio.c_cflag &= ~HW_FLOW;
    if( flow_control == CSERIAL_FLOW_NONE ) {
        newio.c_iflag &= ~( IXON | IXOFF | IXANY );
    } else if( flow_control == CSERIAL_FLOW_HARDWARE ) {
        newio.c_cflag |= HW_FLOW;
    } else if( flow_control == CSERIAL_FLOW_SOFTWARE ) {
        newio.c_iflag |= ( IXON | IXOFF | IXANY );
    }
#endif /* CSERIAL_PLATFORM_WINDOWS */

	if (c_serial_set_serial_port_struct(desc, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

    status = set_rts_settings( desc );

    return status;
}

/*
 * Method Implementations
 */

int c_serial_new( c_serial_port_type** port, c_serial_errnum_t* errnum ) {
    c_serial_port_type* new_port;

    if( port == NULL ) {
        return CSERIAL_ERROR_CANT_CREATE;
    }

    new_port = malloc( sizeof( struct c_serial_port ) );
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
    new_port->rs485_is_software = 0;

#ifdef CSERIAL_PLATFORM_WINDOWS
  new_port->port = INVALID_HANDLE_VALUE;
  new_port->mutex = CreateMutex( NULL, FALSE, NULL );
	new_port->cancel_read_event = CreateEvent(NULL, TRUE, FALSE, NULL);

	memset(&(new_port->read_overlap), 0, sizeof(OVERLAPPED));
	memset(&(new_port->write_overlap), 0, sizeof(OVERLAPPED));
	new_port->read_overlap.hEvent = CreateEvent(0, FALSE, FALSE, 0);
	new_port->write_overlap.hEvent = CreateEvent(0, FALSE, FALSE, 0);

    if( new_port->mutex == NULL || new_port->cancel_read_event == NULL || new_port->read_overlap.hEvent == NULL || new_port->write_overlap.hEvent == NULL ) {
        /* Unable to create mutex for some reason, error out */
        if( errnum != NULL ) *errnum = GetLastError();

		if (new_port->mutex)
			CloseHandle(new_port->mutex);
		if (new_port->cancel_read_event)
			CloseHandle(new_port->cancel_read_event);
		if (new_port->read_overlap.hEvent)
			CloseHandle(new_port->read_overlap.hEvent);
		if (new_port->write_overlap.hEvent)
			CloseHandle(new_port->write_overlap.hEvent);

        free( new_port );
        return CSERIAL_ERROR_CANT_CREATE;
    }
	
#else /*CSERIAL_PLATFORM_WINDOWS*/
    new_port->port = -1;
    pthread_mutex_init( &(new_port->mutex), NULL );
#endif /* CSERIAL_PLATFORM_WINDOWS */

    *port = new_port;

    return CSERIAL_OK;
}

void c_serial_free( c_serial_port_type* port ) {
    if( port == NULL ) 
        return;

    c_serial_close( port );

#ifdef CSERIAL_PLATFORM_WINDOWS
	CloseHandle(port->mutex);
	CloseHandle(port->cancel_read_event);
	CloseHandle(port->read_overlap.hEvent);
	CloseHandle(port->write_overlap.hEvent);
#else
	pthread_mutex_destroy(&(port->mutex));
#endif

    free( port->port_name );
    port->port_name = NULL;

    free( port );
}

void c_serial_close( c_serial_port_type* port ) {
    if( port == NULL ) {
        return;
    }

	c_serial_read_cancel(port, -1);

	port->is_open = 0;

#ifdef CSERIAL_PLATFORM_WINDOWS
  if (port->port != INVALID_HANDLE_VALUE)
    close( port->port );
  port->port = INVALID_HANDLE_VALUE;

  WaitForSingleObject( port->mutex, INFINITE );
	ReleaseMutex( port->mutex );
#else /*CSERIAL_PLATFORM_WINDOWS*/
  if (port->port)
    close( port->port );
  port->port = -1;

  pthread_mutex_lock( &(port->mutex) );
	pthread_mutex_unlock( &(port->mutex) );
#endif /*CSERIAL_PLATFORM_WINDOWS*/
}

int c_serial_open( c_serial_port_type* port ) {
    return c_serial_open_keep_settings( port, 0 );
}

int c_serial_open_keep_settings( 
	c_serial_port_type* port, 
	int keep_settings ) {

    int rc;
    int retval = CSERIAL_OK;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    if( port->is_open ) 
		return CSERIAL_ERROR_ALREADY_OPEN;
    if( port->port_name == NULL ) 
		return CSERIAL_ERROR_NO_PORT;
    rc = check_rts_handling( port );
    if( rc != CSERIAL_OK ){
        return rc;
    }

#ifdef CSERIAL_PLATFORM_WINDOWS
    port->port = CreateFileA( port->port_name,
                             GENERIC_READ | GENERIC_WRITE,
                             0, 0,
                             OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED,
                             0 );
    if( port->port == INVALID_HANDLE_VALUE ) {
        port->last_errnum = GetLastError();
        if( port->last_errnum == ERROR_FILE_NOT_FOUND ) {
            return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
        }
        return CSERIAL_ERROR_GENERIC;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    port->port = open( port->port_name, O_RDWR );
    if( port->port < 0 ) {
        port->last_errnum = errno;
        if( port->last_errnum == ENOENT ) {
            return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
        }
        return CSERIAL_ERROR_GENERIC;
    }

    {
        struct termios io_tmp;
        if( tcgetattr( port->port, &io_tmp ) < 0 ) {
            port->last_errnum = errno;
            if( port->last_errnum == ENOTTY ) {
                return CSERIAL_ERROR_NOT_A_SERIAL_PORT;
            }
            return CSERIAL_ERROR_GENERIC;
        }
    }
#endif /* CSERIAL_PLATFORM_WINDOWS */

    port->is_open = 1;

    /* Set all of our settings.  If there are any erorrs, close
     * the port and bail out
     */
	do {
		if (keep_settings)
			break;
		
		retval = set_raw_input(port);
		if (retval) {
			c_serial_close(port);
			break;
		}
		retval = set_baud_rate(port, port->baud_rate);
		if (retval) {
			c_serial_close(port);
			break;
		}
		retval = set_data_bits(port, port->data_bits);
		if (retval) {
			c_serial_close(port);
			break;
		}
		retval = set_stop_bits(port, port->stop_bits);
		if (retval) {
			c_serial_close(port);
			break;
		}
		retval = set_parity(port, port->parity);
		if (retval) {
			c_serial_close(port);
			break;
		}
		retval = set_flow_control(port, port->flow, port->rs485);
		if (retval) {
			c_serial_close(port);
			break;
		}
	} while (0);

    return retval;
}

int c_serial_is_open( c_serial_port_type* port ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    return port->is_open;
}

int c_serial_set_port_name( c_serial_port_type* port,
                            const char* port_name ) {
    size_t port_name_len;
    int port_name_offset;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    port_name_len = strlen( port_name );
#ifdef CSERIAL_PLATFORM_WINDOWS
	if( port_name_len > 6 ){
		return CSERIAL_ERROR_NAME_TOO_LONG;
	}
    port_name_offset = 4;
    port_name_len += 5; /* add in \\.\ to the front and have NULL terminator */
#else /*CSERIAL_PLATFORM_WINDOWS*/
	if( port_name_len > 255 ){
		return CSERIAL_ERROR_NAME_TOO_LONG;
	}
    port_name_offset = 0;
    port_name_len += 1;
#endif /*CSERIAL_PLATFORM_WINDOWS*/

    if (port->port_name) {
      free((void*)port->port_name);
      port->port_name = NULL;
    }

    port->port_name = malloc( port_name_len );
    if (!port->port_name)
      return CSERIAL_ERROR_NO_MEMORY;

    memset( port->port_name, 0, port_name_len );
    memcpy( port->port_name + port_name_offset,
            port_name,
            strlen( port_name ) );
#ifdef CSERIAL_PLATFORM_WINDOWS
    memcpy( port->port_name, "\\\\.\\", 4 );
#endif /*CSERIAL_PLATFORM_WINDOWS*/

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

    port->baud_rate = baud;
    
	if( port->is_open ) {
        return set_baud_rate( port, port->baud_rate );
    }

    return CSERIAL_OK;
}

int c_serial_get_baud_rate( 
	c_serial_port_type* port ) {
    int baud_return;
	serial_io_type newio;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    if( !port->is_open ) 
        return port->baud_rate;

	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    GetCommState( port->port, &newio );
    switch( newio.BaudRate ) {
		DEFAULT_GET_SPEED_SWITCH(newio.BaudRate, newio);
#else /*CSERIAL_PLATFORM_WINDOWS*/
    switch( cfgetispeed( &newio ) ) {
		DEFAULT_GET_SPEED_SWITCH(cfgetispeed(&newio), newio);
            GET_SPEED_SWITCH( 0, newio );
            GET_SPEED_SWITCH( 50, newio );
            GET_SPEED_SWITCH( 75, newio );
#endif /* CSERIAL_PLATFORM_WINDOWS */
            GET_SPEED_SWITCH( 110, newio );
#ifndef CSERIAL_PLATFORM_WINDOWS
            GET_SPEED_SWITCH( 134, newio );
            GET_SPEED_SWITCH( 150, newio );
            GET_SPEED_SWITCH( 200, newio );
#endif /* CSERIAL_PLATFORM_WINDOWS */
            GET_SPEED_SWITCH( 300, newio );
            GET_SPEED_SWITCH( 600, newio );
            GET_SPEED_SWITCH( 1200, newio );
#ifndef CSERIAL_PLATFORM_WINDOWS
            GET_SPEED_SWITCH( 1800, newio );
#endif /* CSERIAL_PLATFORM_WINDOWS */
            GET_SPEED_SWITCH( 2400, newio );
            GET_SPEED_SWITCH( 4800, newio );
            GET_SPEED_SWITCH( 9600, newio );
            GET_SPEED_SWITCH( 19200, newio );
            GET_SPEED_SWITCH( 38400, newio );
            GET_SPEED_SWITCH( 115200, newio );
    } /* end switch */

    port->baud_rate = baud_return;
    return port->baud_rate;
}

int c_serial_set_data_bits( c_serial_port_type* port,
                            enum CSerial_Data_Bits bits ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    port->data_bits = bits;
    if( port->is_open ) {
        return set_data_bits( port, port->data_bits );
    }

    return CSERIAL_OK;
}

enum CSerial_Data_Bits c_serial_get_data_bits( c_serial_port_type* port ) {
	serial_io_type newio;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
        return CSERIAL_ERROR_GENERIC;
		
#ifdef CSERIAL_PLATFORM_WINDOWS
    switch( newio.ByteSize ) {
    case 5:
        return CSERIAL_BITS_5;
    case 6:
        return CSERIAL_BITS_6;
    case 7:
        return CSERIAL_BITS_7;
    case 8:
        return CSERIAL_BITS_8;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    if( ( newio.c_cflag | CS8 ) == CS8 ) {
        return CSERIAL_BITS_8;
    } else if( ( newio.c_cflag | CS7 ) == CS7 ) {
        return CSERIAL_BITS_7;
    } else if( ( newio.c_cflag | CS6 ) == CS6 ) {
        return CSERIAL_BITS_6;
    } else if( ( newio.c_cflag | CS5 ) == CS5 ) {
        return CSERIAL_BITS_5;
    } else {
        return 0;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	return -1;
}

int c_serial_set_stop_bits( c_serial_port_type* port,
                            enum CSerial_Stop_Bits bits ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    port->stop_bits = bits;
    if( port->is_open ) {
        return set_stop_bits( port, port->stop_bits );
    }

    return CSERIAL_OK;
}

enum CSerial_Stop_Bits c_serial_get_stop_bits( c_serial_port_type* port ) {
	serial_io_type newio;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    port->stop_bits = newio.StopBits;
    if( newio.StopBits == 1 ) {
        return 1;
    } else if( newio.StopBits == 2 ) {
        return 2;
    } else {
        return -1;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    if( newio.c_cflag & CSTOPB ) {
        port->stop_bits = 2;
        return 2;
    } else {
        port->stop_bits = 1;
        return 1;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/
}

int c_serial_set_parity( c_serial_port_type* port,
                         enum CSerial_Parity parity ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    port->parity = parity;
    if( port->is_open ) {
        return set_parity( port, port->parity );
    }

    return CSERIAL_OK;
}

enum CSerial_Parity c_serial_get_parity( c_serial_port_type* port ) {
	serial_io_type newio;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    if( newio.Parity == NOPARITY ) {
        return 0;
    } else if( newio.Parity == ODDPARITY ) {
        return 1;
    } else if( newio.Parity == EVENPARITY ) {
        return 2;
    } else {
        return -1;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    if( !( newio.c_cflag & PARENB ) ) {
        /* No parity */
        return 0;
    } else if( newio.c_cflag & PARODD ) {
        /* Odd parity */
        return 1;
    } else if( !( newio.c_cflag & PARODD ) ) {
        /* Even parity */
        return 2;
    } else {
        return -1;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/
}

int c_serial_set_flow_control( c_serial_port_type* port,
                               enum CSerial_Flow_Control control ) {
    int ok = CSERIAL_OK;
    enum CSerial_Flow_Control old;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

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
        ok = set_flow_control( port, port->flow, port->rs485 );
    }

    return ok;
}

enum CSerial_Flow_Control c_serial_get_flow_control( 
	c_serial_port_type* port) {

	serial_io_type newio;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;
	
	c_serial_init_serial_io(&newio);
	if (c_serial_get_serial_port_struct(port, &newio) < 0)
		return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    if( newio.fOutX == TRUE && newio.fInX == TRUE ) {
        return 2;
    } else if( newio.fRtsControl == TRUE && newio.fOutxCtsFlow == TRUE ) {
        return 1;
    } else {
        return 0;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    if( newio.c_cflag & ~( IXON ) &&
            newio.c_cflag & ~( IXOFF ) &&
            newio.c_cflag & ~( IXANY ) ) {
        return 0;
    } else if( newio.c_cflag & HW_FLOW ) {
        return 1;
    } else if( newio.c_cflag & ( IXON ) &&
               newio.c_cflag & ( IXOFF ) &&
               newio.c_cflag & ( IXANY ) ) {
        return 2;
    }
#endif /* CSERIAL_PLATFORM_WINDOWS */

    return 0;
}

int c_serial_write_data(c_serial_port_type* port,
                         const void *data,
                         int* length ) {
#ifdef CSERIAL_PLATFORM_WINDOWS
	DWORD bytes_written;
#else /*CSERIAL_PLATFORM_WINDOWS*/
    int bytes_written;
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	c_serial_set_rts_if_required( port );

#ifdef CSERIAL_PLATFORM_WINDOWS
    if( !WriteFile( port->port, data, *length, NULL, &(port->write_overlap) ) ) {
        port->last_errnum = GetLastError();
        if( GetLastError() == ERROR_IO_PENDING ) {
            /* Probably not an error, we're just doing this in an async fasion */
            if( WaitForSingleObject( port->write_overlap.hEvent, INFINITE ) == WAIT_FAILED ) {
                port->last_errnum = GetLastError();
				CSERIALDBG( "Unable to write data out: OVERLAPPED operation failed" );
                return CSERIAL_ERROR_GENERIC;
            }
        } else {
			CSERIALDBG( "Unable to write data" );
            return CSERIAL_ERROR_GENERIC;
        }
    }

	if( GetOverlappedResult( port->port, &(port->write_overlap), &bytes_written, 1 ) == 0 ){
		CSERIALDBG( "Unable to write data" );
		return CSERIAL_ERROR_GENERIC;
	}
#else /*CSERIAL_PLATFORM_WINDOWS*/
    bytes_written = write( port->port, data, *length );
    if( bytes_written < 0 ) {
        port->last_errnum = errno;
		CSERIALDBG( "Unable to write data to serial port" );
        return CSERIAL_ERROR_GENERIC;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/
	*length = bytes_written;

	c_serial_clear_rts_if_required( port );

    return CSERIAL_OK;
}


int c_serial_write_data_timeout(
	c_serial_port_type* port,
	const void* data,
	int* length,
	int timeout_msec) {
#ifdef CSERIAL_PLATFORM_WINDOWS
	DWORD bytes_written;
	ULONGLONG start_timestamp = c_serial_get_tick_count();
#else /*CSERIAL_PLATFORM_WINDOWS*/
	int bytes_written;
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	c_serial_set_rts_if_required(port);

#ifdef CSERIAL_PLATFORM_WINDOWS
	if (!WriteFile(port->port, data, *length, NULL, &(port->write_overlap))) {
		port->last_errnum = GetLastError();
		if (GetLastError() == ERROR_IO_PENDING) {
			DWORD wait_time = INFINITE;

			if (timeout_msec >= 0) {
				DWORD time_elapsed = (DWORD)(c_serial_get_tick_count() - start_timestamp);
				if (time_elapsed >= (DWORD)timeout_msec)
					return CSERIAL_ERROR_TIMEOUT;

				wait_time = (DWORD)timeout_msec - time_elapsed;
			}

			/* Probably not an error, we're just doing this in an async fasion */
			if (WaitForSingleObject(port->write_overlap.hEvent, wait_time) == WAIT_FAILED) {
				port->last_errnum = GetLastError();
				CSERIALDBG("Unable to write data out: timeout");
				return CSERIAL_ERROR_TIMEOUT;
			}
		}
		else {
			CSERIALDBG("Unable to write data");
			return CSERIAL_ERROR_GENERIC;
		}
	}

	if (GetOverlappedResult(port->port, &(port->write_overlap), &bytes_written, 1) == 0) {
		CSERIALDBG("Unable to write data");
		return CSERIAL_ERROR_GENERIC;
	}
#else /*CSERIAL_PLATFORM_WINDOWS*/
	(void)timeout_msec;

	bytes_written = write(port->port, data, *length);
	if (bytes_written < 0) {
		port->last_errnum = errno;
		CSERIALDBG("Unable to write data to serial port");
		return CSERIAL_ERROR_GENERIC;
	}
#endif /*CSERIAL_PLATFORM_WINDOWS*/
	*length = bytes_written;

	c_serial_clear_rts_if_required(port);

	return CSERIAL_OK;
}


int c_serial_read_data_timeout(
	c_serial_port_type* port,
	void* data,
	int* data_length,
	c_serial_control_lines_type* lines,
	int timeout_msec) {

	int ret_code = CSERIAL_OK;
	uint64_t start_timestamp = c_serial_get_tick_count();
	int can_read_control_state = 1;

#ifdef CSERIAL_PLATFORM_WINDOWS
	DWORD ret = 0;
	int current_available = 0;
  DWORD bytes_got;
  DWORD original_control_state;
	int got_data = 0;
	HANDLE wait_objects[2];
	DWORD wait_result = 0;
#else /*CSERIAL_PLATFORM_WINDOWS*/
	fd_set fdset;
	struct timeval timeout;
	int original_control_state = 0;
	int select_status;
	int new_control_state = 0;
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	if ((data == NULL || data_length == NULL) && lines == NULL) 
		return CSERIAL_ERROR_INCORRECT_READ_PARAMS;

	/* Set read activity flag*/
	atomic_exchange(&port->is_read_active, 1);

#ifdef CSERIAL_PLATFORM_WINDOWS
	ResetEvent(port->cancel_read_event);

	do {
    if (GetCommModemStatus(port->port, &original_control_state) == 0) {
      CSERIALDBG("Unable to get comm modem lines");
      ret_code = CSERIAL_ERROR_GENERIC;
      break;
    }

		{
			DWORD com_errors = { 0 };
			COMSTAT port_status = { 0 };
			if (!ClearCommError(port->port, &com_errors, &port_status)) {
				CSERIALDBG("Unable to ClearCommError");
        ret_code = CSERIAL_ERROR_GENERIC;
        break;
			}
			else {
				current_available = port_status.cbInQue;
			}
		}

		if (current_available) {
			/* Data is available; set the RXCHAR mask so we try to read from the port */
			ret = EV_RXCHAR;
		} else {
			/* If nothing is currently available, wait until we get an event of some kind.
			 * This could be the serial lines changing state, or it could be some data
			 * coming into the system.
			 */
			DWORD time_elapsed = (DWORD)(c_serial_get_tick_count() - start_timestamp);
			DWORD wait_time = INFINITE;

			if (timeout_msec >= 0) {
				if (time_elapsed >= (DWORD)timeout_msec) {
					ret_code = CSERIAL_ERROR_TIMEOUT;
					break;
				}

				wait_time = (DWORD)timeout_msec - time_elapsed;
			}

			SetCommMask(port->port, EV_RXCHAR | EV_CTS | EV_DSR | EV_RING);
			if (!WaitCommEvent(port->port, &ret, &(port->read_overlap))) {
				DWORD last_error = GetLastError();
				if (last_error == ERROR_IO_PENDING) {

				}
			}
			
			wait_objects[0] = port->read_overlap.hEvent;
			wait_objects[1] = port->cancel_read_event;

			wait_result = WaitForMultipleObjects(2, wait_objects, FALSE, wait_time);
			if (wait_result == WAIT_TIMEOUT) {
				ret_code = CSERIAL_ERROR_TIMEOUT;
				break;
			}

			if (wait_result == WAIT_OBJECT_0 + 1) {
				ret_code = CSERIAL_ERROR_CANCELLED;
				break;
			}
		}

		if (ret == 0 && !port->is_open) {
			/* the port was closed */
			ret_code = CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
			break;
		}

		if (ret & EV_RXCHAR && data != NULL) {
			if (!ReadFile(port->port, data, *data_length, &bytes_got, &(port->read_overlap))) {
				CSERIALDBG("Unable to read bytes from port");
				ReleaseMutex(port->mutex);
				*data_length = 0;
        ret_code = CSERIAL_ERROR_GENERIC;
        break;
			}
			got_data = 1;
			*data_length = bytes_got;
			break;
		}

		/* Check to see if anything changed that we care about */
		if ((ret & EV_CTS) && (port->line_flags & CSERIAL_LINE_FLAG_CTS)) 
			break;

		if ((ret & EV_DSR) && (port->line_flags & CSERIAL_LINE_FLAG_DSR)) 
			break;

		if ((ret & EV_RING) && (port->line_flags & CSERIAL_LINE_FLAG_RI)) 
			break;
	} while (1);

	if (ret_code == CSERIAL_OK && lines != NULL) {
    DWORD modem_lines;

		if (GetCommModemStatus(port->port, &modem_lines) == 0) {
			CSERIALDBG("Unable to get comm modem lines");
      ret_code = CSERIAL_ERROR_GENERIC;
    } else {
      memset(lines, 0, sizeof(c_serial_control_lines_type));
      lines->cts = (modem_lines & MS_CTS_ON) ? 1 : 0;
      lines->dsr = (modem_lines & MS_DSR_ON) ? 1 : 0;
      lines->dtr = port->winDTR ? 1 : 0;
      lines->rts = port->winRTS ? 1 : 0;
      lines->ri = (modem_lines & MS_RING_ON) ? 1 : 0;
    }
	}

	if (!got_data) 
		*data_length = 0;

	ReleaseMutex(port->mutex);
#else /*CSERIAL_PLATFORM_WINDOWS*/
	atomic_exchange(&port->cancel_read_event, 0);

  while (1) {
    if (timeout_msec < 0) {
      pthread_mutex_lock(&(port->mutex));
    }
    else {
      struct timespec timeout_time;
      clock_gettime(CLOCK_REALTIME, &timeout_time);
      timeout_time.tv_sec += timeout_msec / 1000;
      timeout_time.tv_nsec += (timeout_msec % 1000) * 1000;

      if (pthread_mutex_timedlock(&(port->mutex), &timeout_time) != 0) {
        ret_code = CSERIAL_ERROR_TIMEOUT;
        break;
      }
    }

    /* first get the original state of the serial port lines */
    if (ioctl(port->port, TIOCMGET, &original_control_state) < 0) {
      /* Some USB emulated serials may not support lines at all */
      can_read_control_state = 0;
      if (lines)
        lines->unsupported = 1;
    }

		uint64_t time_elapsed = c_serial_get_tick_count() - start_timestamp;
		if (timeout_msec >= 0 && time_elapsed >= (uint64_t)timeout_msec) {
			ret_code = CSERIAL_ERROR_TIMEOUT;
			break;
		}

		if (atomic_read(&port->cancel_read_event)) {
			ret_code = CSERIAL_ERROR_CANCELLED;
			break;
		}

		if (!port->is_open) {
			ret_code = CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
			break;
		}

		FD_ZERO(&fdset);
		FD_SET(port->port, &fdset);
		timeout.tv_sec = 0;
		timeout.tv_usec = 30000; /* 30,000 microseconds = 30ms */

		if (timeout_msec >= 0 && timeout_msec - time_elapsed < 10) {
			timeout.tv_usec = ((uint64_t)timeout_msec - time_elapsed + 1) * 1000;
		}

		select_status = select(port->port + 1, &fdset, NULL, NULL, &timeout);

		if (select_status < 0) {
			if (errno != EBADF) {
				port->last_errnum = errno;
				CSERIALDBG("Bad value from select");
			}

			ret_code = CSERIAL_ERROR_GENERIC;
			break;
		}

		if (select_status == 0 && can_read_control_state) {
			/* This was a timeout */
			if (ioctl(port->port, TIOCMGET, &new_control_state) < 0) {
				port->last_errnum = errno;
				CSERIALDBG("IOCTL call failed");
				
				ret_code = CSERIAL_ERROR_GENERIC;
				break;
			}

			if (new_control_state == original_control_state) {
				/* The state of the lines have not changed,
				 * continue on until something changes
							  */
				pthread_mutex_unlock(&(port->mutex));
				continue;
			}
		}

		if (FD_ISSET(port->port, &fdset) && data != NULL) {
			break;
		}

    if (select_status == 0) {
      pthread_mutex_unlock(&(port->mutex));
      continue;
    }

		if (lines != NULL && can_read_control_state) {
			/* Our line state has changed - check to see if we should ignore the
			 * change or if this is a valid reason to stop trying to read
			 */
			int should_continue = 1;
			int cts_changed = (new_control_state & TIOCM_CTS) != (original_control_state & TIOCM_CTS);
			int cd_changed = (new_control_state & TIOCM_CD) != (original_control_state & TIOCM_CD);
			int dsr_changed = (new_control_state & TIOCM_DSR) != (original_control_state & TIOCM_DSR);
			int dtr_changed = (new_control_state & TIOCM_DTR) != (original_control_state & TIOCM_DTR);
			int rts_changed = (new_control_state & TIOCM_RTS) != (original_control_state & TIOCM_RTS);
			int ri_changed = (new_control_state & TIOCM_RI) != (original_control_state & TIOCM_RI);

			if ((port->line_flags & CSERIAL_LINE_FLAG_CTS) &&
				cts_changed) {
				should_continue = 0;
			}
			if ((port->line_flags & CSERIAL_LINE_FLAG_CD) &&
				cd_changed) {
				should_continue = 0;
			}
			if ((port->line_flags & CSERIAL_LINE_FLAG_DSR) &&
				dsr_changed) {
				should_continue = 0;
			}
			if ((port->line_flags & CSERIAL_LINE_FLAG_DTR) &&
				dtr_changed) {
				should_continue = 0;
			}
			if ((port->line_flags & CSERIAL_LINE_FLAG_RTS) &&
				rts_changed) {
				should_continue = 0;
			}
			if ((port->line_flags & CSERIAL_LINE_FLAG_RI) &&
				ri_changed) {
				should_continue = 0;
			}

			if (should_continue) {
				pthread_mutex_unlock(&(port->mutex));
				continue;
			}

			break;
		}

		/* We get to this point if we either 1. have data or
		 * 2. our state has changed
				 */
		break;
	}

	if (ret_code == CSERIAL_OK 
		&& FD_ISSET(port->port, &fdset)
		&& data != NULL) {
		
		int ret = read(port->port, data, *data_length);
		if (ret < 0) {
			port->last_errnum = errno;
			CSERIALDBG("Unable to read data");
			ret_code = CSERIAL_ERROR_GENERIC;
		}
		else {
			*data_length = ret;
		}
	}
	else {
		*data_length = 0;
	}
	if (ret_code == CSERIAL_OK 
		&& lines != NULL) {

		memset(lines, 0, sizeof(struct c_serial_control_lines));
		if (!can_read_control_state) {
			lines->unsupported = 1;
		}

		if (new_control_state & TIOCM_CD) {
			/* Carrier detect */
			lines->cd = 1;
		}

		if (new_control_state & TIOCM_CTS) {
			/* CTS */
			lines->cts = 1;
		}

		if (new_control_state & TIOCM_DSR) {
			/* Data Set Ready */
			lines->dsr = 1;
		}

		if (new_control_state & TIOCM_DTR) {
			/* Data Terminal Ready */
			lines->dtr = 1;
		}

		if (new_control_state & TIOCM_RTS) {
			/* Request To Send */
			lines->rts = 1;
		}

		if (new_control_state & TIOCM_RI) {
			/* Ring Indicator */
			lines->ri = 1;
		}
	}

	pthread_mutex_unlock(&(port->mutex));
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	atomic_exchange(&port->is_read_active, 0);
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
    uint64_t start_timestamp = c_serial_get_tick_count();

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

	while (atomic_read(&port->is_read_active)) {
		uint64_t current_timestamp = c_serial_get_tick_count();
#ifdef CSERIAL_PLATFORM_WINDOWS
		ULONGLONG wait_time = INFINITE;
		SetEvent(port->cancel_read_event);
#else /*CSERIAL_PLATFORM_WINDOWS*/
		uint64_t wait_time = 0;
		atomic_exchange(&port->cancel_read_event, 1);
#endif /*CSERIAL_PLATFORM_WINDOWS*/

		if (timeout_msec >= 0) {
			if ((int)(current_timestamp - start_timestamp) >= timeout_msec) {
				ret_code = CSERIAL_ERROR_TIMEOUT;
				break;
			}
			wait_time = timeout_msec - (int)(current_timestamp - start_timestamp);
		}
		
#ifdef CSERIAL_PLATFORM_WINDOWS
		if (WaitForSingleObject(port->mutex, (DWORD)wait_time) == WAIT_OBJECT_0)
			ReleaseMutex(port->mutex);
#else /*CSERIAL_PLATFORM_WINDOWS*/
		if (timeout_msec < 0) {
			pthread_mutex_lock(&(port->mutex));
			pthread_mutex_unlock(&(port->mutex));
		} else {
			struct timespec timeout_time;
			clock_gettime(CLOCK_REALTIME, &timeout_time);
			timeout_time.tv_sec += wait_time / 1000;
			timeout_time.tv_nsec += (wait_time % 1000) * 1000;

			if (pthread_mutex_timedlock(&(port->mutex), &timeout_time) == 0) {
				pthread_mutex_unlock(&(port->mutex));
			}
		}
#endif /*CSERIAL_PLATFORM_WINDOWS*/
	}

	if (ret_code == CSERIAL_OK) {
#ifdef CSERIAL_PLATFORM_WINDOWS
		ResetEvent(port->cancel_read_event);
#else /*CSERIAL_PLATFORM_WINDOWS*/
		atomic_exchange(&port->cancel_read_event, 0);
#endif /*CSERIAL_PLATFORM_WINDOWS*/
	}

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

#ifdef CSERIAL_PLATFORM_WINDOWS
	if (out_handle)
		*out_handle = port->read_overlap.hEvent;
#else /*CSERIAL_PLATFORM_WINDOWS*/
	if (out_handle)
		*out_handle = port->port;
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	return CSERIAL_OK;
}

int c_serial_set_control_line( c_serial_port_type* port,
                               c_serial_control_lines_type* lines,
                               int return_state ) {
    int toSet = 0;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    if( lines == NULL ) 
        return CSERIAL_ERROR_GENERIC;

#ifdef CSERIAL_PLATFORM_WINDOWS
    if( lines->dtr ) {
        if( !EscapeCommFunction( port->port, SETDTR ) ) {
            port->last_errnum = GetLastError();
			CSERIALDBG( "Unable to get serial line state" );
            return CSERIAL_ERROR_GENERIC;
        }
        port->winDTR = 1;
    } else {
        if( !EscapeCommFunction( port->port, CLRDTR ) ) {
            port->last_errnum = GetLastError();
			CSERIALDBG( "Unable to get serial line state" );
            return CSERIAL_ERROR_GENERIC;
        }
        port->winDTR = 0;
    }

    if( lines->rts ) {
        if( !EscapeCommFunction( port->port, SETRTS ) ) {
            port->last_errnum = GetLastError();
			CSERIALDBG( "Unable to get serial line state" );
            return CSERIAL_ERROR_GENERIC;
        }
        port->winRTS = 1;
    } else {
        if( !EscapeCommFunction( port->port, CLRRTS ) ) {
            port->last_errnum = GetLastError();
			CSERIALDBG( "Unable to get serial line state" );
            return CSERIAL_ERROR_GENERIC;
        }
        port->winRTS = 0;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/

    if( ioctl( port->port, TIOCMGET, &toSet ) < 0 ) {
        port->last_errnum = errno;
		CSERIALDBG( "IOCTL failed when attempting to read control lines" );
        return CSERIAL_ERROR_GENERIC;
    }

    if( lines->dtr ) {
        toSet |= TIOCM_DTR;
    } else {
        toSet &= ~TIOCM_DTR;
    }

    if( lines->rts ) {
        toSet |= TIOCM_RTS;
    } else {
        toSet &= ~TIOCM_RTS;
    }

    if( ioctl( port->port, TIOCMSET, &toSet ) < 0 ) {
        port->last_errnum = errno;
		CSERIALDBG( "IOCTL failed when attempting to set control lines" );
        return CSERIAL_ERROR_GENERIC;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/

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
        return CSERIAL_ERROR_GENERIC;

    memset( lines, 0, sizeof( c_serial_control_lines_type ) );

    {
#ifdef CSERIAL_PLATFORM_WINDOWS
        DWORD get_val;
        if( GetCommModemStatus( port->port, &get_val ) == 0 ) {
            port->last_errnum = GetLastError();
			CSERIALDBG( "Unable to get serial line state" );
            return CSERIAL_ERROR_GENERIC;
        }

        if( get_val & MS_CTS_ON ) {
            lines->cts = 1;
        }

        if( get_val & MS_DSR_ON ) {
            lines->dsr = 1;
        }

        if( port->winDTR ) {
            lines->dtr = 1;
        }

        if( port->winRTS ) {
            lines->rts = 1;
        }

        if( get_val & MS_RING_ON ) {
            lines->ri = 1;
        }
#else /*CSERIAL_PLATFORM_WINDOWS*/
        int get_val;
        if( ioctl( port->port, TIOCMGET, &get_val ) < 0 ) {
            port->last_errnum = errno;
			CSERIALDBG( "IOCTL failed when attempting to read control lines" );
            return CSERIAL_ERROR_GENERIC;
        }

        if( get_val & TIOCM_CD ) {
            lines->cd = 1;
        }

        if( get_val & TIOCM_CTS ) {
            lines->cts = 1;
        }

        if( get_val & TIOCM_DSR ) {
            lines->dsr = 1;
        }

        if( get_val & TIOCM_DTR ) {
            lines->dtr = 1;
        }

        if( get_val & TIOCM_RTS ) {
            lines->rts = 1;
        }

        if( get_val & TIOCM_RI ) {
            lines->ri = 1;
        }
#endif /*CSERIAL_PLATFORM_WINDOWS*/
    }

    return CSERIAL_OK;
}

int c_serial_get_available( c_serial_port_type* port,
                            int* available ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

#ifdef CSERIAL_PLATFORM_WINDOWS
    {
        DWORD com_errors = {0};
        COMSTAT port_status = {0};
        if( !ClearCommError( port->port, &com_errors, &port_status ) ) {
            port->last_errnum = GetLastError();
			CSERIALDBG( "Unable to retrieve bytes available" );
            return CSERIAL_ERROR_GENERIC;
        } else {
            *available = port_status.cbInQue;
        }
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    if( ioctl( port->port, FIONREAD, available ) < 0 ) {
        port->last_errnum = errno;
		CSERIALDBG( "IOCTL failed when attempting to read bytes available" );
        return CSERIAL_ERROR_GENERIC;
    }
#endif /*CSERIAL_PLATFORM_WINDOWS*/

	return CSERIAL_OK;
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
    case CSERIAL_ERROR_TIMEOUT:
        return "Timeout";
    case CSERIAL_ERROR_CANCELLED:
        return "Operation cancelled";
    case CSERIAL_ERROR_NO_MEMORY:
        return "No memory";
    default:
        return "Unknown error";
    }
}

c_serial_errnum_t c_serial_get_last_native_errnum( c_serial_port_type* port ) {
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    return port->last_errnum;
}

const char** c_serial_get_serial_ports_list() {
    char** port_names;
    int port_names_size;

    port_names_size = 0;
    port_names = malloc( sizeof( char* ) * CSERIAL_MAX_PORTS);
    if (!port_names)
      return NULL;
	
    memset( port_names, 0, sizeof( char* ) * CSERIAL_MAX_PORTS);

#ifdef CSERIAL_PLATFORM_WINDOWS
    {
        int x;
        /* Brute force, baby! */
        char* port_to_open = malloc( 11 );
        if (!port_to_open) {
          free(port_names);
          return NULL;
        }

        HANDLE* port;
        for( x = 0; x <= CSERIAL_MAX_PORTS; x++ ) {
            _snprintf_s( port_to_open, 11, 11, "\\\\.\\COM%d", x );
            port = CreateFileA( port_to_open, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0 );
            if( port != INVALID_HANDLE_VALUE ||
                    ( port == INVALID_HANDLE_VALUE &&
                      GetLastError() != ERROR_FILE_NOT_FOUND ) ) {
                /* This is a valid port
                 * we could get INVALID_HANDLE_VALUE if the port is already open,
                 * so make sure we check to see if it is not that value
                 */
                port_names[port_names_size] = malloc( 6 );
                if (port_names[port_names_size]) {
                  memcpy(port_names[port_names_size], port_to_open + 4, 6);
                  port_names_size++;
                }
            }
            CloseHandle( port );
        }
        free( port_to_open );
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    {
#define CSERIAL_MAX_NAME_LENGTH 100
        struct dirent *entry;
        DIR* dir;
        int fd;
        char* device_name;

		device_name = malloc(CSERIAL_MAX_NAME_LENGTH);

        dir = opendir( "/dev/" );
        if( dir == NULL ) {
            CSERIALDBG( "Unable to open /dev" );
            return NULL;
        }
        while ( entry = readdir( dir ), entry != NULL) {
            if( snprintf( device_name, CSERIAL_MAX_NAME_LENGTH, "/dev/%s", entry->d_name ) >= (CSERIAL_MAX_NAME_LENGTH-1) ) {
                CSERIALDBG( "Ignoring device in /dev/: name too long" );
                continue;
            }
            fd = open(device_name, O_RDONLY | O_NONBLOCK );
            if( fd < 0 ) {
                switch( errno ) {
                case EACCES:
                case ENOMEDIUM:
                    /* For some reason, I get errno 22 on my laptop
                     * when opening /dev/video0, which prints out
                     * "No such device or address"
                     * Not adding it here, because that does seem bad
                     */
                    break;
                default:
                    CSERIALDBG( "Got unkown errno value" );
                }
                close( fd );
                continue;
            }

            if( isatty( fd ) && port_names_size < CSERIAL_MAX_PORTS) {
                port_names[ port_names_size ] = malloc( strlen( entry->d_name ) + 6 );
                memcpy( port_names[ port_names_size ], "/dev/", 5 );
                memcpy( port_names[ port_names_size ] + 5, entry->d_name, strlen( entry->d_name ) + 1 );
                port_names_size++;
            }

            close( fd );
        }
        closedir( dir );
        free(device_name);
    }
#endif /* CSERIAL_PLATFORM_WINDOWS */

    port_names[port_names_size + 1] = NULL;

    return (const char**)port_names;
}

void c_serial_free_serial_ports_list( const char** port_list ) {
    const char** real_port_list = (char**)port_list;
    int x;
    for( x = 0; x < CSERIAL_MAX_PORTS; x++ ) {
        if( real_port_list[x] == NULL ) {
            break;
        }
        free( (void*)real_port_list[x] );
    }

    free((void*)real_port_list );
}

int c_serial_set_rts_control( c_serial_port_type* port,
                              enum CSerial_RTS_Handling handling ){
    int ok = CSERIAL_OK;
    enum CSerial_RTS_Handling old;

	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

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
        ok = set_flow_control( port, port->flow, port->rs485 );
    }

    return ok;
}

enum CSerial_RTS_Handling c_serial_get_rts_control( c_serial_port_type* port ){
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

    return port->rs485;
}

int c_serial_flush( c_serial_port_type* port ){
	if (port == NULL)
		return CSERIAL_ERROR_INVALID_PORT;

#ifdef CSERIAL_PLATFORM_WINDOWS
    if( FlushFileBuffers( port->port ) == 0 ){
        port->last_errnum = GetLastError();
        return CSERIAL_ERROR_GENERIC;
    }
#else /*CSERIAL_PLATFORM_WINDOWS*/
    if( tcflush( port->port, TCOFLUSH ) < 0 ){
        port->last_errnum = errno;
        return CSERIAL_ERROR_GENERIC;
    }
#endif /* CSERIAL_PLATFORM_WINDOWS */

    return CSERIAL_OK;
}
