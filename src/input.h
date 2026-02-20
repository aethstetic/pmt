#pragma once
#include <string>

namespace pmt {

enum class Key {
    None,
    Char,
    Up, Down, Left, Right,
    Home, End,
    PageUp, PageDown,
    Enter, Tab, Backspace, Delete, Escape,
    CtrlC, CtrlD, CtrlL,
};

struct KeyEvent {
    Key key = Key::None;
    char ch = 0;
};

class Input {
public:
    KeyEvent read_key();
    KeyEvent read_key_timeout(int timeout_ms);

private:
    KeyEvent decode_escape();
    int read_byte(int timeout_ms = -1);
};

}
