#include "input.h"
#include <unistd.h>
#include <poll.h>

namespace pmt {

int Input::read_byte(int timeout_ms) {
    if (timeout_ms >= 0) {
        struct pollfd pfd{};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return -1;
    }
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    return c;
}

KeyEvent Input::read_key() {
    return read_key_timeout(-1);
}

KeyEvent Input::read_key_timeout(int timeout_ms) {
    int c = read_byte(timeout_ms);
    if (c < 0) return {Key::None, 0};

    if (c == 27) return decode_escape();
    if (c == '\r' || c == '\n') return {Key::Enter, 0};
    if (c == '\t') return {Key::Tab, 0};
    if (c == 127 || c == 8) return {Key::Backspace, 0};
    if (c == 3) return {Key::CtrlC, 0};
    if (c == 4) return {Key::CtrlD, 0};
    if (c == 12) return {Key::CtrlL, 0};

    if (c >= 32 && c < 127) {
        return {Key::Char, static_cast<char>(c)};
    }

    return {Key::None, 0};
}

/* decodes ANSI escape sequences into key events */
KeyEvent Input::decode_escape() {
    int c1 = read_byte(50);
    if (c1 < 0) return {Key::Escape, 0};

    if (c1 == '[') {
        int c2 = read_byte(50);
        if (c2 < 0) return {Key::Escape, 0};

        if (c2 >= '0' && c2 <= '9') {
            int c3 = read_byte(50);
            if (c3 == '~') {
                switch (c2) {
                    case '1': return {Key::Home, 0};
                    case '3': return {Key::Delete, 0};
                    case '4': return {Key::End, 0};
                    case '5': return {Key::PageUp, 0};
                    case '6': return {Key::PageDown, 0};
                    case '7': return {Key::Home, 0};
                    case '8': return {Key::End, 0};
                }
            }
            return {Key::None, 0};
        }

        switch (c2) {
            case 'A': return {Key::Up, 0};
            case 'B': return {Key::Down, 0};
            case 'C': return {Key::Right, 0};
            case 'D': return {Key::Left, 0};
            case 'H': return {Key::Home, 0};
            case 'F': return {Key::End, 0};
        }
    } else if (c1 == 'O') {
        int c2 = read_byte(50);
        if (c2 < 0) return {Key::Escape, 0};
        switch (c2) {
            case 'H': return {Key::Home, 0};
            case 'F': return {Key::End, 0};
        }
    }

    return {Key::Escape, 0};
}

}
