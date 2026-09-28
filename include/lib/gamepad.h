#ifndef LIB_GAMEPAD_H_
#define LIB_GAMEPAD_H_

#include <lib/winapp.h>

namespace win32
{

enum InputType
{
    InputType_Keyboard,
    InputType_Mouse,
    InputType_Gamepad
};

class Input : public GenericResource
{
public:
    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;
    static void poll();
};

class Keyboard : public Input
{
};

class Mouse : public Input
{
};

struct GamepadState
{
    x86::reg64  buttons;
    x86::sreg16 axes[10];
    x86::reg8   hats[10];
};

// The Vita controller exposed as DirectInput joystick 0.
class Gamepad : public Input
{
public:
    Gamepad(x86::reg32 gamepadIndex);
    ~Gamepad();

    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;

    static x86::reg32 getCount();

    GamepadState getState() const;
    // Turns pad changes into keyboard messages while the game is not polling DirectInput; Triangle opens
    // the system IME and types the confirmed text into the game. Any thread.
    static void updateKeys();
    // Posts a key by name (up down left right enter escape space); unknown names are ignored.
    static void pushNamedKey(const char* name, bool down);
};

}

#endif
