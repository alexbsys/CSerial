# Changelog

## 0.4.0 (unreleased)

Changes of this fork relative to upstream CSerial 0.3.

### Security and memory safety

- POSIX reads used `select()`, overflowing the `fd_set` on the stack for
  descriptors >= 1024; they now use `poll()` (`select()` with heap-allocated
  sets on Darwin, where `poll()` does not support devices).
- Port list: fixed an out-of-bounds write of the terminating NULL at 512
  ports, unchecked allocations and leaks; on Windows, too small name buffers
  and a missing terminator for `COM100` and above.
- The POSIX port list no longer opens every file in `/dev` (opening e.g.
  `/dev/watchdog` arms a hardware reset): only `tty*`, `cu.*` and `rfcomm*`
  character devices are probed, with `O_NOCTTY`.  Linux virtual consoles
  (`/dev/ttyN`) are skipped.
- Negative or zero lengths and NULL arguments are rejected with the new
  `CSERIAL_ERROR_INVALID_PARAM` instead of reaching the system calls.
- Windows: a write or wait that timed out returned while the kernel still
  referenced the caller's buffer or stack; the I/O is now cancelled and
  completed first.
- Windows: opening a name that is not a COM port (`C:`, `NUL`, a pipe) fails
  with `CSERIAL_ERROR_NOT_A_SERIAL_PORT` instead of opening the device.
- Descriptors are opened with `O_CLOEXEC` and no longer leak into child
  processes or on failed opens.

### Threads

- `c_serial_close()` closes the handle under the port mutex after cancelling
  the reader, so a reader can no longer use a closed (and possibly reused)
  descriptor.
- `c_serial_close()` could hang, or take seconds, when the reader thread
  immediately read again after `CSERIAL_ERROR_CANCELLED`.  Reads started
  while the port is closing now fail with `CSERIAL_ERROR_NO_SUCH_SERIAL_PORT`.
- Fixed unlocking a mutex that was not locked after a timed lock failed, a
  wrong nanosecond conversion in timeouts, `ReleaseMutex` without a matching
  wait, and the non-atomic fallback implementation.
- Reading and writing from two different threads is supported and tested.

### Functional fixes

- Odd parity was never enabled; the data bits and flow control getters read
  the wrong fields; Linux reported raw `Bxxx` constants for custom baud rates.
- Windows: with the default settings the driver toggled RTS on every write
  (`RTS_CONTROL_TOGGLE`) and `c_serial_set_control_line()` always failed.
- RS485: `CSERIAL_RTS_BEST_AVAILABLE` did not toggle RTS at all on macOS, iOS,
  BSD and Linux ports without driver RS485 support, it now falls back to
  software RTS; `CSERIAL_RTS_HARDWARE` returns `CSERIAL_RTS_TYPE_NOT_AVAILABLE`
  there.  `CSERIAL_RTS_SOFTWARE` turns the driver mode off, and a rejected
  `c_serial_set_rts_control()` keeps the previous mode.
- Timeouts use a monotonic clock instead of `times()`.
- Partial writes and `EINTR` are handled on POSIX.

### Platforms

- macOS: working timed lock, opening `/dev/tty.*` no longer waits for DCD,
  non-standard baud rates through `IOSSIOSPEED`.
- iOS and Android build (with documented sandbox limitations); the Android
  shared library is unversioned so that Gradle packages it.
- Linux `termios2` custom baud rates only on architectures with a known
  struct layout.

### Project

- Sources split into a platform-independent part and per-platform backends.
- GoogleTest unit and pseudo-terminal tests, Windows hardware tests,
  sanitizer builds and GitHub Actions CI for Linux, macOS, Windows, Android
  and iOS.
- New examples for corner cases (`examples/`), README quick start.
- Installs a CMake package (`find_package(cserial)` with `cserial::cserial`
  and `cserial::cserial_static`); the pkg-config file now has the right
  `libdir` on multiarch systems and a version.
