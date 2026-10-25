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
 * Windows backend: overlapped I/O on COM ports.
 *
 * The port is opened with FILE_FLAG_OVERLAPPED, so ReadFile(), WriteFile()
 * and WaitCommEvent() start an operation and return at once; completion is
 * signalled through the event in an OVERLAPPED structure.  This allows
 * waiting with a timeout and on the cancel event at the same time.
 *
 * All port settings live in a DCB (Device Control Block): BaudRate,
 * ByteSize, Parity, StopBits and the flow control fields.  Every setter reads
 * the current DCB, changes its fields and writes it back.
 */

#include "c_serial_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "\\.\" opens the device namespace directly; plain "COM10" and above
 * would not open without it */
const char cserial_os_port_name_prefix[] = "\\\\.\\";
/* "COM255" */
const size_t cserial_os_port_name_max_length = 6;

/* Read the port settings into dcb.  DCBlength must be set by the caller of
 * GetCommState(); -1 with last_errnum on failure. */
static int get_dcb(c_serial_port_type* port, DCB* dcb) {
	memset(dcb, 0, sizeof(DCB));
	dcb->DCBlength = sizeof(DCB);
	if (!GetCommState(port->port, dcb)) {
		port->last_errnum = GetLastError();
		CSERIALDBG("bad get comm line %d\n", __LINE__);
		return -1;
	}
	return 0;
}

/* Apply dcb to the port; the driver rejects unsupported combinations (e.g. a
 * baud rate the UART cannot do). -1 with last_errnum on failure. */
static int set_dcb(c_serial_port_type* port, DCB* dcb) {
	if (!SetCommState(port->port, dcb)) {
		port->last_errnum = GetLastError();
		CSERIALDBG("bad set comm\n");
		return -1;
	}
	return 0;
}

uint64_t cserial_os_tick_count(void) {
	return GetTickCount64();
}

/* DCB.BaudRate is a plain number, the driver decides what it supports */
int cserial_os_is_valid_baud_rate(int baud) {
	return baud > 0;
}

int cserial_os_init(c_serial_port_type* port, c_serial_errnum_t* errnum) {
	struct c_serial_os_state* os = &port->os;

	port->port = INVALID_HANDLE_VALUE;
	/* CreateEvent(security, manual_reset, initial_state, name) */
	os->mutex = CreateMutex(NULL, FALSE, NULL);
	/* Manual-reset: stays signalled until the next read resets it, so a
	 * cancel is never lost */
	os->cancel_read_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	/* Auto-reset completion events of the overlapped operations */
	os->read_overlap.hEvent = CreateEvent(0, FALSE, FALSE, 0);
	os->write_overlap.hEvent = CreateEvent(0, FALSE, FALSE, 0);

	if (os->mutex == NULL || os->cancel_read_event == NULL ||
		os->read_overlap.hEvent == NULL || os->write_overlap.hEvent == NULL) {
		if (errnum != NULL)
			*errnum = GetLastError();
		cserial_os_destroy(port);
		return CSERIAL_ERROR_CANT_CREATE;
	}

	return CSERIAL_OK;
}

void cserial_os_destroy(c_serial_port_type* port) {
	struct c_serial_os_state* os = &port->os;

	if (os->mutex)
		CloseHandle(os->mutex);
	if (os->cancel_read_event)
		CloseHandle(os->cancel_read_event);
	if (os->read_overlap.hEvent)
		CloseHandle(os->read_overlap.hEvent);
	if (os->write_overlap.hEvent)
		CloseHandle(os->write_overlap.hEvent);
}

int cserial_os_open(c_serial_port_type* port) {
	HANDLE handle;
	DCB dcb;

	/* Share mode 0: COM ports are exclusive, a second open fails with
	 * ERROR_ACCESS_DENIED */
	handle = CreateFileA(port->port_name,
						 GENERIC_READ | GENERIC_WRITE,
						 0, 0,
						 OPEN_EXISTING,
						 FILE_FLAG_OVERLAPPED,
						 0);
	if (handle == INVALID_HANDLE_VALUE) {
		port->last_errnum = GetLastError();
		if (port->last_errnum == ERROR_FILE_NOT_FOUND ||
			port->last_errnum == ERROR_PATH_NOT_FOUND) {
			return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;
		}
		return CSERIAL_ERROR_GENERIC;
	}

	/* "\\.\<name>" can also be a volume, pipe or other device */
	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);
	if (GetFileType(handle) != FILE_TYPE_CHAR || !GetCommState(handle, &dcb)) {
		port->last_errnum = GetLastError();
		CloseHandle(handle);
		return CSERIAL_ERROR_NOT_A_SERIAL_PORT;
	}

	port->port = handle;
	return CSERIAL_OK;
}

/* The mutex waits for a running read to leave; CancelIoEx() aborts any
 * overlapped operation still pending on the handle (e.g. from a writer
 * thread) before it is closed. */
void cserial_os_close(c_serial_port_type* port) {
	WaitForSingleObject(port->os.mutex, INFINITE);
	port->is_open = 0;
	if (port->port != INVALID_HANDLE_VALUE) {
		CancelIoEx(port->port, NULL);
		CloseHandle(port->port);
	}
	port->port = INVALID_HANDLE_VALUE;
	ReleaseMutex(port->os.mutex);
}

int cserial_os_set_raw_input(c_serial_port_type* port) {
	DCB dcb;
	COMMTIMEOUTS timeouts;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	dcb.fBinary = TRUE;                     /* no EOF character processing (always TRUE on Windows) */
	dcb.fParity = TRUE;                     /* check parity when DCB.Parity enables it */
	dcb.fOutxCtsFlow = FALSE;               /* do not wait for CTS before sending */
	dcb.fOutxDsrFlow = FALSE;               /* do not wait for DSR before sending */
	dcb.fDtrControl = DTR_CONTROL_DISABLE;  /* DTR low, then set by the user */
	dcb.fDsrSensitivity = FALSE;            /* receive regardless of DSR */
	dcb.fOutX = FALSE;                      /* no XON/XOFF on output */
	dcb.fInX = FALSE;                       /* no XON/XOFF on input */
	dcb.fNull = FALSE;                      /* keep received zero bytes */
	dcb.fRtsControl = RTS_CONTROL_DISABLE;  /* RTS low, then set by the user */

	/* ReadIntervalTimeout = MAXDWORD with zero multiplier and constant:
	 * ReadFile returns immediately with whatever is buffered, even nothing.
	 * Waiting is done by wait_for_event(); write timeouts are disabled and
	 * handled by waiting on the overlapped event. */
	memset(&timeouts, 0, sizeof(timeouts));
	timeouts.ReadIntervalTimeout = MAXDWORD;
	if (SetCommTimeouts(port->port, &timeouts) == 0) {
		port->last_errnum = GetLastError();
		CSERIALDBG("Unable to set comm timeouts");
		return CSERIAL_ERROR_GENERIC;
	}

	if (set_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_set_baud_rate(c_serial_port_type* port, int baud) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	dcb.BaudRate = (DWORD)baud;

	if (set_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_baud_rate(c_serial_port_type* port) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return (int)dcb.BaudRate;
}

int cserial_os_set_data_bits(c_serial_port_type* port, enum CSerial_Data_Bits bits) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	dcb.ByteSize = (BYTE)bits;

	if (set_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_data_bits(c_serial_port_type* port) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (dcb.ByteSize >= CSERIAL_BITS_5 && dcb.ByteSize <= CSERIAL_BITS_8)
		return dcb.ByteSize;

	return CSERIAL_ERROR_GENERIC;
}

int cserial_os_set_stop_bits(c_serial_port_type* port, enum CSerial_Stop_Bits bits) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	/* StopBits is a code, not a count: ONESTOPBIT = 0, ONE5STOPBITS = 1
	 * (1.5 bits, not exposed by the API), TWOSTOPBITS = 2 */
	dcb.StopBits = (bits == CSERIAL_STOP_BITS_2) ? TWOSTOPBITS : ONESTOPBIT;

	if (set_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_stop_bits(c_serial_port_type* port) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (dcb.StopBits == ONESTOPBIT)
		return CSERIAL_STOP_BITS_1;
	if (dcb.StopBits == TWOSTOPBITS)
		return CSERIAL_STOP_BITS_2;

	return CSERIAL_ERROR_GENERIC;
}

int cserial_os_set_parity(c_serial_port_type* port, enum CSerial_Parity parity) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	/* MARKPARITY and SPACEPARITY (parity bit always 1 / 0) are not exposed */
	if (parity == CSERIAL_PARITY_ODD)
		dcb.Parity = ODDPARITY;
	else if (parity == CSERIAL_PARITY_EVEN)
		dcb.Parity = EVENPARITY;
	else
		dcb.Parity = NOPARITY;

	if (set_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_parity(c_serial_port_type* port) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (dcb.Parity == NOPARITY)
		return CSERIAL_PARITY_NONE;
	if (dcb.Parity == ODDPARITY)
		return CSERIAL_PARITY_ODD;
	if (dcb.Parity == EVENPARITY)
		return CSERIAL_PARITY_EVEN;

	return CSERIAL_ERROR_GENERIC;
}

int cserial_os_set_flow_control(c_serial_port_type* port, enum CSerial_Flow_Control flow) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	/* Hardware: the driver sends only while CTS is high (fOutxCtsFlow) and
	 * drops RTS when its input buffer fills up (RTS_CONTROL_HANDSHAKE).
	 * Software: XOFF/XON characters pause and resume the other side (fInX)
	 * and are obeyed when received (fOutX). */
	dcb.fOutxCtsFlow = FALSE;
	dcb.fRtsControl = RTS_CONTROL_DISABLE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	if (flow == CSERIAL_FLOW_HARDWARE) {
		dcb.fOutxCtsFlow = TRUE;
		dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
	} else if (flow == CSERIAL_FLOW_SOFTWARE) {
		dcb.fOutX = TRUE;
		dcb.fInX = TRUE;
	}

	if (set_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_get_flow_control(c_serial_port_type* port) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	if (dcb.fOutxCtsFlow)
		return CSERIAL_FLOW_HARDWARE;
	if (dcb.fOutX && dcb.fInX)
		return CSERIAL_FLOW_SOFTWARE;

	return CSERIAL_FLOW_NONE;
}

/* RTS_CONTROL_TOGGLE: the driver raises RTS while there are bytes to send
 * and lowers it afterwards, i.e. driver-level RS485 direction control.
 * Not every driver supports it; SetCommState() then fails. */
static int set_rts_toggle(c_serial_port_type* port) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	dcb.fRtsControl = RTS_CONTROL_TOGGLE;

	if (set_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

int cserial_os_set_rts_hardware(c_serial_port_type* port) {
	return set_rts_toggle(port);
}

int cserial_os_clear_rts_hardware(c_serial_port_type* port) {
	DCB dcb;

	if (get_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	/* Only leave toggle mode: RTS_CONTROL_HANDSHAKE belongs to hardware flow control */
	if (dcb.fRtsControl != RTS_CONTROL_TOGGLE)
		return CSERIAL_OK;

	/* Back to manual RTS (ENABLE = high, DISABLE = low), keeping the level
	 * last set by the user.  EscapeCommFunction(SETRTS) fails in toggle mode. */
	dcb.fRtsControl = port->os.rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;

	if (set_dcb(port, &dcb) < 0)
		return CSERIAL_ERROR_GENERIC;

	return CSERIAL_OK;
}

/* WriteFile() either completes at once or returns ERROR_IO_PENDING; then the
 * write_overlap event is waited for up to timeout_msec.  GetOverlappedResult()
 * reports how many bytes went out. */
int cserial_os_write(c_serial_port_type* port, const void* data, int* length, int timeout_msec) {
	OVERLAPPED* overlap = &port->os.write_overlap;
	DWORD bytes_written = 0;
	DWORD wait_result;

	if (!WriteFile(port->port, data, (DWORD)*length, NULL, overlap)) {
		DWORD err = GetLastError();
		if (err != ERROR_IO_PENDING) {
			port->last_errnum = err;
			CSERIALDBG("Unable to write data");
			*length = 0;
			return CSERIAL_ERROR_GENERIC;
		}

		wait_result = WaitForSingleObject(overlap->hEvent, timeout_msec < 0 ? INFINITE : (DWORD)timeout_msec);
		if (wait_result != WAIT_OBJECT_0) {
			if (wait_result == WAIT_FAILED)
				port->last_errnum = GetLastError();
			/* The kernel still references the caller's buffer: the write must be
			 * cancelled and finished before returning. */
			CancelIoEx(port->port, overlap);
			GetOverlappedResult(port->port, overlap, &bytes_written, TRUE);
			*length = (int)bytes_written;
			return (wait_result == WAIT_TIMEOUT) ? CSERIAL_ERROR_TIMEOUT : CSERIAL_ERROR_GENERIC;
		}
	}

	if (!GetOverlappedResult(port->port, overlap, &bytes_written, TRUE)) {
		port->last_errnum = GetLastError();
		CSERIALDBG("Unable to write data");
		*length = (int)bytes_written;
		return CSERIAL_ERROR_GENERIC;
	}

	*length = (int)bytes_written;
	return CSERIAL_OK;
}

/*
 * Waits until data is buffered (want_data) or a line selected by
 * port->line_flags changes.  Called with the port mutex held.
 *
 * Each iteration:
 *  1. ClearCommError() returns the number of buffered bytes (cbInQue) and
 *     clears the line error flags (framing, overrun, parity); with
 *     DCB.fAbortOnError set, I/O stops until they are cleared;
 *  2. checks cancel and timeout;
 *  3. SetCommMask() selects the events and WaitCommEvent() waits for one of
 *     them, together with cancel_read_event:
 *       EV_RXCHAR  a byte was received
 *       EV_CTS     CTS changed        EV_DSR   DSR changed
 *       EV_RING    RI (ring) changed  EV_RLSD  CD (carrier detect) changed
 *  4. a received byte goes back to step 1 (cbInQue is the source of truth);
 *     a line change returns only if the user asked for that line.
 */
static int wait_for_event(c_serial_port_type* port, int want_data, int* data_available,
	int timeout_msec, uint64_t start_timestamp) {
	OVERLAPPED* overlap = &port->os.read_overlap;

	*data_available = 0;

	while (1) {
		DWORD com_errors = 0;
		COMSTAT port_status;
		DWORD evt = 0;
		DWORD transferred = 0;
		DWORD wait_time = INFINITE;
		DWORD wait_result;
		HANDLE wait_objects[2];

		if (!port->is_open)
			return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;

		memset(&port_status, 0, sizeof(port_status));
		if (!ClearCommError(port->port, &com_errors, &port_status)) {
			port->last_errnum = GetLastError();
			CSERIALDBG("Unable to ClearCommError");
			return CSERIAL_ERROR_GENERIC;
		}

		if (want_data && port_status.cbInQue > 0) {
			*data_available = 1;
			return CSERIAL_OK;
		}

		if (!want_data && port->line_flags == 0)
			return CSERIAL_OK;

		if (WaitForSingleObject(port->os.cancel_read_event, 0) == WAIT_OBJECT_0)
			return CSERIAL_ERROR_CANCELLED;

		if (timeout_msec >= 0) {
			int64_t remaining = cserial_internal_remaining_ms(timeout_msec, start_timestamp);
			if (remaining == 0)
				return CSERIAL_ERROR_TIMEOUT;
			wait_time = (DWORD)remaining;
		}

		if (!SetCommMask(port->port, (want_data ? EV_RXCHAR : 0) | EV_CTS | EV_DSR | EV_RING | EV_RLSD)) {
			port->last_errnum = GetLastError();
			return CSERIAL_ERROR_GENERIC;
		}

		if (!WaitCommEvent(port->port, &evt, overlap)) {
			DWORD last_error = GetLastError();
			if (last_error != ERROR_IO_PENDING) {
				port->last_errnum = last_error;
				return CSERIAL_ERROR_GENERIC;
			}

			wait_objects[0] = overlap->hEvent;
			wait_objects[1] = port->os.cancel_read_event;
			wait_result = WaitForMultipleObjects(2, wait_objects, FALSE, wait_time);
			if (wait_result != WAIT_OBJECT_0) {
				if (wait_result == WAIT_FAILED)
					port->last_errnum = GetLastError();
				/* The pending WaitCommEvent writes to evt on this stack frame:
				 * it must be finished before leaving. */
				CancelIoEx(port->port, overlap);
				GetOverlappedResult(port->port, overlap, &transferred, TRUE);

				if (wait_result == WAIT_TIMEOUT)
					return CSERIAL_ERROR_TIMEOUT;
				if (wait_result == WAIT_OBJECT_0 + 1)
					return CSERIAL_ERROR_CANCELLED;
				return CSERIAL_ERROR_GENERIC;
			}

			if (!GetOverlappedResult(port->port, overlap, &transferred, FALSE)) {
				port->last_errnum = GetLastError();
				return CSERIAL_ERROR_GENERIC;
			}
		}

		if (evt & EV_RXCHAR)
			continue;

		if (((evt & EV_CTS) && (port->line_flags & CSERIAL_LINE_FLAG_CTS)) ||
			((evt & EV_DSR) && (port->line_flags & CSERIAL_LINE_FLAG_DSR)) ||
			((evt & EV_RING) && (port->line_flags & CSERIAL_LINE_FLAG_RI)) ||
			((evt & EV_RLSD) && (port->line_flags & CSERIAL_LINE_FLAG_CD))) {
			return CSERIAL_OK;
		}
	}
}

int cserial_os_read(c_serial_port_type* port, void* data, int* data_length,
	c_serial_control_lines_type* lines, int timeout_msec, uint64_t start_timestamp) {

	int ret_code;
	int data_available = 0;
	int got_data = 0;
	DWORD lock_result;

	lock_result = WaitForSingleObject(port->os.mutex, timeout_msec < 0 ? INFINITE : (DWORD)timeout_msec);
	if (lock_result == WAIT_TIMEOUT || lock_result == WAIT_FAILED) {
		if (lock_result == WAIT_FAILED)
			port->last_errnum = GetLastError();
		if (data_length != NULL)
			*data_length = 0;
		return lock_result == WAIT_TIMEOUT ? CSERIAL_ERROR_TIMEOUT : CSERIAL_ERROR_GENERIC;
	}

	ret_code = wait_for_event(port, data != NULL, &data_available, timeout_msec, start_timestamp);

	/* With ReadIntervalTimeout = MAXDWORD this read returns at once with
	 * up to *data_length of the buffered bytes */
	if (ret_code == CSERIAL_OK && data_available) {
		OVERLAPPED* overlap = &port->os.read_overlap;
		DWORD bytes_got = 0;

		if (!ReadFile(port->port, data, (DWORD)*data_length, NULL, overlap)) {
			DWORD last_error = GetLastError();
			if (last_error != ERROR_IO_PENDING) {
				port->last_errnum = last_error;
				CSERIALDBG("Unable to read bytes from port");
				ret_code = CSERIAL_ERROR_GENERIC;
			}
		}

		if (ret_code == CSERIAL_OK) {
			if (!GetOverlappedResult(port->port, overlap, &bytes_got, TRUE)) {
				port->last_errnum = GetLastError();
				ret_code = CSERIAL_ERROR_GENERIC;
			} else {
				got_data = 1;
				*data_length = (int)bytes_got;
			}
		}
	}

	if (ret_code == CSERIAL_OK && lines != NULL) {
		memset(lines, 0, sizeof(c_serial_control_lines_type));
		ret_code = cserial_os_get_control_lines(port, lines);
	}

	if (!got_data && data_length != NULL)
		*data_length = 0;

	ReleaseMutex(port->os.mutex);
	return ret_code;
}

void cserial_os_cancel_read_signal(c_serial_port_type* port) {
	SetEvent(port->os.cancel_read_event);
}

void cserial_os_cancel_read_reset(c_serial_port_type* port) {
	ResetEvent(port->os.cancel_read_event);
}

/* The reader holds the mutex for the whole read: getting it means the
 * reader has left.  WAIT_ABANDONED: the owner thread exited while holding
 * it, the mutex is ours anyway. */
void cserial_os_wait_for_reader(c_serial_port_type* port, int64_t timeout_msec) {
	DWORD wait_result = WaitForSingleObject(port->os.mutex, (DWORD)timeout_msec);
	if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED)
		ReleaseMutex(port->os.mutex);
}

/* Windows cannot read DTR/RTS back, so the levels set here are remembered in
 * os.dtr / os.rts and reported by cserial_os_get_control_lines() */
int cserial_os_set_control_lines(c_serial_port_type* port, const c_serial_control_lines_type* lines) {
	if (!EscapeCommFunction(port->port, lines->dtr ? SETDTR : CLRDTR)) {
		port->last_errnum = GetLastError();
		CSERIALDBG("Unable to set DTR line state");
		return CSERIAL_ERROR_GENERIC;
	}
	port->os.dtr = lines->dtr ? 1 : 0;

	if (!EscapeCommFunction(port->port, lines->rts ? SETRTS : CLRRTS)) {
		port->last_errnum = GetLastError();
		CSERIALDBG("Unable to set RTS line state");
		return CSERIAL_ERROR_GENERIC;
	}
	port->os.rts = lines->rts ? 1 : 0;

	return CSERIAL_OK;
}

int cserial_os_get_control_lines(c_serial_port_type* port, c_serial_control_lines_type* lines) {
	DWORD modem_lines;

	if (GetCommModemStatus(port->port, &modem_lines) == 0) {
		port->last_errnum = GetLastError();
		CSERIALDBG("Unable to get serial line state");
		return CSERIAL_ERROR_GENERIC;
	}

	/* RLSD (receive line signal detect) is the Windows name of CD */
	lines->cd = (modem_lines & MS_RLSD_ON) ? 1 : 0;
	lines->cts = (modem_lines & MS_CTS_ON) ? 1 : 0;
	lines->dsr = (modem_lines & MS_DSR_ON) ? 1 : 0;
	lines->dtr = port->os.dtr;
	lines->rts = port->os.rts;
	lines->ri = (modem_lines & MS_RING_ON) ? 1 : 0;

	return CSERIAL_OK;
}

int cserial_os_get_available(c_serial_port_type* port, int* available) {
	DWORD com_errors = 0;
	COMSTAT port_status;

	memset(&port_status, 0, sizeof(port_status));
	if (!ClearCommError(port->port, &com_errors, &port_status)) {
		port->last_errnum = GetLastError();
		CSERIALDBG("Unable to retrieve bytes available");
		return CSERIAL_ERROR_GENERIC;
	}

	*available = (int)port_status.cbInQue;
	return CSERIAL_OK;
}

/* Blocks until the driver has sent everything that was written */
int cserial_os_flush(c_serial_port_type* port) {
	if (FlushFileBuffers(port->port) == 0) {
		port->last_errnum = GetLastError();
		return CSERIAL_ERROR_GENERIC;
	}
	return CSERIAL_OK;
}

c_serial_handle_t cserial_os_poll_handle(c_serial_port_type* port) {
	return port->os.read_overlap.hEvent;
}

int cserial_os_list_ports(char** names, int max) {
	int count = 0;
	int x;
	char name[16];
	char target[1024];

	/* QueryDosDevice does not open the port, so it neither disturbs
	 * the device (DTR toggling) nor misses ports opened elsewhere. */
	for (x = 1; x <= CSERIAL_MAX_PORTS && count < max; x++) {
		snprintf(name, sizeof(name), "COM%d", x);
		/* QueryDosDeviceA() succeeds for every existing "COMn" and returns
		 * its kernel target (e.g. \Device\Serial0); a too-small buffer
		 * still proves the name exists */
		if (QueryDosDeviceA(name, target, sizeof(target)) == 0 &&
			GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			continue;
		}

		names[count] = cserial_internal_strdup(name);
		if (names[count] == NULL)
			return -1;
		count++;
	}

	return count;
}
