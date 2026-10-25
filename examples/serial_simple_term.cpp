
#include <thread>

#include <iostream>
#include <string>
#include <thread> // contains <chrono>
#include <cstring>

#ifndef CSERIAL_STATIC
#define CSERIAL_STATIC
#endif

#include <cserial/c_serial.h>

void println(const std::string& s = "") {
	std::cout << s << std::endl;
}
void sleep(const double t) {
	if (t > 0.0) std::this_thread::sleep_for(std::chrono::milliseconds((int)(1E3*t + 0.5)));
}



// ASCII codes (key>0): 8 backspace, 9 tab, 10 newline, 27 escape, 127 delete, !"#$%&'()*+,-./0-9:;<=>?@A-Z[]^_`a-z{|}~üäÄöÖÜßµ´§°¹³²
// control key codes (key<0): -38/-40/-37/-39 up/down/left/right arrow, -33/-34 page up/down, -36/-35 pos1/end
// other key codes (key<0): -45 insert, -144 num lock, -20 caps lock, -91 windows key, -93 kontext menu key, -112 to -123 F1 to F12
// not working: ¹ (251), num lock (-144), caps lock (-20), windows key (-91), kontext menu key (-93), F11 (-122)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
int key_press() { // not working: F11 (-122, toggles fullscreen)
	KEY_EVENT_RECORD keyevent;
	INPUT_RECORD irec;
	DWORD events;
	while (true) {
		ReadConsoleInput(GetStdHandle(STD_INPUT_HANDLE), &irec, 1, &events);
		if (irec.EventType == KEY_EVENT && ((KEY_EVENT_RECORD&)irec.Event).bKeyDown) {
			keyevent = (KEY_EVENT_RECORD&)irec.Event;
			const int ca = (int)keyevent.uChar.AsciiChar;
			const int cv = (int)keyevent.wVirtualKeyCode;
			const int key = ca == 0 ? -cv : ca + (ca > 0 ? 0 : 256);
			switch (key) {
			case  -16: continue; // disable Shift
			case  -17: continue; // disable Ctrl / AltGr
			case  -18: continue; // disable Alt / AltGr
			case -220: continue; // disable first detection of "^" key (not "^" symbol)
			case -221: continue; // disable first detection of "`" key (not "`" symbol)
			case -191: continue; // disable AltGr + "#"
			case  -52: continue; // disable AltGr + "4"
			case  -53: continue; // disable AltGr + "5"
			case  -54: continue; // disable AltGr + "6"
			case  -12: continue; // disable num block 5 with num lock deactivated
			case   13: return  10; // enter
			case  -46: return 127; // delete
			case  -49: return 251; // ¹
			case    0: continue;
			case    1: continue; // disable Ctrl + a (selects all text)
			case    2: continue; // disable Ctrl + b
			case    3: continue; // disable Ctrl + c (terminates program)
			case    4: continue; // disable Ctrl + d
			case    5: continue; // disable Ctrl + e
			case    6: continue; // disable Ctrl + f (opens search)
			case    7: continue; // disable Ctrl + g
			//case    8: continue; // disable Ctrl + h (ascii for backspace)
			//case    9: continue; // disable Ctrl + i (ascii for tab)
			case   10: continue; // disable Ctrl + j
			case   11: continue; // disable Ctrl + k
			case   12: continue; // disable Ctrl + l
			//case   13: continue; // disable Ctrl + m (breaks console, ascii for new line)
			case   14: continue; // disable Ctrl + n
			case   15: continue; // disable Ctrl + o
			case   16: continue; // disable Ctrl + p
			case   17: continue; // disable Ctrl + q
			case   18: continue; // disable Ctrl + r
			case   19: continue; // disable Ctrl + s
			case   20: continue; // disable Ctrl + t
			case   21: continue; // disable Ctrl + u
			case   22: continue; // disable Ctrl + v (inserts clipboard)
			case   23: continue; // disable Ctrl + w
			case   24: continue; // disable Ctrl + x
			case   25: continue; // disable Ctrl + y
			case   26: continue; // disable Ctrl + z
			default: return key; // any other ASCII/virtual character
			}
		}
	}
}
#elif defined(__linux__)
#include <sys/ioctl.h>
#include <termios.h>
int key_press() { // not working: ¹ (251), num lock (-144), caps lock (-20), windows key (-91), kontext menu key (-93)
	struct termios term;
	tcgetattr(0, &term);
	while (true) {
		term.c_lflag &= ~(ICANON | ECHO); // turn off line buffering and echoing
		tcsetattr(0, TCSANOW, &term);
		int nbbytes;
		ioctl(0, FIONREAD, &nbbytes); // 0 is STDIN
		while (!nbbytes) {
			sleep(0.01);
			fflush(stdout);
			ioctl(0, FIONREAD, &nbbytes); // 0 is STDIN
		}
		int key = (int)getchar();
		if (key == 27 || key == 194 || key == 195) { // escape, 194/195 is escape for °ß´äöüÄÖÜ
			key = (int)getchar();
			if (key == 91) { // [ following escape
				key = (int)getchar(); // get code of next char after \e[
				if (key == 49) { // F5-F8
					key = 62 + (int)getchar(); // 53, 55-57
					if (key == 115) key++; // F5 code is too low by 1
					getchar(); // take in following ~ (126), but discard code
				}
				else if (key == 50) { // insert or F9-F12
					key = (int)getchar();
					if (key == 126) { // insert
						key = 45;
					}
					else { // F9-F12
						key += 71; // 48, 49, 51, 52
						if (key < 121) key++; // F11 and F12 are too low by 1
						getchar(); // take in following ~ (126), but discard code
					}
				}
				else if (key == 51 || key == 53 || key == 54) { // delete, page up/down
					getchar(); // take in following ~ (126), but discard code
				}
			}
			else if (key == 79) { // F1-F4
				key = 32 + (int)getchar(); // 80-83
			}
			key = -key; // use negative numbers for escaped keys
		}
		term.c_lflag |= (ICANON | ECHO); // turn on line buffering and echoing
		tcsetattr(0, TCSANOW, &term);
		switch (key) {
		case  127: return   8; // backspace
		case  -27: return  27; // escape
		case  -51: return 127; // delete
		case -164: return 132; // ä
		case -182: return 148; // ö
		case -188: return 129; // ü
		case -132: return 142; // Ä
		case -150: return 153; // Ö
		case -156: return 154; // Ü
		case -159: return 225; // ß
		case -181: return 230; // µ
		case -167: return 245; // §
		case -176: return 248; // °
		case -178: return 253; // ²
		case -179: return 252; // ³
		case -180: return 239; // ´
		case  -65: return -38; // up arrow
		case  -66: return -40; // down arrow
		case  -68: return -37; // left arrow
		case  -67: return -39; // right arrow
		case  -53: return -33; // page up
		case  -54: return -34; // page down
		case  -72: return -36; // pos1
		case  -70: return -35; // end
		case    0: continue;
		case    1: continue; // disable Ctrl + a
		case    2: continue; // disable Ctrl + b
		case    3: continue; // disable Ctrl + c (terminates program)
		case    4: continue; // disable Ctrl + d
		case    5: continue; // disable Ctrl + e
		case    6: continue; // disable Ctrl + f
		case    7: continue; // disable Ctrl + g
		case    8: continue; // disable Ctrl + h
		//case    9: continue; // disable Ctrl + i (ascii for tab)
		//case   10: continue; // disable Ctrl + j (ascii for new line)
		case   11: continue; // disable Ctrl + k
		case   12: continue; // disable Ctrl + l
		case   13: continue; // disable Ctrl + m
		case   14: continue; // disable Ctrl + n
		case   15: continue; // disable Ctrl + o
		case   16: continue; // disable Ctrl + p
		case   17: continue; // disable Ctrl + q
		case   18: continue; // disable Ctrl + r
		case   19: continue; // disable Ctrl + s
		case   20: continue; // disable Ctrl + t
		case   21: continue; // disable Ctrl + u
		case   22: continue; // disable Ctrl + v
		case   23: continue; // disable Ctrl + w
		case   24: continue; // disable Ctrl + x
		case   25: continue; // disable Ctrl + y
		case   26: continue; // disable Ctrl + z (terminates program)
		default: return key; // any other ASCII character
		}
	}
}
#endif // Windows/Linux

struct terminal_context_type {
	c_serial_port_type* port;

};


void serial_read_thread_fn(terminal_context_type* ctx) {
	c_serial_control_lines_type m_lines;
	c_serial_control_lines_type prev_lines;
	/*
	 * Listen for anything that comes across, and echo it back.
	 */
	do {
		int data_length = 255;
		char data[255];

		int status = c_serial_read_data_timeout(ctx->port, data, &data_length, &m_lines, -1);
		if (status < 0) {
			if (status == CSERIAL_ERROR_TIMEOUT) {
				continue;
			}

			if (status == CSERIAL_ERROR_CANCELLED) {
				printf("CANCEL READ ");
				break;
			}

			break;
		}

		for (int x = 0; x < data_length; x++) {
			printf("%c" /*    0x%02X (ASCII: %c)\n"*/, data[x]/*, data[x]*/);
		}

		if (memcmp(&m_lines, &prev_lines, sizeof(c_serial_control_lines_type))) {
			printf("Serial line state: CD: %d CTS: %d DSR: %d DTR: %d RTS: %d RI: %d\n",
				m_lines.cd,
				m_lines.cts,
				m_lines.dsr,
				m_lines.dtr,
				m_lines.rts,
				m_lines.ri);

			prev_lines = m_lines;
		}
	} while (1);

}

int main(int argc, char* argv[]) {
	terminal_context_type terminal_ctx;
	int status;
	int x;
    int baud_rate = 115200;

	/*
	 * Use the first argument as the port to open
	 */
	if (argc < 2) {
		fprintf(stderr, "USAGE: simpleterm <port> [<baudrate>]\n");
		/*
		 * Since no port was provided, print the available ports
		 */
		const char** port_list = c_serial_get_serial_ports_list();
		x = 0;
		printf("Available ports:\n");
		while (port_list[x] != NULL) {
			printf("%s\n", port_list[x]);
			x++;
		}
		c_serial_free_serial_ports_list(port_list);
		return 1;
	}

    if (argc >= 3) {
      baud_rate = atoi(argv[2]);
    }

	/*
	 * Allocate the serial port struct.
	 * This defaults to 9600-8-N-1
	 */
	if (c_serial_new(&terminal_ctx.port, NULL) < 0) {
		fprintf(stderr, "ERROR: Unable to create new serial port\n");
		return 1;
	}

	/*
	 * The port name is the device to open(/dev/ttyS0 on Linux,
	 * COM1 on Windows)
	 */
	if (c_serial_set_port_name(terminal_ctx.port, argv[1]) < 0) {
		fprintf(stderr, "ERROR: can't set port name\n");
	}

	/*
	 * Set various serial port settings.  These are the default.
	 */
	c_serial_set_baud_rate(terminal_ctx.port, baud_rate);
	c_serial_set_data_bits(terminal_ctx.port, CSERIAL_BITS_8);
	c_serial_set_stop_bits(terminal_ctx.port, CSERIAL_STOP_BITS_1);
	c_serial_set_parity(terminal_ctx.port, CSERIAL_PARITY_NONE);
	c_serial_set_flow_control(terminal_ctx.port, CSERIAL_FLOW_NONE);

	printf("Baud rate is %d\n", c_serial_get_baud_rate(terminal_ctx.port));

	/*
	 * We want to get all line flags when they change
	 */
	c_serial_set_serial_line_change_flags(terminal_ctx.port, CSERIAL_LINE_FLAG_ALL);

	status = c_serial_open(terminal_ctx.port);
	if (status < 0) {
		fprintf(stderr, "ERROR: Can't open serial port\n");
		return 1;
	}

	std::thread read_thread(&serial_read_thread_fn, &terminal_ctx);

	while (true) {
		int key = key_press(); // blocks until a key is pressed
		//println("Input is: " + std::to_string(key) + ", \"" + (char)key + "\"");

		int data_length = 1;
		//c_serial_read_cancel(terminal_ctx.port, -1);
		//printf("S");
		status = c_serial_write_data(terminal_ctx.port, &key, &data_length);
		if (status < 0) {
			break;
		}

		if (key == 10) {
      // add \n to \r ?
			//key = 13;
			//int data_length = 1;
			//status = c_serial_write_data(terminal_ctx.port, &key, &data_length);
			//if (status < 0) {
			//	break;
			//}

		}

	}
	return 0;
}
