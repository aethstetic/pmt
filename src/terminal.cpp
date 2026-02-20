#include "terminal.h"
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstdio>

namespace pmt {

Terminal::Terminal() {
    buffer_.reserve(4096);
    update_size();
}

Terminal::~Terminal() {
    if (raw_mode_) {
        exit_raw_mode();
    }
}

void Terminal::enter_raw_mode() {
    if (raw_mode_) return;
    tcgetattr(STDIN_FILENO, &orig_termios_);
    struct termios raw = orig_termios_;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_ = true;
}

void Terminal::exit_raw_mode() {
    if (!raw_mode_) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
    raw_mode_ = false;
}

void Terminal::enter_alt_screen() { buffer_ += "\033[?1049h"; }
void Terminal::exit_alt_screen()  { buffer_ += "\033[?1049l"; }
void Terminal::hide_cursor()      { buffer_ += "\033[?25l"; }
void Terminal::show_cursor()      { buffer_ += "\033[?25h"; }
void Terminal::clear()            { buffer_ += "\033[2J\033[H"; }

void Terminal::move_to(int row, int col) {
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "\033[%d;%dH", row + 1, col + 1);
    buffer_.append(buf, n);
}

void Terminal::write(const std::string& s) { buffer_ += s; }
void Terminal::write(const char* s)        { buffer_ += s; }

void Terminal::write_truncated(const std::string& s, int max_width) {
    if (max_width <= 0) return;
    int len = static_cast<int>(s.size());
    if (len <= max_width) {
        buffer_ += s;
    } else if (max_width <= 3) {
        buffer_.append(s, 0, max_width);
    } else {
        buffer_.append(s, 0, max_width - 3);
        buffer_ += "...";
    }
}

void Terminal::flush() {
    if (!buffer_.empty()) {
        ::write(STDOUT_FILENO, buffer_.data(), buffer_.size());
        buffer_.clear();
    }
}

const char* Terminal::fg(Color c) {
    static constexpr const char* table[] = {
        "\033[30m", "\033[31m", "\033[32m", "\033[33m",
        "\033[34m", "\033[35m", "\033[36m", "\033[37m",
        "\033[90m", "\033[91m", "\033[92m", "\033[93m",
        "\033[94m", "\033[95m", "\033[96m", "\033[97m",
    };
    if (c == Default) return "\033[39m";
    int ci = static_cast<int>(c);
    if (ci >= 0 && ci < 16) return table[ci];
    return "";
}

std::string Terminal::fg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    char buf[24];
    snprintf(buf, sizeof(buf), "\033[38;2;%d;%d;%dm", r, g, b);
    return buf;
}

const char* Terminal::bold()          { return "\033[1m"; }
const char* Terminal::dim()           { return "\033[2m"; }
const char* Terminal::reverse_video() { return "\033[7m"; }
const char* Terminal::reset()         { return "\033[0m"; }

void Terminal::update_size() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        rows_ = ws.ws_row;
        cols_ = ws.ws_col;
    }
}

}
