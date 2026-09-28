#ifndef LIB_GAMEPAD_H_
#define LIB_GAMEPAD_H_

#include <lib/controls.h>
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

// The Vita controller exposed as DirectInput joystick 0; layout in controls.cpp.
class Gamepad : public Input
{
public:
    // DirectInput axis properties (DIPROP_RANGE, DIPROP_DEADZONE, DIPROP_SATURATION).
    struct AxisProperties
    {
        x86::sreg32 min;
        x86::sreg32 max;
        x86::reg32  deadZone;   // 0..10000 of the distance from the centre
        x86::reg32  saturation;
    };

    Gamepad(x86::reg32 gamepadIndex);
    ~Gamepad();

    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;

    static x86::reg32 getCount();

    // Guest threads with the execution lock held.
    AxisProperties& axisProperties(x86::reg32 axis) { return m_axisProperties[axis]; }

    // Joystick axes (-32767..32767) and buttons of the current pad state. Any thread.
    GamepadState getState() const;
    // Turns pad changes into keyboard messages (menu keys, or race keys while the game polls the joystick)
    // unless a text field (TextField) owns the pad. Any thread.
    static void updateKeys();
    // Posts a key by name (up down left right enter escape space tab b delete); unknown names are ignored.
    static void pushNamedKey(const char* name, bool down);

private:
    AxisProperties m_axisProperties[controls::JoystickAxisCount];
};

}

#endif
