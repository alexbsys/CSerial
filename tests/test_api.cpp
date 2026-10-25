#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>

#include <cserial/c_serial.h>

namespace {

#ifdef _WIN32
const char* kNotASerialPort = "NUL";
const char* kMissingPort = "COM250";
#else
const char* kNotASerialPort = "/dev/null";
const char* kMissingPort = "/dev/cserial-test-does-not-exist";
#endif

class CSerialApi : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(CSERIAL_OK, c_serial_new(&port_, nullptr));
        ASSERT_NE(nullptr, port_);
    }
    void TearDown() override { c_serial_free(port_); }

    c_serial_port_type* port_ = nullptr;
};

}  // namespace

TEST(CSerialNull, NewRejectsNullOutPointer) {
    EXPECT_EQ(CSERIAL_ERROR_CANT_CREATE, c_serial_new(nullptr, nullptr));
}

TEST(CSerialNull, AllFunctionsRejectNullPort) {
    int length = 1;
    char buf[1];
    c_serial_control_lines_type lines;
    c_serial_handle_t handle;
    int available;

    c_serial_free(nullptr);
    c_serial_close(nullptr);
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_open(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_open_keep_settings(nullptr, 1));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_is_open(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_port_name(nullptr, "x"));
    EXPECT_EQ(nullptr, c_serial_get_port_name(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_baud_rate(nullptr, 9600));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_get_baud_rate(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_data_bits(nullptr, CSERIAL_BITS_8));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, (int)c_serial_get_data_bits(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_stop_bits(nullptr, CSERIAL_STOP_BITS_1));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, (int)c_serial_get_stop_bits(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_parity(nullptr, CSERIAL_PARITY_NONE));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, (int)c_serial_get_parity(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_flow_control(nullptr, CSERIAL_FLOW_NONE));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, (int)c_serial_get_flow_control(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_write_data(nullptr, buf, &length));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_write_data_timeout(nullptr, buf, &length, 10));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_read_data(nullptr, buf, &length, &lines));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_read_data_timeout(nullptr, buf, &length, &lines, 10));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_read_cancel(nullptr, 0));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_get_native_handle(nullptr, &handle));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_get_poll_handle(nullptr, &handle));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_control_line(nullptr, &lines, 0));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_get_control_lines(nullptr, &lines));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_get_available(nullptr, &available));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_serial_line_change_flags(nullptr, 0));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_get_serial_line_change_flags(nullptr));
    c_serial_set_user_data(nullptr, buf);
    EXPECT_EQ(nullptr, c_serial_get_user_data(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_set_rts_control(nullptr, CSERIAL_RTS_NONE));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, (int)c_serial_get_rts_control(nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PORT, c_serial_flush(nullptr));
    c_serial_free_serial_ports_list(nullptr);
}

TEST_F(CSerialApi, DefaultsAreReportedWhileClosed) {
    EXPECT_EQ(0, c_serial_is_open(port_));
    EXPECT_EQ(nullptr, c_serial_get_port_name(port_));
    EXPECT_EQ(9600, c_serial_get_baud_rate(port_));
    EXPECT_EQ(CSERIAL_BITS_8, c_serial_get_data_bits(port_));
    EXPECT_EQ(CSERIAL_STOP_BITS_1, c_serial_get_stop_bits(port_));
    EXPECT_EQ(CSERIAL_PARITY_NONE, c_serial_get_parity(port_));
    EXPECT_EQ(CSERIAL_FLOW_NONE, c_serial_get_flow_control(port_));
    EXPECT_EQ(CSERIAL_RTS_NONE, c_serial_get_rts_control(port_));
    EXPECT_EQ(CSERIAL_LINE_FLAG_NONE, c_serial_get_serial_line_change_flags(port_));
}

TEST_F(CSerialApi, SettersAreCachedWhileClosed) {
    EXPECT_EQ(CSERIAL_OK, c_serial_set_baud_rate(port_, 115200));
    EXPECT_EQ(CSERIAL_OK, c_serial_set_data_bits(port_, CSERIAL_BITS_7));
    EXPECT_EQ(CSERIAL_OK, c_serial_set_stop_bits(port_, CSERIAL_STOP_BITS_2));
    EXPECT_EQ(CSERIAL_OK, c_serial_set_parity(port_, CSERIAL_PARITY_ODD));
    EXPECT_EQ(CSERIAL_OK, c_serial_set_flow_control(port_, CSERIAL_FLOW_SOFTWARE));
    EXPECT_EQ(CSERIAL_OK, c_serial_set_serial_line_change_flags(port_, CSERIAL_LINE_FLAG_ALL));

    EXPECT_EQ(115200, c_serial_get_baud_rate(port_));
    EXPECT_EQ(CSERIAL_BITS_7, c_serial_get_data_bits(port_));
    EXPECT_EQ(CSERIAL_STOP_BITS_2, c_serial_get_stop_bits(port_));
    EXPECT_EQ(CSERIAL_PARITY_ODD, c_serial_get_parity(port_));
    EXPECT_EQ(CSERIAL_FLOW_SOFTWARE, c_serial_get_flow_control(port_));
    EXPECT_EQ(CSERIAL_LINE_FLAG_ALL, c_serial_get_serial_line_change_flags(port_));
}

TEST_F(CSerialApi, InvalidSettingsAreRejected) {
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_baud_rate(port_, -1));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_data_bits(port_, (CSerial_Data_Bits)9));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_stop_bits(port_, (CSerial_Stop_Bits)3));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_parity(port_, (CSerial_Parity)7));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_flow_control(port_, (CSerial_Flow_Control)7));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_rts_control(port_, (CSerial_RTS_Handling)9));

    /* rejected values do not overwrite the previous ones */
    EXPECT_EQ(9600, c_serial_get_baud_rate(port_));
    EXPECT_EQ(CSERIAL_BITS_8, c_serial_get_data_bits(port_));
}

TEST_F(CSerialApi, HardwareFlowConflictsWithRtsHandling) {
    ASSERT_EQ(CSERIAL_OK, c_serial_set_rts_control(port_, CSERIAL_RTS_SOFTWARE));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_FLOW, c_serial_set_flow_control(port_, CSERIAL_FLOW_HARDWARE));
    EXPECT_EQ(CSERIAL_FLOW_NONE, c_serial_get_flow_control(port_));

    ASSERT_EQ(CSERIAL_OK, c_serial_set_rts_control(port_, CSERIAL_RTS_NONE));
    ASSERT_EQ(CSERIAL_OK, c_serial_set_flow_control(port_, CSERIAL_FLOW_HARDWARE));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_FLOW, c_serial_set_rts_control(port_, CSERIAL_RTS_HARDWARE));
    EXPECT_EQ(CSERIAL_RTS_NONE, c_serial_get_rts_control(port_));
}

TEST_F(CSerialApi, PortNameIsCopied) {
#ifdef _WIN32
    std::string name = "COM7";
    const char* expected = "\\\\.\\COM7";
#else
    std::string name = "/dev/ttyS7";
    const char* expected = "/dev/ttyS7";
#endif
    ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port_, name.c_str()));
    name.assign(name.size(), 'X');
    EXPECT_STREQ(expected, c_serial_get_port_name(port_));

    /* replacing the name must not leak or corrupt */
#ifdef _WIN32
    ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port_, "COM12"));
    EXPECT_STREQ("\\\\.\\COM12", c_serial_get_port_name(port_));
#else
    ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port_, "/dev/ttyUSB0"));
    EXPECT_STREQ("/dev/ttyUSB0", c_serial_get_port_name(port_));
#endif
}

TEST_F(CSerialApi, PortNameValidation) {
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_port_name(port_, nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_port_name(port_, ""));
#ifdef _WIN32
    EXPECT_EQ(CSERIAL_ERROR_NAME_TOO_LONG, c_serial_set_port_name(port_, "COM12345"));
#else
    std::string long_name(300, 'a');
    EXPECT_EQ(CSERIAL_ERROR_NAME_TOO_LONG, c_serial_set_port_name(port_, long_name.c_str()));
#endif
}

TEST_F(CSerialApi, UserDataRoundTrip) {
    int marker = 0;
    c_serial_set_user_data(port_, &marker);
    EXPECT_EQ(&marker, c_serial_get_user_data(port_));
}

TEST_F(CSerialApi, OpenWithoutNameFails) {
    EXPECT_EQ(CSERIAL_ERROR_NO_PORT, c_serial_open(port_));
    EXPECT_EQ(0, c_serial_is_open(port_));
}

TEST_F(CSerialApi, OpenMissingPortFails) {
    ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port_, kMissingPort));
    EXPECT_EQ(CSERIAL_ERROR_NO_SUCH_SERIAL_PORT, c_serial_open(port_));
    EXPECT_EQ(0, c_serial_is_open(port_));
}

TEST_F(CSerialApi, OpenNonSerialDeviceFails) {
    ASSERT_EQ(CSERIAL_OK, c_serial_set_port_name(port_, kNotASerialPort));
    EXPECT_EQ(CSERIAL_ERROR_NOT_A_SERIAL_PORT, c_serial_open(port_));
    EXPECT_EQ(0, c_serial_is_open(port_));
    /* keep_settings must not bypass the check */
    EXPECT_EQ(CSERIAL_ERROR_NOT_A_SERIAL_PORT, c_serial_open_keep_settings(port_, 1));
    EXPECT_EQ(0, c_serial_is_open(port_));
}

TEST_F(CSerialApi, IoOnClosedPortFails) {
    char buf[8] = {0};
    int length = sizeof(buf);
    int available = 0;
    c_serial_handle_t handle;
    c_serial_control_lines_type lines;

    EXPECT_NE(CSERIAL_OK, c_serial_write_data(port_, buf, &length));
    length = sizeof(buf);
    EXPECT_EQ(CSERIAL_ERROR_NO_SUCH_SERIAL_PORT, c_serial_read_data_timeout(port_, buf, &length, nullptr, 10));
    EXPECT_EQ(0, length);
    EXPECT_NE(CSERIAL_OK, c_serial_get_available(port_, &available));
    EXPECT_NE(CSERIAL_OK, c_serial_get_control_lines(port_, &lines));
    EXPECT_EQ(CSERIAL_ERROR_NO_PORT, c_serial_get_native_handle(port_, &handle));
    EXPECT_EQ(CSERIAL_ERROR_NO_PORT, c_serial_get_poll_handle(port_, &handle));
}

TEST_F(CSerialApi, InvalidIoArgumentsAreRejected) {
    char buf[8] = {0};
    int length = -1;
    c_serial_control_lines_type lines;

    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_write_data(port_, buf, &length));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_write_data(port_, buf, nullptr));
    length = 4;
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_write_data(port_, nullptr, &length));
    length = -1;
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_write_data_timeout(port_, buf, &length, 10));

    length = -5;
    EXPECT_EQ(CSERIAL_ERROR_INCORRECT_READ_PARAMS, c_serial_read_data_timeout(port_, buf, &length, nullptr, 10));
    length = 0;
    EXPECT_EQ(CSERIAL_ERROR_INCORRECT_READ_PARAMS, c_serial_read_data_timeout(port_, buf, &length, nullptr, 10));
    EXPECT_EQ(CSERIAL_ERROR_INCORRECT_READ_PARAMS, c_serial_read_data_timeout(port_, buf, nullptr, nullptr, 10));
    EXPECT_EQ(CSERIAL_ERROR_INCORRECT_READ_PARAMS, c_serial_read_data_timeout(port_, nullptr, nullptr, nullptr, 10));

    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_get_available(port_, nullptr));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_set_control_line(port_, nullptr, 0));
    EXPECT_EQ(CSERIAL_ERROR_INVALID_PARAM, c_serial_get_control_lines(port_, nullptr));
    (void)lines;
}

TEST_F(CSerialApi, CancelWithoutReaderReturnsImmediately) {
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(CSERIAL_OK, c_serial_read_cancel(port_, 1000));
    EXPECT_EQ(CSERIAL_OK, c_serial_read_cancel(port_, -1));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(500));
}

TEST_F(CSerialApi, CloseIsIdempotent) {
    c_serial_close(port_);
    c_serial_close(port_);
    EXPECT_EQ(0, c_serial_is_open(port_));
}

TEST(CSerialErrors, EveryErrorCodeHasAString) {
    const char* unknown = c_serial_get_error_string(12345);
    ASSERT_NE(nullptr, unknown);
    for (int code = CSERIAL_ERROR_INVALID_PARAM; code <= CSERIAL_OK; ++code) {
        const char* text = c_serial_get_error_string(code);
        ASSERT_NE(nullptr, text) << code;
        EXPECT_STRNE(unknown, text) << "missing description for " << code;
    }
}

TEST(CSerialPortsList, IsNullTerminatedAndFreeable) {
    const char** list = c_serial_get_serial_ports_list();
    ASSERT_NE(nullptr, list);
    int count = 0;
    while (list[count] != nullptr) {
        ASSERT_LT(count, CSERIAL_MAX_PORTS);
        EXPECT_GT(std::strlen(list[count]), 0u);
#ifndef _WIN32
        EXPECT_EQ(0, std::strncmp(list[count], "/dev/", 5)) << list[count];
#else
        EXPECT_EQ(0, std::strncmp(list[count], "COM", 3)) << list[count];
#endif
        ++count;
    }
    c_serial_free_serial_ports_list(list);
}
