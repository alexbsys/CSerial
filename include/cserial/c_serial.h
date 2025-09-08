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
#ifndef C_SERIAL_H
#define C_SERIAL_H

/**
 * \addtogroup CSerial
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef _WIN32 
  #ifdef cserial_EXPORTS
    #define CSERIAL_EXPORT __declspec( dllexport )
  #else
    #define CSERIAL_EXPORT __declspec( dllimport )
  #endif /* CSERIAL_LIB */

	/* Static libraries don't use dllexport/dllimport */
#ifdef CSERIAL_STATIC
#undef CSERIAL_EXPORT
#define CSERIAL_EXPORT
#endif

#include <windows.h>

typedef HANDLE c_serial_handle_t;
typedef DWORD c_serial_errnum_t;

#else
    /* Linux/POSIX zone */
    #define CSERIAL_EXPORT

typedef int c_serial_handle_t;
typedef int c_serial_errnum_t;

#endif /* _WIN32 */

#define CSERIAL_MAX_PORTS 512

/*
 * Error code definitions
 */
/** Everything OK */
#define CSERIAL_OK 0
/** Generic error */
#define CSERIAL_ERROR_GENERIC -1
/** The port we tried to open is not actually a serial port */
#define CSERIAL_ERROR_NOT_A_SERIAL_PORT -2
/** The port that we tried to open does not exist */
#define CSERIAL_ERROR_NO_SUCH_SERIAL_PORT -3
/** The parameters to cserial_read were incorrect */
#define CSERIAL_ERROR_INCORRECT_READ_PARAMS -4
/** Unable to create new serial port */
#define CSERIAL_ERROR_CANT_CREATE -5
/** No port defined to open */
#define CSERIAL_ERROR_NO_PORT -6
/** Port already open */
#define CSERIAL_ERROR_ALREADY_OPEN -7
/** invalid c_serial_port_t, e.g. it is NULL */
#define CSERIAL_ERROR_INVALID_PORT -8
/** name of the port to open is too long(6 chars on Windows, 255 on POSIX */
#define CSERIAL_ERROR_NAME_TOO_LONG -9
/** Invalid parameters for flow control. This will be returned if the flow
 * control and the RTS handling conflict with each other.
 */
#define CSERIAL_ERROR_INVALID_FLOW -10
/** Returned if the specified RTS control type is not available
 */
#define CSERIAL_RTS_TYPE_NOT_AVAILABLE -11
/** Timeout occured */
#define CSERIAL_ERROR_TIMEOUT -12
/** Operation cancelled */
#define CSERIAL_ERROR_CANCELLED -13
/** No memory error */
#define CSERIAL_ERROR_NO_MEMORY -14

/**
 * Serial line flags
 */
#define CSERIAL_LINE_FLAG_CTS     ( 0x01 << 0 )
#define CSERIAL_LINE_FLAG_DSR     ( 0x01 << 1 )
#define CSERIAL_LINE_FLAG_DTR     ( 0x01 << 2 )
#define CSERIAL_LINE_FLAG_RTS     ( 0x01 << 3 )
#define CSERIAL_LINE_FLAG_RI      ( 0x01 << 4 )
#define CSERIAL_LINE_FLAG_CD      ( 0x01 << 5 )
#define CSERIAL_LINE_FLAG_NONE    0x00
#define CSERIAL_LINE_FLAG_ALL     ( CSERIAL_LINE_FLAG_CTS | \
                                    CSERIAL_LINE_FLAG_DSR | \
                                    CSERIAL_LINE_FLAG_DTR | \
                                    CSERIAL_LINE_FLAG_RTS | \
                                    CSERIAL_LINE_FLAG_RI  | \
                                    CSERIAL_LINE_FLAG_CD )

#define CSERIAL_TIMEOUT_INFINITE (-1)

/**
 * Struct representing the control line state.
 */
struct c_serial_control_lines {
    /** CD - Carrier Detect */
    int cd;
    /** CTS - Clear To Send */
    int cts;
    /** DSR - Data Set Ready */
    int dsr;
    /** DTR - Data Terminal Ready */
    int dtr;
    /** RTS - Request To Send */
    int rts;
    /** Ring Indicator(is the phone ringing?) */
    int ri;
	/** Lines are not available */
	int unsupported;
};
typedef struct c_serial_control_lines c_serial_control_lines_type;

enum CSerial_Data_Bits{
    CSERIAL_BITS_5 = 5,
    CSERIAL_BITS_6,
    CSERIAL_BITS_7,
    CSERIAL_BITS_8
};

enum CSerial_Stop_Bits{
    CSERIAL_STOP_BITS_1 = 1,
    CSERIAL_STOP_BITS_2
};

enum CSerial_Parity{
    CSERIAL_PARITY_NONE = 0,
    CSERIAL_PARITY_ODD,
    CSERIAL_PARITY_EVEN
};

enum CSerial_Flow_Control{
    CSERIAL_FLOW_NONE = 0,
    CSERIAL_FLOW_HARDWARE,
    CSERIAL_FLOW_SOFTWARE
};

enum CSerial_RTS_Handling{
    /** Do not handle the RTS line */
    CSERIAL_RTS_NONE = 0,
    /** Use hardware/driver level RTS handling.   */
    CSERIAL_RTS_HARDWARE,
    /** Use software-level RTS handling.  This will cause any calls 
     * to c_serial_write_data to block
     */
    CSERIAL_RTS_SOFTWARE,
    /** Use the best available RTS handling.
     * CSERIAL_RTS_HARDWARE will be used if it is available
     */
    CSERIAL_RTS_BEST_AVAILABLE
};

/*
 * Opaque type for interfacing with a serial port.
 */
typedef struct c_serial_port c_serial_port_type;

/**
 * Cerate a new C Serial object with default settings.
 * 
 * @param port A pointer to the pointer that will be filled in with the 
 *  new data.
 * @param errnum A pointer to the native error type if extended error
 *  information is required
 * @return CSERIAL_OK or an error code.  If there was an error, the port will
 *  not be valid.
 */
CSERIAL_EXPORT int c_serial_new( c_serial_port_type** port,
                                 c_serial_errnum_t* errnum );

/**
 * Close the serial port(if it is not already closed) and free all resources.
 * The pointer will be invalid after this call
 */
CSERIAL_EXPORT void c_serial_free( c_serial_port_type* port );

/**
 * Close the port associated with this serial port.  The port may be opened
 * again by calling c_serial_open()
 */
CSERIAL_EXPORT void c_serial_close( c_serial_port_type* port );

/**
 * Open the port on the system.
 */
CSERIAL_EXPORT int c_serial_open( c_serial_port_type* port );

/**
 * Open the port on the system, optionally keeping all settings.
 * If keepSettings is set to 0, acts the same as c_serial_open
 * 
 * @param port The port to open
 * @param keepSettings 1 if current serial port settings should be kept,
 *  0 otherwise
 */
CSERIAL_EXPORT int c_serial_open_keep_settings( c_serial_port_type* port,
                                                int keepSettings );

/**
 * Returns 1 if the port is open, 0 otherwise
 */
CSERIAL_EXPORT int c_serial_is_open( c_serial_port_type* port );

/**
 * Set the name of the port to open.
 * The value sent is platform-dependent, e.g. COM1 on Windows and /dev/ttyS1
 * on Linux and Linux-like systems.
 *
 * The port name is copied to internal memory and may be freed after.
 */
CSERIAL_EXPORT int c_serial_set_port_name( c_serial_port_type* port,
                                           const char* port_name );

/**
 * Get the port name that this serial port represents, e.g. COM1 on Windows
 * and /dev/ttyS1 on Linux and Linux-like systems
 */
CSERIAL_EXPORT const char* c_serial_get_port_name( c_serial_port_type* port );

/**
 * Set the baud rate of the serial port
 */
CSERIAL_EXPORT int c_serial_set_baud_rate( c_serial_port_type* port, 
                                           int baud );

/**
 * Get the baud rate of the serial port.  Note that this function returns 
 * a cached value if the port is not open, otherwise it reads directly
 * from the port and returns the proper value.
 */
CSERIAL_EXPORT int c_serial_get_baud_rate( c_serial_port_type* port );

/**
 * Set the number of data bits.
 */
CSERIAL_EXPORT int c_serial_set_data_bits( c_serial_port_type* port,
                                           enum CSerial_Data_Bits bits );

/**
 * Get the number of data bits
 */
CSERIAL_EXPORT enum CSerial_Data_Bits c_serial_get_data_bits( 
                                                   c_serial_port_type* port );

/**
 * Set the number of stop bits
 */
CSERIAL_EXPORT int c_serial_set_stop_bits( c_serial_port_type* port,
                                           enum CSerial_Stop_Bits bits );

/**
 * Get the number of stop bits
 */
CSERIAL_EXPORT enum CSerial_Stop_Bits c_serial_get_stop_bits(
                                                   c_serial_port_type* port );

/**
 * Set the parity 
 */
CSERIAL_EXPORT int c_serial_set_parity( c_serial_port_type* port,
                                        enum CSerial_Parity parity );

/**
 * Get the parity.
 */
CSERIAL_EXPORT enum CSerial_Parity c_serial_get_parity( 
                                                  c_serial_port_type* port );

/**
 * Set the flow control
 */
CSERIAL_EXPORT int c_serial_set_flow_control( c_serial_port_type* port,
                                             enum CSerial_Flow_Control contol );

/**
 * Get the flow control
 */
CSERIAL_EXPORT enum CSerial_Flow_Control c_serial_get_flow_control(
                                                  c_serial_port_type* port );

/**
 * Write a block of data to the serial port.
 * Acts the same as write() in POSIX systems.
 *
 * @param port The port to write data out to.
 * @param data The data to write out to the port.
 * @param length How long(in bytes) the data is.  Set to the number of bytes written on return.
 * @return status code
 */
CSERIAL_EXPORT int c_serial_write_data( c_serial_port_type* port,
                                        const void* data,
                                        int* length );

CSERIAL_EXPORT int c_serial_write_data_timeout(c_serial_port_type* port,
	const void* data,
	int* length,
	int timeout_msec);

/**
 * Read data from the serial port.
 * Acts like read() in POSIX systems, except that it will also return the 
 * state of the serial lines.  This function will block for data before it 
 * returns.  If a mask of serial lines to listen to has been set using
 * c_serial_set_serial_line_change_flags, then this function may return
 * without having read any data into the data buffer.
 * Function blocks control execution, but can be cancelled and unblocked immediately
 * with c_serial_read_cancel() call from another thread. In this case function 
 * returns CSERIAL_ERROR_CANCELLED
 *
 * If data is NULL and lines is non-NULL, but no change_flags have been set,
 * this function returns immediately without having read any data.
 * 
 * @param port The port to read data from
 * @param data The buffer to write data to.  May be NULL if you are only
 *  interested in listening for serial line changes.
 * @param data_length On entry, how long the data is.  On exit, how many bytes
 *  were read. 
 * @param lines The state of the serial lines on read of data.  If waiting for
 *  the line state to change, cannot be NULL
 * @return CSERIAL_OK on success, 
 *         CSERIAL_ERROR_CANCELLED on operation cancelled via call c_serial_read_cancel() from other thread,
 *         error code otherwise
 */
CSERIAL_EXPORT int c_serial_read_data( c_serial_port_type* port,
                                       void* data,
                                       int* data_length,
                                       c_serial_control_lines_type* lines );

/**
 * Read data from the serial port.
 * Acts like read() in POSIX systems, except that it will also return the
 * state of the serial lines.  This function will block for data before it
 * returns.  If a mask of serial lines to listen to has been set using
 * c_serial_set_serial_line_change_flags, then this function may return
 * without having read any data into the data buffer.
 * Function blocks control execution, but can be cancelled and unblocked immediately
 * with c_serial_read_cancel() call from another thread. In this case function
 * returns CSERIAL_ERROR_CANCELLED
 *
 * Execution control will be blocked for timeout_msec time. If timeout elapsed,
 * but nothing received, function will return CSERIAL_ERROR_TIMEOUT. Serial read
 * event is not cancelled in this case, so user can continue to polling handle by himself
 * If timeout_msec < 0, it means infinite wait.
 *
 * If data is NULL and lines is non-NULL, but no change_flags have been set,
 * this function returns immediately without having read any data.
 *
 * @param port The port to read data from
 * @param data The buffer to write data to.  May be NULL if you are only
 *  interested in listening for serial line changes.
 * @param data_length On entry, how long the data is.  On exit, how many bytes
 *  were read.
 * @param lines The state of the serial lines on read of data.  If waiting for
 *  the line state to change, cannot be NULL
 * @param timeout_msec  Wait timeout for serial data or state change, in milliseconds. 
 *  If value less than zero, it means wait infinite.
 * @return CSERIAL_OK on success, 
 *         CSERIAL_ERROR_TIMEOUT on timeout happened,
 *         CSERIAL_ERROR_CANCELLED on operation cancelled via call c_serial_read_cancel() from other thread,
 *         error code otherwise
 */
CSERIAL_EXPORT int c_serial_read_data_timeout(
	c_serial_port_type* port,
	void* data,
	int* data_length,
	c_serial_control_lines_type* lines,
	int timeout_msec);

/**
 * Get the native handle(int or HANDLE) that is used by this serial port.
 * 
 * Note that on Windows, this will return the actual handle used for 
 * the ReadFile/WriteFile system calls.  To get the HANDLE for the 
 * Event, use c_serial_get_poll_handle()
 */
CSERIAL_EXPORT int c_serial_get_native_handle(
	c_serial_port_type* port, 
	c_serial_handle_t* out_handle);

/**
 * Get the native handle used for a poll()-like function.
 * On POSIX systems, this will return the same as 
 * c_serial_get_native_handle()
 *
 * On Windows, this will return a HANDLE to an created with the 
 * CreateEvent system call, which can then be used to listen for changes
 * using a function such as MsgWaitForMultipleObjectsEx or 
 * WaitForSingleObject.
 */
CSERIAL_EXPORT int c_serial_get_poll_handle(
	c_serial_port_type* port,
	c_serial_handle_t* out_handle);

/**
 * Set the state of the control lines, optionally returning the current 
 * state of the lines after setting them.
 * 
 * @param port The port to set the line state for.
 * @param lines The line state to set.  Note that only DTR and RTS can be set.
 * @param return_state If true, modify lines on return with the current state
 *  of the serial lines.
 */
CSERIAL_EXPORT int c_serial_set_control_line( c_serial_port_type* port,
                                              c_serial_control_lines_type* lines,
                                              int return_state );

/**
 * Get the current state of the serial lines
 */
CSERIAL_EXPORT int c_serial_get_control_lines( c_serial_port_type* port,
                                              c_serial_control_lines_type* lines );

/**
 * Get the number of bytes currently available
 * 
 * @param port The port to get the number of bytes currently available from
 * @param available The number of bytes available
 * @return CSERIAL_OK or an error code
 */
CSERIAL_EXPORT int c_serial_get_available( c_serial_port_type* port,
                                           int* available );

/**
 * Set the serial lines that will cause a return from c_serial_read
 *
 * @param port The port to set the line bitmask for
 * @param flags Bitwise-OR of any CSERIAL_LINE_FLAG_XXX
 */
CSERIAL_EXPORT int c_serial_set_serial_line_change_flags( c_serial_port_type* port,
                                                          int flags );

/**
 * Get the serial line flags that will cause a return from c_serial_read
 */
CSERIAL_EXPORT int c_serial_get_serial_line_change_flags( 
                                                    c_serial_port_type* port );

/**
 * Set any user data that should be associated with this serial port 
 */
CSERIAL_EXPORT void c_serial_set_user_data( c_serial_port_type* port, void* data );

/**
 * Get any user data associated with this serial port
 */
CSERIAL_EXPORT void* c_serial_get_user_data( c_serial_port_type* port );

/**
 * Get the text of a C Serial error number
 */
CSERIAL_EXPORT const char* c_serial_get_error_string( int errnum );

/**
 * Get the last error number associated with this port.  This corresponds
 * to errno on Linux-like systems, and GetLastError() on Windows.
 */
CSERIAL_EXPORT c_serial_errnum_t c_serial_get_last_native_errnum( 
                                                       c_serial_port_type* port );

/**
 * Get a list of all serial ports that are on the system.
 * This must be freed with c_serial_free_serial_ports_list()
 *
 * @return An array of char* listing all of the serial ports on the system
 *  e.g. /dev/ttyS1, /dev/ttyS2 on Linux; COM1,COM2 on Windows.  This array is 
 *  NULL-terminated.
 */
CSERIAL_EXPORT const char** c_serial_get_serial_ports_list();

/**
 * Free the list of serial ports retrieved with c_serial_get_serial_ports
 */
CSERIAL_EXPORT void c_serial_free_serial_ports_list( const char** port_list );

/**
 * Set how RTS should be controlled.  This is generally used for RS-485 
 * converters.  Note that this logically separate from the flow control, even
 * if they both use RTS.  That is, if c_serial_set_flow_control() is set with 
 * HARDWARE flow control, this will override that setting.
 *
 * Important note regarding setting the RTS control: If the port is not open,
 * this function may return CSERIAL_OK if the setting is valid, and upon
 * opening the port CSERIAL_RTS_TYPE_NOT_AVAILABLE may be returned.
 *
 * @param handling How to handle setting the RTS control.  HARDWARE indicates 
 * that we will attempt to use the OS-level way of setting the RTS line.
 * SOFTWARE indicates that we will set the RTS line on the library side.
 * NONE turns it off.
 */
CSERIAL_EXPORT int c_serial_set_rts_control( c_serial_port_type* port,
                                        enum CSerial_RTS_Handling handling );

/**
 * Get the current handling of the RTS line for RS485 handling
 */
CSERIAL_EXPORT enum CSerial_RTS_Handling c_serial_get_rts_control( 
                                                       c_serial_port_type* port );

/**
 * Flush all data.
 */
CSERIAL_EXPORT int c_serial_flush( c_serial_port_type* port );

/**
 * Terminate c_serial_read or c_serial_read_timeout function which called on another thread.
 * Read function will unblock execution control as fast as possible. 
 * c_serial_read_cancel can wait for read operation termination, or return control immediately.
 * timeout_msec means the timeout for wait until all read operations will be cancelled.
 * This function is cancel only current operations, but not future calls for serial read.
 * @param  port  The serial port object
 * @param  timeout_msec  How much time wait for read operation has been terminated, in millisecond.
 *         Negative values means infinite wait
 * @return  CSERIAL_OK - read operations were not started or terminated successfully,
 *          CSERIAL_ERROR_TIMEOUT - timeout occured,
 *          any other values - error code
 */
CSERIAL_EXPORT int c_serial_read_cancel(c_serial_port_type* port, int timeout_msec);

#ifdef __cplusplus
} /* end extern "C" */
#endif /* __cplusplus */

/** @} */

#endif /* C_SERIAL_H */
