#ifndef _WIN32

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cserial/c_serial.h>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

class PtyPair {
public:
    bool Open() {
        master_ = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0) return false;
        if (grantpt(master_) != 0 || unlockpt(master_) != 0) return false;
        const char* name = ptsname(master_);
        if (name == nullptr) return false;
        slave_name_ = name;
        return true;
    }
    ~PtyPair() {
        if (master_ >= 0) close(master_);
    }

    int master() const { return master_; }
    const std::string& slave_name() const { return slave_name_; }

    bool Write(const std::string& data) const {
        return write(master_, data.data(), data.size()) == (ssize_t)data.size();
    }

    std::string Read(size_t expected, int timeout_ms) const {
        std::string out;
        auto deadline = Clock::now() + milliseconds(timeout_ms);
        while (out.size() < expected && Clock::now() < deadline) {
            struct pollfd pfd = {master_, POLLIN, 0};
            if (poll(&pfd, 1, 50) <= 0) continue;
            char buf[256];
            ssize_t n = read(master_, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, (size_t)n);
        }
        return out;
    }

private:
    int master_ = -1;
    std::string slave_name_;
};

class CSerialPty : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pty_.Open()) << strerror(errno);
        ASSERT_EQ(CSERIAL_OK, c_serial_new(&port_, nullptr));
        ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port_, pty_.slave_name().c_str()));
        ASSERT_EQ(CSERIAL_OK, c_serial_open(port_));
    }
    void TearDown() override { c_serial_free(port_); }

    std::string ReadFromPort(size_t expected, int timeout_ms) {
        std::string out;
        auto deadline = Clock::now() + milliseconds(timeout_ms);
        while (out.size() < expected && Clock::now() < deadline) {
            char buf[64];
            int len = sizeof(buf);
            int rc = c_serial_read_data_timeout(port_, buf, &len, nullptr, 100);
            if (rc == CSERIAL_ERROR_TIMEOUT) continue;
            EXPECT_EQ(CSERIAL_OK, rc);
            if (rc != CSERIAL_OK) break;
            out.append(buf, (size_t)len);
        }
        return out;
    }

    PtyPair pty_;
    c_serial_port_type* port_ = nullptr;
};

}  // namespace

TEST_F(CSerialPty, OpenStateAndDoubleOpen) {
    EXPECT_EQ(1, c_serial_is_open(port_));
    EXPECT_EQ(CSERIAL_ERROR_ALREADY_OPEN, c_serial_open(port_));

    c_serial_close(port_);
    EXPECT_EQ(0, c_serial_is_open(port_));
    EXPECT_EQ(CSERIAL_OK, c_serial_open(port_));
}

TEST_F(CSerialPty, HandleIsCloseOnExec) {
    c_serial_handle_t fd = -1;
    ASSERT_EQ(CSERIAL_OK, c_serial_get_native_handle(port_, &fd));
    ASSERT_GE(fd, 0);
    int flags = fcntl(fd, F_GETFD);
    ASSERT_GE(flags, 0);
    EXPECT_TRUE(flags & FD_CLOEXEC);

    c_serial_handle_t poll_fd = -1;
    ASSERT_EQ(CSERIAL_OK, c_serial_get_poll_handle(port_, &poll_fd));
    EXPECT_EQ(fd, poll_fd);
}

TEST_F(CSerialPty, WriteIsReceivedByPeer) {
    const std::string msg = "hello, serial";
    int len = (int)msg.size();
    ASSERT_EQ(CSERIAL_OK, c_serial_write_data(port_, msg.data(), &len));
    EXPECT_EQ((int)msg.size(), len);
    EXPECT_EQ(msg, pty_.Read(msg.size(), 2000));

    len = (int)msg.size();
    ASSERT_EQ(CSERIAL_OK, c_serial_write_data_timeout(port_, msg.data(), &len, 1000));
    EXPECT_EQ(msg, pty_.Read(msg.size(), 2000));
}

TEST_F(CSerialPty, RtsHandlingWithoutDriverRs485) {
    /* A pty has no driver RS485 mode */
    EXPECT_EQ(CSERIAL_RTS_TYPE_NOT_AVAILABLE, c_serial_set_rts_control(port_, CSERIAL_RTS_HARDWARE));
    EXPECT_EQ(CSERIAL_RTS_NONE, c_serial_get_rts_control(port_));

    ASSERT_EQ(CSERIAL_OK, c_serial_set_rts_control(port_, CSERIAL_RTS_BEST_AVAILABLE));
    c_serial_close(port_);
    ASSERT_EQ(CSERIAL_OK, c_serial_open(port_));

    const std::string msg = "rs485";
    int len = (int)msg.size();
    ASSERT_EQ(CSERIAL_OK, c_serial_write_data(port_, msg.data(), &len));
    EXPECT_EQ(msg, pty_.Read(msg.size(), 2000));

    EXPECT_EQ(CSERIAL_OK, c_serial_set_rts_control(port_, CSERIAL_RTS_SOFTWARE));
    EXPECT_EQ(CSERIAL_OK, c_serial_set_rts_control(port_, CSERIAL_RTS_NONE));
}

TEST_F(CSerialPty, ZeroLengthWriteIsNoop) {
    int len = 0;
    EXPECT_EQ(CSERIAL_OK, c_serial_write_data(port_, "", &len));
    EXPECT_EQ(0, len);
}

TEST_F(CSerialPty, ReadReceivesPeerData) {
    static const char kRaw[] = "\x01\x02 binary \xff\x00 data\r\n";
    const std::string payload(kRaw, sizeof(kRaw) - 1);
    ASSERT_TRUE(pty_.Write(payload));
    EXPECT_EQ(payload, ReadFromPort(payload.size(), 2000));
}

TEST_F(CSerialPty, ReadDoesNotOverrunBuffer) {
    ASSERT_TRUE(pty_.Write(std::string(64, 'z')));
    std::this_thread::sleep_for(milliseconds(50));
    char buf[16];
    std::memset(buf, 0, sizeof(buf));
    int len = 4;
    ASSERT_EQ(CSERIAL_OK, c_serial_read_data_timeout(port_, buf, &len, nullptr, 1000));
    EXPECT_GE(len, 1);
    EXPECT_LE(len, 4);
    for (size_t i = 4; i < sizeof(buf); ++i) EXPECT_EQ(0, buf[i]);
}

TEST_F(CSerialPty, AvailableReportsPendingBytes) {
    ASSERT_TRUE(pty_.Write("abc"));
    int available = 0;
    auto deadline = Clock::now() + milliseconds(1000);
    while (available < 3 && Clock::now() < deadline) {
        ASSERT_EQ(CSERIAL_OK, c_serial_get_available(port_, &available));
        std::this_thread::sleep_for(milliseconds(10));
    }
    EXPECT_EQ(3, available);
}

TEST_F(CSerialPty, ReadTimesOut) {
    char buf[8];
    int len = sizeof(buf);
    auto start = Clock::now();
    EXPECT_EQ(CSERIAL_ERROR_TIMEOUT, c_serial_read_data_timeout(port_, buf, &len, nullptr, 200));
    auto elapsed = Clock::now() - start;
    EXPECT_EQ(0, len);
    EXPECT_GE(elapsed, milliseconds(150));
    EXPECT_LT(elapsed, milliseconds(2000));
}

TEST_F(CSerialPty, ZeroTimeoutReturnsBufferedData) {
    ASSERT_TRUE(pty_.Write("x"));
    std::this_thread::sleep_for(milliseconds(50));
    char buf[8];
    int len = sizeof(buf);
    ASSERT_EQ(CSERIAL_OK, c_serial_read_data_timeout(port_, buf, &len, nullptr, 0));
    ASSERT_EQ(1, len);
    EXPECT_EQ('x', buf[0]);

    len = sizeof(buf);
    EXPECT_EQ(CSERIAL_ERROR_TIMEOUT, c_serial_read_data_timeout(port_, buf, &len, nullptr, 0));
}

TEST_F(CSerialPty, CancelUnblocksReaderInAnotherThread) {
    std::atomic<int> result{1};
    std::thread reader([&] {
        char buf[8];
        int len = sizeof(buf);
        result = c_serial_read_data(port_, buf, &len, nullptr);
    });
    std::this_thread::sleep_for(milliseconds(150));
    auto start = Clock::now();
    EXPECT_EQ(CSERIAL_OK, c_serial_read_cancel(port_, 2000));
    reader.join();
    EXPECT_LT(Clock::now() - start, milliseconds(1500));
    EXPECT_EQ(CSERIAL_ERROR_CANCELLED, result.load());

    /* the port is still usable after a cancel */
    ASSERT_TRUE(pty_.Write("ok"));
    EXPECT_EQ("ok", ReadFromPort(2, 2000));
}

TEST_F(CSerialPty, TimedReadCanBeCancelled) {
    std::atomic<int> result{1};
    std::thread reader([&] {
        char buf[8];
        int len = sizeof(buf);
        result = c_serial_read_data_timeout(port_, buf, &len, nullptr, 10000);
    });
    std::this_thread::sleep_for(milliseconds(150));
    auto start = Clock::now();
    EXPECT_EQ(CSERIAL_OK, c_serial_read_cancel(port_, 2000));
    reader.join();
    EXPECT_LT(Clock::now() - start, milliseconds(1500));
    EXPECT_EQ(CSERIAL_ERROR_CANCELLED, result.load());
}

TEST_F(CSerialPty, CloseDuringBlockingRead) {
    std::atomic<int> result{1};
    std::thread reader([&] {
        char buf[8];
        int len = sizeof(buf);
        result = c_serial_read_data(port_, buf, &len, nullptr);
    });
    std::this_thread::sleep_for(milliseconds(150));
    c_serial_close(port_);
    reader.join();
    EXPECT_TRUE(result == CSERIAL_ERROR_CANCELLED || result == CSERIAL_ERROR_NO_SUCH_SERIAL_PORT)
        << result.load();
    EXPECT_EQ(0, c_serial_is_open(port_));
}

TEST_F(CSerialPty, CloseStopsReaderThatRetries) {
    std::atomic<int> result{1};
    std::thread reader([&] {
        int rc;
        do {
            char buf[8];
            int len = sizeof(buf);
            rc = c_serial_read_data(port_, buf, &len, nullptr);
        } while (rc == CSERIAL_OK || rc == CSERIAL_ERROR_CANCELLED);
        result = rc;
    });
    std::this_thread::sleep_for(milliseconds(150));
    auto start = Clock::now();
    c_serial_close(port_);
    reader.join();
    EXPECT_LT(Clock::now() - start, milliseconds(1500));
    EXPECT_EQ(CSERIAL_ERROR_NO_SUCH_SERIAL_PORT, result.load());
}

TEST_F(CSerialPty, ReadAndWriteFromDifferentThreads) {
    const size_t kTotal = 32 * 1024;
    auto pattern = [](size_t i) { return (char)((i * 7 + (i >> 8)) & 0xFF); };

    std::string received;
    std::thread reader([&] {
        while (received.size() < kTotal) {
            char buf[256];
            int len = sizeof(buf);
            int rc = c_serial_read_data_timeout(port_, buf, &len, nullptr, 3000);
            if (rc != CSERIAL_OK || len == 0) break;
            received.append(buf, (size_t)len);
        }
    });

    size_t written = 0;
    std::thread writer([&] {
        while (written < kTotal) {
            char buf[512];
            size_t n = std::min(sizeof(buf), kTotal - written);
            for (size_t i = 0; i < n; ++i) buf[i] = pattern(written + i);
            int len = (int)n;
            if (c_serial_write_data_timeout(port_, buf, &len, 3000) != CSERIAL_OK) break;
            written += (size_t)len;
        }
    });

    /* Pump the other end of the pty in both directions */
    size_t to_port = 0;
    std::string from_port;
    auto deadline = Clock::now() + milliseconds(20000);
    while ((to_port < kTotal || from_port.size() < kTotal) && Clock::now() < deadline) {
        struct pollfd pfd = {pty_.master(), (short)(POLLIN | (to_port < kTotal ? POLLOUT : 0)), 0};
        if (poll(&pfd, 1, 50) <= 0) continue;
        if (pfd.revents & POLLIN) {
            char buf[1024];
            ssize_t n = read(pty_.master(), buf, sizeof(buf));
            if (n > 0) from_port.append(buf, (size_t)n);
        }
        if ((pfd.revents & POLLOUT) && to_port < kTotal) {
            char buf[512];
            size_t n = std::min(sizeof(buf), kTotal - to_port);
            for (size_t i = 0; i < n; ++i) buf[i] = pattern(to_port + i);
            ssize_t w = write(pty_.master(), buf, n);
            if (w > 0) to_port += (size_t)w;
        }
    }

    writer.join();
    reader.join();
    ASSERT_EQ(kTotal, received.size());
    ASSERT_EQ(kTotal, from_port.size());
    for (size_t i = 0; i < kTotal; ++i) {
        ASSERT_EQ(pattern(i), received[i]) << "port read, byte " << i;
        ASSERT_EQ(pattern(i), from_port[i]) << "port write, byte " << i;
    }
}

TEST_F(CSerialPty, LinesOnlyReadWithoutFlagsReturnsImmediately) {
    c_serial_control_lines_type lines;
    auto start = Clock::now();
    EXPECT_EQ(CSERIAL_OK, c_serial_read_data(port_, nullptr, nullptr, &lines));
    EXPECT_LT(Clock::now() - start, milliseconds(500));
}

#ifdef __linux__
TEST_F(CSerialPty, ControlLinesAreReportedUnsupportedOnPty) {
    ASSERT_TRUE(pty_.Write("q"));
    char buf[4];
    int len = sizeof(buf);
    c_serial_control_lines_type lines;
    std::memset(&lines, 0xAA, sizeof(lines));
    ASSERT_EQ(CSERIAL_OK, c_serial_read_data_timeout(port_, buf, &len, &lines, 1000));
    EXPECT_EQ(1, len);
    EXPECT_EQ(1, lines.unsupported);
    EXPECT_EQ(0, lines.cts);
}
#endif

/* The Linux pty driver forces CS8 and clears PARENB (and rejects other
 * character sizes), so parity/data bits can only be verified on ptys elsewhere. */
#ifdef __linux__
#define CSERIAL_PTY_KEEPS_CHAR_FORMAT 0
#else
#define CSERIAL_PTY_KEEPS_CHAR_FORMAT 1
#endif

TEST_F(CSerialPty, SettingsRoundTrip) {
#if CSERIAL_PTY_KEEPS_CHAR_FORMAT
    for (auto parity : {CSERIAL_PARITY_ODD, CSERIAL_PARITY_EVEN, CSERIAL_PARITY_NONE}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_parity(port_, parity));
        EXPECT_EQ(parity, c_serial_get_parity(port_));
    }
    for (auto bits : {CSERIAL_BITS_5, CSERIAL_BITS_6, CSERIAL_BITS_7, CSERIAL_BITS_8}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_data_bits(port_, bits));
        EXPECT_EQ(bits, c_serial_get_data_bits(port_));
    }
#else
    EXPECT_EQ(CSERIAL_BITS_8, c_serial_get_data_bits(port_));
    EXPECT_EQ(CSERIAL_PARITY_NONE, c_serial_get_parity(port_));
#endif
    for (auto stop : {CSERIAL_STOP_BITS_2, CSERIAL_STOP_BITS_1}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_stop_bits(port_, stop));
        EXPECT_EQ(stop, c_serial_get_stop_bits(port_));
    }
    for (auto flow : {CSERIAL_FLOW_SOFTWARE, CSERIAL_FLOW_HARDWARE, CSERIAL_FLOW_NONE}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_flow_control(port_, flow));
        EXPECT_EQ(flow, c_serial_get_flow_control(port_));
    }
    for (int baud : {1200, 9600, 19200, 38400, 57600, 115200, 230400}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_baud_rate(port_, baud)) << baud;
        EXPECT_EQ(baud, c_serial_get_baud_rate(port_));
    }
}

TEST(CSerialPtySettings, SettingsAppliedOnOpen) {
    PtyPair pty;
    ASSERT_TRUE(pty.Open());
    c_serial_port_type* port = nullptr;
    ASSERT_EQ(CSERIAL_OK, c_serial_new(&port, nullptr));
    ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port, pty.slave_name().c_str()));
    ASSERT_EQ(CSERIAL_OK, c_serial_set_baud_rate(port, 19200));
    ASSERT_EQ(CSERIAL_OK, c_serial_set_stop_bits(port, CSERIAL_STOP_BITS_2));
    ASSERT_EQ(CSERIAL_OK, c_serial_set_flow_control(port, CSERIAL_FLOW_SOFTWARE));
#if CSERIAL_PTY_KEEPS_CHAR_FORMAT
    ASSERT_EQ(CSERIAL_OK, c_serial_set_data_bits(port, CSERIAL_BITS_7));
    ASSERT_EQ(CSERIAL_OK, c_serial_set_parity(port, CSERIAL_PARITY_ODD));
#endif
    ASSERT_EQ(CSERIAL_OK, c_serial_open(port));

    EXPECT_EQ(19200, c_serial_get_baud_rate(port));
    EXPECT_EQ(CSERIAL_STOP_BITS_2, c_serial_get_stop_bits(port));
    EXPECT_EQ(CSERIAL_FLOW_SOFTWARE, c_serial_get_flow_control(port));
#if CSERIAL_PTY_KEEPS_CHAR_FORMAT
    EXPECT_EQ(CSERIAL_BITS_7, c_serial_get_data_bits(port));
    EXPECT_EQ(CSERIAL_PARITY_ODD, c_serial_get_parity(port));
#endif
    c_serial_free(port);
}

#ifdef __linux__
TEST_F(CSerialPty, CustomBaudRateRoundTrip) {
    ASSERT_EQ(CSERIAL_OK, c_serial_set_baud_rate(port_, 250000));
    EXPECT_EQ(250000, c_serial_get_baud_rate(port_));

    /* other settings must not drop the custom rate */
    ASSERT_EQ(CSERIAL_OK, c_serial_set_parity(port_, CSERIAL_PARITY_EVEN));
    EXPECT_EQ(250000, c_serial_get_baud_rate(port_));

    ASSERT_EQ(CSERIAL_OK, c_serial_set_baud_rate(port_, 9600));
    EXPECT_EQ(9600, c_serial_get_baud_rate(port_));
}
#endif

/* Regression: select()/FD_SET overflowed the stack for descriptors >= FD_SETSIZE. */
TEST(CSerialPtyHighFd, ReadWorksWithDescriptorAboveFdSetSize) {
    const int kTarget = 1100;
    struct rlimit lim;
    ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &lim));
    if (lim.rlim_cur < (rlim_t)kTarget + 64) {
        rlim_t wanted = (rlim_t)kTarget + 64;
        if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < wanted) {
            GTEST_SKIP() << "RLIMIT_NOFILE hard limit too low";
        }
        lim.rlim_cur = wanted;
        if (setrlimit(RLIMIT_NOFILE, &lim) != 0) GTEST_SKIP() << "cannot raise RLIMIT_NOFILE";
    }

    PtyPair pty;
    ASSERT_TRUE(pty.Open());

    std::vector<int> fillers;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        ASSERT_GE(fd, 0);
        fillers.push_back(fd);
        if (fd >= kTarget) break;
    }

    c_serial_port_type* port = nullptr;
    ASSERT_EQ(CSERIAL_OK, c_serial_new(&port, nullptr));
    ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port, pty.slave_name().c_str()));
    ASSERT_EQ(CSERIAL_OK, c_serial_open(port));

    c_serial_handle_t fd = -1;
    ASSERT_EQ(CSERIAL_OK, c_serial_get_native_handle(port, &fd));
    EXPECT_GE(fd, 1024);

    char buf[8];
    int len = sizeof(buf);
    EXPECT_EQ(CSERIAL_ERROR_TIMEOUT, c_serial_read_data_timeout(port, buf, &len, nullptr, 50));

    ASSERT_TRUE(pty.Write("hi"));
    std::string got;
    auto deadline = Clock::now() + milliseconds(2000);
    while (got.size() < 2 && Clock::now() < deadline) {
        len = sizeof(buf);
        int rc = c_serial_read_data_timeout(port, buf, &len, nullptr, 100);
        if (rc == CSERIAL_OK) got.append(buf, (size_t)len);
    }
    EXPECT_EQ("hi", got);

    c_serial_free(port);
    for (int f : fillers) close(f);
}

#endif /* _WIN32 */
