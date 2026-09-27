#ifndef LIB_INPUTSCRIPT_H_
#define LIB_INPUTSCRIPT_H_

namespace win32
{

// Replays key presses listed in a text file ("<milliseconds> <key>" per line) for unattended tests.
// Keys: up down left right enter escape space.
class InputScript
{
public:
    static void load(const char* path);
    static void update(); // called from the input poll
};

}

#endif
