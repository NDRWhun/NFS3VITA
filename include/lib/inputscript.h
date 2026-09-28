#ifndef LIB_INPUTSCRIPT_H_
#define LIB_INPUTSCRIPT_H_

namespace win32
{

// Replays key presses listed in a text file ("<milliseconds> <key>" per line) for unattended tests.
// Times count from the latest switch to Glide rendering (menu shown, race started); the clock stops
// while DirectDraw frames (movies, loading screens) are presented.
// Keys: up down left right enter escape space.
class InputScript
{
public:
    static void load(const char* path);
    static void resume();   // a Glide frame is being drawn
    static void pause();    // a DirectDraw frame is being presented
    static void update();   // called from the input poll, any thread
};

}

#endif
