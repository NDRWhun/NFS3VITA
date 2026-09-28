#ifndef LIB_CONTROLS_H_
#define LIB_CONTROLS_H_

#include <x86.h>
#include <cstdint>

namespace win32
{

// A key sent as WM_KEYDOWN/WM_KEYUP; a non-zero character also sends WM_CHAR on key down.
struct KeyCode
{
    x86::reg32 vk;
    x86::reg8  scancode;
    x86::reg32 character;
};

// Vita pad layout: the DirectInput joystick the game binds its race actions to, and the keys posted
// for menus and for the keyboard-only race actions. The tables are in controls.cpp.
namespace controls
{

// Vita inputs; stick directions are digital views of the sticks.
enum Input
{
    Cross, Circle, Square, Triangle, L, R, Select, Start,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,
    RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
    InputCount
};

inline uint32_t bit(Input input) { return uint32_t(1) << input; }

struct PadState
{
    uint32_t    inputs;     // bit per Input
    x86::sreg16 stickX;     // left stick after dead zone and response curve, -32767..32767
    x86::sreg16 stickY;
};

// Reads the pad. Any thread.
PadState readPad();

// DirectInput joystick axes: lX steers (left stick X, d-pad left/right), lY is the left stick Y.
enum JoystickAxis
{
    AxisX,
    AxisY,
    JoystickAxisCount
};

// Product name and DIDEVTYPE_JOYSTICK | DIDEVTYPEJOYSTICK_GAMEPAD << 8; the game resets the
// bindings to its gamepad defaults when either differs from the device saved in config.dat.
extern const char* const JoystickName;
extern const x86::reg32  JoystickDeviceType;

x86::reg32  joystickButtonCount();
x86::sreg16 joystickAxis(const PadState& pad, JoystickAxis axis);  // -32767..32767
uint32_t    joystickButtons(const PadState& pad);                  // bit per DirectInput button

// Race: the game polls the joystick. Menu: everything else.
enum Mode
{
    MenuMode,
    RaceMode
};

enum Key
{
    KeyEnter, KeyEscape, KeySpace, KeyLeft, KeyUp, KeyRight, KeyDown, KeyTab, KeyB, KeyDelete,
    KeyCount
};

const KeyCode& keyCode(Key key);
// Key by name (enter escape space left up right down tab b delete); nullptr if unknown.
const KeyCode* keyByName(const char* name);
// Keys the pressed inputs hold down in a mode; bit per Key.
uint32_t heldKeys(uint32_t inputs, Mode mode);

}

}

#endif
