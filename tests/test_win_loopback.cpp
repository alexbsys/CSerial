/*
 * Windows hardware tests.
 *
 * Single-port tests (settings, timeouts, cancel) need any real COM port:
 *
 *   set CSERIAL_TEST_PORT=COM1
 *
 * Loopback tests need a pair of connected COM ports, e.g. a com0com
 * virtual null-modem pair:
 *
 *   set CSERIAL_TEST_PORT_A=COM10
 *   set CSERIAL_TEST_PORT_B=COM11
 *
 * Without these variables the corresponding tests are skipped.
 */
#ifdef _WIN32

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include <cserial/c_serial.h>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

std::string EnvOrEmpty(const char* name) {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return std::string();
    std::string result(value);
    free(value);
    return result;
}

class CSerialWinLoopback : public ::testing::Test {
protected:
    void SetUp() override {
        std::string a = EnvOrEmpty("CSERIAL_TEST_PORT_A");
        std::string b = EnvOrEmpty("CSERIAL_TEST_PORT_B");
        if (a.empty() || b.empty()) {
            GTEST_SKIP() << "CSERIAL_TEST_PORT_A/B are not set";
        }
        ASSERT_EQ(CSERIAL_OK, c_serial_new(&a_, nullptr));
        ASSERT_EQ(CSERIAL_OK, c_serial_new(&b_, nullptr));
        ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(a_, a.c_str()));
        ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(b_, b.c_str()));
        ASSERT_EQ(CSERIAL_OK, c_serial_set_baud_rate(a_, 115200));
        ASSERT_EQ(CSERIAL_OK, c_serial_set_baud_rate(b_, 115200));
        ASSERT_EQ(CSERIAL_OK, c_serial_open(a_));
        ASSERT_EQ(CSERIAL_OK, c_serial_open(b_));
    }
    void TearDown() override {
        c_serial_free(a_);
        c_serial_free(b_);
    }

    std::string Read(c_serial_port_type* port, size_t expected, int timeout_ms) {
        std::string out;
        auto deadline = Clock::now() + milliseconds(timeout_ms);
        while (out.size() < expected && Clock::now() < deadline) {
            char buf[64];
            int len = sizeof(buf);
            int rc = c_serial_read_data_timeout(port, buf, &len, nullptr, 100);
            if (rc == CSERIAL_ERROR_TIMEOUT) continue;
            EXPECT_EQ(CSERIAL_OK, rc);
            if (rc != CSERIAL_OK) break;
            out.append(buf, (size_t)len);
        }
        return out;
    }

    c_serial_port_type* a_ = nullptr;
    c_serial_port_type* b_ = nullptr;
};

class CSerialWinSinglePort : public ::testing::Test {
protected:
    void SetUp() override {
        std::string name = EnvOrEmpty("CSERIAL_TEST_PORT");
        if (name.empty()) {
            GTEST_SKIP() << "CSERIAL_TEST_PORT is not set";
        }
        ASSERT_EQ(CSERIAL_OK, c_serial_new(&port_, nullptr));
        ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port_, name.c_str()));
        ASSERT_EQ(CSERIAL_OK, c_serial_open(port_)) << "native error " << c_serial_get_last_native_errnum(port_);
    }
    void TearDown() override { c_serial_free(port_); }

    c_serial_port_type* port_ = nullptr;
};

}  // namespace

TEST_F(CSerialWinSinglePort, SettingsRoundTrip) {
    for (auto parity : {CSERIAL_PARITY_ODD, CSERIAL_PARITY_EVEN, CSERIAL_PARITY_NONE}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_parity(port_, parity));
        EXPECT_EQ(parity, c_serial_get_parity(port_));
    }
    for (auto bits : {CSERIAL_BITS_7, CSERIAL_BITS_8}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_data_bits(port_, bits));
        EXPECT_EQ(bits, c_serial_get_data_bits(port_));
    }
    for (auto stop : {CSERIAL_STOP_BITS_2, CSERIAL_STOP_BITS_1}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_stop_bits(port_, stop));
        EXPECT_EQ(stop, c_serial_get_stop_bits(port_));
    }
    for (auto flow : {CSERIAL_FLOW_SOFTWARE, CSERIAL_FLOW_HARDWARE, CSERIAL_FLOW_NONE}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_flow_control(port_, flow));
        EXPECT_EQ(flow, c_serial_get_flow_control(port_));
    }
    for (int baud : {9600, 19200, 57600, 115200}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_baud_rate(port_, baud));
        EXPECT_EQ(baud, c_serial_get_baud_rate(port_));
    }
}

TEST_F(CSerialWinSinglePort, ReadTimesOut) {
    char buf[8];
    int len = sizeof(buf);
    auto start = Clock::now();
    int rc = c_serial_read_data_timeout(port_, buf, &len, nullptr, 200);
    if (rc == CSERIAL_OK) GTEST_SKIP() << "port is receiving data";
    EXPECT_EQ(CSERIAL_ERROR_TIMEOUT, rc);
    EXPECT_EQ(0, len);
    EXPECT_GE(Clock::now() - start, milliseconds(150));
    EXPECT_LT(Clock::now() - start, milliseconds(2000));
}

TEST_F(CSerialWinSinglePort, CancelUnblocksReader) {
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
}

TEST_F(CSerialWinSinglePort, CloseDuringBlockingRead) {
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

TEST_F(CSerialWinSinglePort, CloseStopsReaderThatRetries) {
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

namespace {
DWORD RtsControlMode(c_serial_port_type* port) {
    c_serial_handle_t h;
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (c_serial_get_native_handle(port, &h) != CSERIAL_OK || !GetCommState(h, &dcb)) return MAXDWORD;
    return dcb.fRtsControl;
}
}  // namespace

TEST_F(CSerialWinSinglePort, RtsModeFollowsRtsControl) {
    /* Default: RTS is not driven by the driver, so it can be set by hand */
    EXPECT_EQ((DWORD)RTS_CONTROL_DISABLE, RtsControlMode(port_));

    ASSERT_EQ(CSERIAL_OK, c_serial_set_rts_control(port_, CSERIAL_RTS_HARDWARE));
    EXPECT_EQ((DWORD)RTS_CONTROL_TOGGLE, RtsControlMode(port_));

    ASSERT_EQ(CSERIAL_OK, c_serial_set_rts_control(port_, CSERIAL_RTS_NONE));
    EXPECT_NE((DWORD)RTS_CONTROL_TOGGLE, RtsControlMode(port_));

    ASSERT_EQ(CSERIAL_OK, c_serial_set_flow_control(port_, CSERIAL_FLOW_HARDWARE));
    EXPECT_EQ((DWORD)RTS_CONTROL_HANDSHAKE, RtsControlMode(port_));
}

TEST_F(CSerialWinSinglePort, ControlLinesCanBeSet) {
    for (int state : {1, 0, 1}) {
        c_serial_control_lines_type lines = {};
        lines.dtr = state;
        lines.rts = state;
        ASSERT_EQ(CSERIAL_OK, c_serial_set_control_line(port_, &lines, 1))
            << "native error " << c_serial_get_last_native_errnum(port_);
        EXPECT_EQ(state, lines.dtr);
        EXPECT_EQ(state, lines.rts);
    }
}

TEST_F(CSerialWinSinglePort, WriteCompletes) {
    const std::string msg = "cserial";
    int len = (int)msg.size();
    int rc = c_serial_write_data_timeout(port_, msg.data(), &len, 2000);
    EXPECT_TRUE(rc == CSERIAL_OK || rc == CSERIAL_ERROR_TIMEOUT) << rc;
    EXPECT_GE(len, 0);
    EXPECT_LE(len, (int)msg.size());
}

TEST(CSerialWinPortsList, ListsConfiguredPort) {
    std::string name = EnvOrEmpty("CSERIAL_TEST_PORT");
    if (name.empty()) GTEST_SKIP() << "CSERIAL_TEST_PORT is not set";
    const char** list = c_serial_get_serial_ports_list();
    ASSERT_NE(nullptr, list);
    bool found = false;
    for (int i = 0; list[i] != nullptr; ++i) {
        if (name == list[i]) found = true;
    }
    c_serial_free_serial_ports_list(list);
    EXPECT_TRUE(found) << name;
}

TEST_F(CSerialWinLoopback, DataFlowsBothWays) {
    const std::string msg = "ping from A";
    int len = (int)msg.size();
    ASSERT_EQ(CSERIAL_OK, c_serial_write_data(a_, msg.data(), &len));
    EXPECT_EQ(msg, Read(b_, msg.size(), 2000));

    const std::string reply = "pong from B";
    len = (int)reply.size();
    ASSERT_EQ(CSERIAL_OK, c_serial_write_data_timeout(b_, reply.data(), &len, 1000));
    EXPECT_EQ(reply, Read(a_, reply.size(), 2000));
}

TEST_F(CSerialWinLoopback, ReadTimesOut) {
    char buf[8];
    int len = sizeof(buf);
    auto start = Clock::now();
    EXPECT_EQ(CSERIAL_ERROR_TIMEOUT, c_serial_read_data_timeout(a_, buf, &len, nullptr, 200));
    EXPECT_EQ(0, len);
    EXPECT_GE(Clock::now() - start, milliseconds(150));
}

TEST_F(CSerialWinLoopback, CancelUnblocksReader) {
    std::atomic<int> result{1};
    std::thread reader([&] {
        char buf[8];
        int len = sizeof(buf);
        result = c_serial_read_data(a_, buf, &len, nullptr);
    });
    std::this_thread::sleep_for(milliseconds(150));
    EXPECT_EQ(CSERIAL_OK, c_serial_read_cancel(a_, 2000));
    reader.join();
    EXPECT_EQ(CSERIAL_ERROR_CANCELLED, result.load());

    const std::string msg = "after cancel";
    int len = (int)msg.size();
    ASSERT_EQ(CSERIAL_OK, c_serial_write_data(b_, msg.data(), &len));
    EXPECT_EQ(msg, Read(a_, msg.size(), 2000));
}

TEST_F(CSerialWinLoopback, CloseDuringBlockingRead) {
    std::atomic<int> result{1};
    std::thread reader([&] {
        char buf[8];
        int len = sizeof(buf);
        result = c_serial_read_data(a_, buf, &len, nullptr);
    });
    std::this_thread::sleep_for(milliseconds(150));
    c_serial_close(a_);
    reader.join();
    EXPECT_TRUE(result == CSERIAL_ERROR_CANCELLED || result == CSERIAL_ERROR_NO_SUCH_SERIAL_PORT)
        << result.load();
}

TEST_F(CSerialWinLoopback, SettingsRoundTrip) {
    for (auto parity : {CSERIAL_PARITY_ODD, CSERIAL_PARITY_EVEN, CSERIAL_PARITY_NONE}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_parity(a_, parity));
        EXPECT_EQ(parity, c_serial_get_parity(a_));
    }
    for (auto stop : {CSERIAL_STOP_BITS_2, CSERIAL_STOP_BITS_1}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_stop_bits(a_, stop));
        EXPECT_EQ(stop, c_serial_get_stop_bits(a_));
    }
    for (auto flow : {CSERIAL_FLOW_SOFTWARE, CSERIAL_FLOW_HARDWARE, CSERIAL_FLOW_NONE}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_flow_control(a_, flow));
        EXPECT_EQ(flow, c_serial_get_flow_control(a_));
    }
    for (int baud : {9600, 57600, 115200, 250000}) {
        ASSERT_EQ(CSERIAL_OK, c_serial_set_baud_rate(a_, baud));
        EXPECT_EQ(baud, c_serial_get_baud_rate(a_));
    }
}

#endif /* _WIN32 */
