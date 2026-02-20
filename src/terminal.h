#pragma once
#include <string>
#include <cstdint>
#include <termios.h>

namespace pmt {

class Terminal {
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void enter_raw_mode();
    void exit_raw_mode();
    void enter_alt_screen();
    void exit_alt_screen();
    void hide_cursor();
    void show_cursor();

    void clear();
    void move_to(int row, int col);
    void write(const std::string& s);
    void write(const char* s);
    void write_truncated(const std::string& s, int max_width);
    void flush();

    enum Color {
        Black = 0, Red, Green, Yellow, Blue, Magenta, Cyan, White,
        BrightBlack, BrightRed, BrightGreen, BrightYellow,
        BrightBlue, BrightMagenta, BrightCyan, BrightWhite,
        Default = -1,
    };

    static const char* fg(Color c);
    static std::string fg_rgb(uint8_t r, uint8_t g, uint8_t b);
    static const char* bold();
    static const char* dim();
    static const char* reverse_video();
    static const char* reset();

    int rows() const { return rows_; }
    int cols() const { return cols_; }
    void update_size();

private:
    struct termios orig_termios_{};
    bool raw_mode_ = false;
    int rows_ = 24;
    int cols_ = 80;
    std::string buffer_;
};

}
