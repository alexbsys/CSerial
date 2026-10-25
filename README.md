# CSerial

Cross-platform serial port implementation.

This is a fork of [rm5248/CSerial](https://github.com/rm5248/CSerial)
([homepage](http://programming.rm5248.com/projects/c-serial/)) with security
and reliability fixes, macOS/iOS/Android support, tests and CI. See
[CHANGELOG.md](CHANGELOG.md) for the differences.

Based largely off of [JavaSerial](https://github.com/rm5248/JavaSerial),
CSerial aims to bring cross-platform serial programming to a computer near you.

The library is structured along the lines of the Apache Portable Runtime(APR).
It aims to provide as much low-level support as is required to create a working
serial port implementation.

CSerial is designed to be fully POSIX-compliant, however if it is not please
open an issue on GitHub.  See the 'Bugs' section for more details on this.

## Continuous Integration

GitHub Actions (`.github/workflows/ci.yml`) builds and tests the library on
Linux (GCC, Clang with ASan/UBSan and TSan), macOS and Windows (MSVC), and
cross-compiles it for Android (arm64-v8a, armeabi-v7a, x86_64) and iOS.

## Platform notes

| Platform | Status |
|----------|--------|
| Linux    | Full support, arbitrary baud rates through `termios2`/`BOTHER` on x86, ARM, RISC-V, LoongArch and s390. Other architectures only accept the standard `Bxxx` rates. RS485 through `TIOCSRS485`. |
| Windows  | Full support. Port names are `COMx` (up to 6 characters). Opening anything that is not a COM port (e.g. `C:` or `NUL`) fails with `CSERIAL_ERROR_NOT_A_SERIAL_PORT`. |
| macOS    | Full support. Use the `/dev/cu.*` devices for outgoing connections; `/dev/tty.*` devices are opened without waiting for DCD. Non-standard baud rates (above 230400) are set with `IOSSIOSPEED`. |
| Android  | Builds with the NDK (API 21+). Regular apps cannot open `/dev/tty*` because of SELinux: you need a rooted device, a system app, or the USB Host API (e.g. a Java USB-serial driver) instead. |
| iOS      | Builds, but the app sandbox does not give access to `/dev/*`; the port list is empty and opening a port fails. Use the ExternalAccessory framework for MFi accessories. |

Source layout:

| File | Contents |
|------|----------|
| `src/c_serial.c` | Platform-independent API: argument checks, cached settings, open sequence, read/cancel protocol, software RTS |
| `src/c_serial_internal.h` | Port structure and the `cserial_os_*` interface each backend implements |
| `src/c_serial_win.c` | Windows backend |
| `src/c_serial_posix.c`, `src/c_serial_posix.h` | Common POSIX backend (termios, ioctl, port enumeration) and the hooks below |
| `src/c_serial_linux.c` | Linux/Android: `termios2` baud rates, RS485 |
| `src/c_serial_darwin.c` | macOS/iOS: `select()` instead of `poll()`, `IOSSIOSPEED`, `pthread_mutex_timedlock` replacement |
| `src/c_serial_unix.c` | Other POSIX systems (BSD etc.) |

## Thread safety

A `c_serial_port_type` is not thread-safe in general: configure it from one
thread at a time. The supported cross-thread operations are:

* one thread reads (`c_serial_read_data`/`c_serial_read_data_timeout`) while
  another thread writes (`c_serial_write_data`/`c_serial_write_data_timeout`);
* one thread blocks in a read while another thread calls
  `c_serial_read_cancel` or `c_serial_close`;
* `c_serial_close` cancels a running read, waits for it to leave the library
  and then closes the handle. Reads started while the port is closing, or
  after it, fail with `CSERIAL_ERROR_NO_SUCH_SERIAL_PORT`, so a reader loop
  can simply exit on that code.

Only one read and one write may be in progress per port. Do not close a port
while another thread is writing to it, and do not `c_serial_free` it while
any other thread may still use it. When reads and writes fail concurrently,
`c_serial_get_last_native_errnum` reports one of the two errors.

## Tests

The tests use GoogleTest, which is downloaded by CMake (`FetchContent`). They
are enabled by default when CSerial is the top-level project and not
cross-compiled (`-DCSERIAL_BUILD_TESTS=OFF` disables them).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure
```

* On Linux and macOS the integration tests use pseudo-terminals, so no
  hardware is needed.
* On Windows, hardware tests are skipped unless you point them at real ports:
  `CSERIAL_TEST_PORT=COM1` runs single-port tests (settings, timeouts,
  cancel); `CSERIAL_TEST_PORT_A=COM10` and `CSERIAL_TEST_PORT_B=COM11` run
  loopback tests over a connected pair (e.g. com0com).
* Sanitizers: `-DCSERIAL_ENABLE_ASAN=ON`, `-DCSERIAL_ENABLE_UBSAN=ON`,
  `-DCSERIAL_ENABLE_TSAN=ON`.

## Build and install

CMake 3.14 or newer is required.  The same commands work on Linux, macOS and
Windows (Visual Studio; MinGW and Cygwin are not tested):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCSERIAL_BUILD_TESTS=OFF
cmake --build build --config Release
cmake --install build --config Release --prefix /usr/local    # e.g. C:/libs/cserial on Windows
```

This builds both a shared (`cserial`) and a static (`cserial_static`)
library and installs the header, the libraries, a CMake package and, on
POSIX systems, a pkg-config file.

Cross-compiling for Android and iOS uses the usual toolchains:

```bash
cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
```

The `debian/` directory builds `cserial0`/`cserial-dev` packages with
`dpkg-buildpackage -us -uc -b`.  The APT repository of the original project
([apt.rm5248.com](http://apt.rm5248.com/)) ships upstream CSerial, without
the changes of this fork.

## Using CSerial in your project

An installed library is found with `find_package`:

```cmake
find_package(cserial 0.4 REQUIRED)
target_link_libraries(my_app PRIVATE cserial::cserial)   # or cserial::cserial_static
```

or, without CMake, with pkg-config: `pkg-config --cflags --libs cserial`
(links the shared library; the static one is `libcserial_static.a` and also
needs `-pthread`).

The sources can also be built as part of your project:

```cmake
include(FetchContent)
FetchContent_Declare(cserial GIT_REPOSITORY https://github.com/alexbsys/CSerial.git GIT_TAG master)
FetchContent_MakeAvailable(cserial)     # or: add_subdirectory(third_party/CSerial)

target_link_libraries(my_app PRIVATE cserial::cserial_static)
```

Tests and examples are not built when CSerial is a subproject.  The targets
carry the include directory, the pthread dependency and, for the static
library on Windows, the `CSERIAL_STATIC` define.  When linking the static
library without CMake on Windows, define `CSERIAL_STATIC` yourself before
including `c_serial.h`.

## Quick start

Open a port, send a command and wait up to one second for the answer:

```c
#include <cserial/c_serial.h>
#include <stdio.h>

int main(void) {
    c_serial_port_type* port;
    char reply[64];
    int len;
    int rc;

    if (c_serial_new(&port, NULL) != CSERIAL_OK)
        return 1;

    c_serial_set_port_name(port, "/dev/ttyUSB0");   /* "COM3" on Windows */
    c_serial_set_baud_rate(port, 115200);           /* defaults: 9600 8N1, no flow control */

    rc = c_serial_open(port);
    if (rc != CSERIAL_OK) {
        fprintf(stderr, "open: %s, OS error %d\n", c_serial_get_error_string(rc),
                (int)c_serial_get_last_native_errnum(port));
        c_serial_free(port);
        return 1;
    }

    len = 4;
    rc = c_serial_write_data(port, "AT\r\n", &len);

    len = sizeof(reply);
    rc = c_serial_read_data_timeout(port, reply, &len, NULL, 1000);
    if (rc == CSERIAL_OK)
        printf("got %d bytes\n", len);
    else if (rc == CSERIAL_ERROR_TIMEOUT)
        printf("no answer\n");

    c_serial_free(port);   /* closes the port as well */
    return 0;
}
```

A read returns as soon as *some* bytes are there, so a reply can arrive in
pieces. Collect it until a terminator or an overall deadline:

```c
int read_until(c_serial_port_type* port, char* buf, int size, char end, int timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;   /* your monotonic clock */
    int used = 0;

    while (used < size) {
        int left = (int)(deadline > now_ms() ? deadline - now_ms() : 0);
        int len = size - used;
        int rc = c_serial_read_data_timeout(port, buf + used, &len, NULL, left);
        if (rc != CSERIAL_OK)
            return rc;                            /* CSERIAL_ERROR_TIMEOUT, ... */
        if (len == 0)
            return CSERIAL_ERROR_NO_SUCH_SERIAL_PORT;   /* POSIX: other side hung up */
        used += len;
        if (buf[used - 1] == end)
            return used;
    }
    return used;
}
```

Writes can be partial too. `c_serial_write_data_timeout` returns
`CSERIAL_ERROR_TIMEOUT` with `*length` set to the bytes already written, so
continue from there.

Read in a background thread and stop it from another thread:

```c
void* reader(void* arg) {
    c_serial_port_type* port = arg;
    char buf[256];
    int len, rc;

    do {
        len = sizeof(buf);
        rc = c_serial_read_data(port, buf, &len, NULL);    /* blocks */
        if (rc == CSERIAL_OK)
            handle_data(buf, len);
    } while (rc == CSERIAL_OK || rc == CSERIAL_ERROR_CANCELLED);
    return NULL;        /* CSERIAL_ERROR_NO_SUCH_SERIAL_PORT after c_serial_close */
}

/* main thread */
c_serial_read_cancel(port, 1000);   /* interrupts the current read only */
c_serial_close(port);               /* stops the reader for good */
pthread_join(thread, NULL);
c_serial_free(port);
```

List the ports (an empty list is normal, e.g. inside a sandboxed mobile app):

```c
const char** ports = c_serial_get_serial_ports_list();
for (int i = 0; ports != NULL && ports[i] != NULL; i++)
    printf("%s\n", ports[i]);
c_serial_free_serial_ports_list(ports);
```

Wait for a modem line change instead of data:

```c
c_serial_control_lines_type lines;
c_serial_set_serial_line_change_flags(port, CSERIAL_LINE_FLAG_CTS | CSERIAL_LINE_FLAG_DSR);
rc = c_serial_read_data_timeout(port, NULL, NULL, &lines, 5000);
if (rc == CSERIAL_OK && !lines.unsupported)
    printf("CTS=%d DSR=%d\n", lines.cts, lines.dsr);
```

Drive an RS485 transceiver (RTS high while sending):

```c
c_serial_set_flow_control(port, CSERIAL_FLOW_NONE);         /* hardware flow uses RTS too */
c_serial_set_rts_control(port, CSERIAL_RTS_BEST_AVAILABLE); /* kernel RS485 mode, else software */
```

## Examples

`-DCSERIAL_BUILD_EXAMPLES=ON` builds the programs in [`examples/`](examples).
Each one starts with a comment listing the corner cases it deals with.

| Program | Shows |
|---------|-------|
| [`list_ports`](examples/list_ports.c) | Enumeration, empty lists, optional probing (and why opening has side effects) |
| [`request_response`](examples/request_response.c) | Command/reply with one deadline, partial reads and writes, draining stale input, end of file |
| [`cancel_and_close`](examples/cancel_and_close.c) | Interrupting a blocked reader thread, shutting it down with `c_serial_close` |
| [`reconnect`](examples/reconnect.c) | USB adapter unplugged and plugged back, retry with backoff, exclusive access on POSIX |
| [`control_lines`](examples/control_lines.c) | DTR reset pulse (Arduino/ESP32), waiting for CTS/DSR/CD/RI, ports without modem lines |
| [`settings`](examples/settings.c) | Cached settings, invalid values, non-standard baud rates, 7E1, RS485 conflicts, keeping driver settings |
| [`loopback_stress`](examples/loopback_stress.c) | Bulk transfer with verification, reader and writer in different threads, stall detection |
| [`example_full`](examples/example_full.c), [`simpleterm`](examples/serial_simple_term.cpp) | Echo program and a minimal terminal |

The examples can also be built on their own against an installed library:
`cmake -S examples -B build-examples`.

## API overview

1. Allocate a new `c_serial_port_type` object with `c_serial_new`.  It is an
opaque type and can only be used through a pointer.
2. Set the name of the serial port with `c_serial_set_port_name`, e.g.
`/dev/ttyUSB0` on Linux, `/dev/cu.usbserial-1410` on macOS, `COM3` on Windows.
3. Set the serial port parameters.  By default, the port will be opened at 
`9600-8-N-1`.  Any other settings can be set with the appropriate setters.
  * To set the baud rate, use `c_serial_set_baud_rate`
  * To set the number of data bits, use `c_serial_set_data_bits`
  * To set the number of stop bits, use `c_serial_set_stop_bits`
  * To set the parity, use `c_serial_set_parity`
  * To set the flow control, use `c_serial_set_flow_control`
4. (optional) Set the serial line change flags.  This determines what changes 
on a serial line will cause a read to return.  By default, no flags are set.
Use the `c_serial_set_serial_line_change_flags` function to set the flags.
5. Open the port.  Use `c_serial_open`
6. Read and write data.  Use `c_serial_read_data` and `c_serial_write_data`, 
respectively, or their `_timeout` variants.
7. Close the port with `c_serial_close` (it can be reopened) and release it
with `c_serial_free`.

The header [`c_serial.h`](include/cserial/c_serial.h) documents every
function; `doxygen cserial-doxy` turns it into HTML in `documentation/`.

## Debug output

Build with `-DCMAKE_C_FLAGS=-DCSERIAL_DEBUG_PRINT=1` to have the library print
the reason of failed system calls to stderr. For error handling in your code,
use `c_serial_get_error_string` and `c_serial_get_last_native_errnum`.

## Other Important Functions

The following presents a brief list of other functions which may be 
important:

* `c_serial_get_serial_ports_list` - Go through the system and find all of 
the valid serial ports.  On POSIX systems only `/dev/tty*` (except the Linux
virtual consoles `/dev/ttyN`), `/dev/cu.*` and `/dev/rfcomm*` character
devices are probed; on Windows the `COMx` DOS
device names are queried without opening the ports.
* `c_serial_get_last_native_errnum` - Return the last native error number.  
This corresponds to `errno` on POSIX and `GetLastError` on Windows.
* `c_serial_[set|get]_user_data` - Set / get user data(`void*`) associated 
with the port.
* `c_serial_get_available` - Get the number of bytes to read.
* `c_serial_get_native_handle` - Get the native handle(int on POSIX, HANDLE on 
Windows) for raw I/O access
* `c_serial_get_poll_handle` - Get the handle used to poll for changes on a 
port.  Useful for use with a `poll()`-like function.  On POSIX systems, this
returns the same as `c_serial_get_native_handle`.  On Windows, this returns
a `HANDLE` corresponding to an event created with `CreateEvent` on the 
`OVERLAPPED` struct associated with the serial port.

### RTS Handling

The library also supports toggling of the RTS line when transmitting data 
over the serial port.  This is generally used for RS-485 transmitting.  This 
can be set to several different settings(see the enum CSerial_RTS_Handling 
for more details).  If set to HARDWARE, this will attempt to use the built-in 
kernel level functions for controlling the RTS line(using `struct serial_rs485` 
on Linux, or setting `fRtsControl` on Windows).  Setting it to SOFTWARE will 
cause the library to manually toggle the RTS line when sending data.

## Bugs

Questions?  Bugs?  Open an [issue](https://github.com/alexbsys/CSerial/issues)
for this fork.  Problems that also exist in the original library can be
reported [upstream](https://github.com/rm5248/CSerial/issues).

## License
Licensed under Apache 2.0
