#include <lib/gamepad.h>
#include <lib/window.h>
#include <psp2/ctrl.h>
#include <cstring>

namespace win32
{

// Joystick button indices seen by the game.
enum PadButton
{
    PadCross = 0,
    PadSquare,
    PadCircle,
    PadTriangle,
    PadL,
    PadR,
    PadSelect,
    PadStart,
    PadUp,
    PadDown,
    PadLeft,
    PadRight,
    PadButtonCount
};

static const x86::reg32 s_axisCount = 4;
static const x86::sreg16 s_axisKeyThreshold = 30000;

// Virtual key + PC scancode pairs used for menu navigation.
struct KeyCode { x86::reg32 vk; x86::reg8 scancode; x86::reg32 character; };
static const KeyCode s_keyEnter  = { 0x0D, 0x1C, 0x0D };
static const KeyCode s_keyEscape = { 0x1B, 0x01, 0x1B };
static const KeyCode s_keySpace  = { 0x20, 0x39, 0x20 };
static const KeyCode s_keyLeft   = { 0x25, 0x4B, 0 };
static const KeyCode s_keyUp     = { 0x26, 0x48, 0 };
static const KeyCode s_keyRight  = { 0x27, 0x4D, 0 };
static const KeyCode s_keyDown   = { 0x28, 0x50, 0 };

static x86::reg32   s_kbPoll = 3;
static GamepadState s_state;
static Gamepad*     s_gp1;

static x86::sreg16 stickAxis(unsigned char raw)
{
    int value = (int(raw) - 128) * 256;
    if (value > 32767) value = 32767;
    return x86::sreg16(value);
}

static GamepadState readPad()
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    GamepadState state;
    memset(&state, 0, sizeof(state));
    state.axes[0] = stickAxis(pad.lx);
    state.axes[1] = stickAxis(pad.ly);
    state.axes[2] = stickAxis(pad.rx);
    state.axes[3] = stickAxis(pad.ry);

    // D-pad steers when the left stick is centered.
    if (state.axes[0] > -8192 && state.axes[0] < 8192)
    {
        if (pad.buttons & SCE_CTRL_LEFT)  state.axes[0] = -32767;
        if (pad.buttons & SCE_CTRL_RIGHT) state.axes[0] = 32767;
    }
    if (state.axes[1] > -8192 && state.axes[1] < 8192)
    {
        if (pad.buttons & SCE_CTRL_UP)   state.axes[1] = -32767;
        if (pad.buttons & SCE_CTRL_DOWN) state.axes[1] = 32767;
    }

    if (pad.buttons & SCE_CTRL_CROSS)    state.buttons |= x86::reg64(1) << PadCross;
    if (pad.buttons & SCE_CTRL_SQUARE)   state.buttons |= x86::reg64(1) << PadSquare;
    if (pad.buttons & SCE_CTRL_CIRCLE)   state.buttons |= x86::reg64(1) << PadCircle;
    if (pad.buttons & SCE_CTRL_TRIANGLE) state.buttons |= x86::reg64(1) << PadTriangle;
    if (pad.buttons & SCE_CTRL_LTRIGGER) state.buttons |= x86::reg64(1) << PadL;
    if (pad.buttons & SCE_CTRL_RTRIGGER) state.buttons |= x86::reg64(1) << PadR;
    if (pad.buttons & SCE_CTRL_SELECT)   state.buttons |= x86::reg64(1) << PadSelect;
    if (pad.buttons & SCE_CTRL_START)    state.buttons |= x86::reg64(1) << PadStart;
    if (pad.buttons & SCE_CTRL_UP)       state.buttons |= x86::reg64(1) << PadUp;
    if (pad.buttons & SCE_CTRL_DOWN)     state.buttons |= x86::reg64(1) << PadDown;
    if (pad.buttons & SCE_CTRL_LEFT)     state.buttons |= x86::reg64(1) << PadLeft;
    if (pad.buttons & SCE_CTRL_RIGHT)    state.buttons |= x86::reg64(1) << PadRight;
    return state;
}

static const KeyCode& buttonKey(x86::reg32 button)
{
    switch (button)
    {
    case PadStart:  return s_keyEnter;
    case PadSelect: return s_keyEscape;
    default:        return s_keySpace;
    }
}

static void axisKeys(x86::sreg16 previous, x86::sreg16 current, const KeyCode& negative, const KeyCode& positive)
{
    if (previous > s_axisKeyThreshold && current <= s_axisKeyThreshold)
        Window::pushKey(positive.vk, positive.scancode, false, 0);
    if (previous < -s_axisKeyThreshold && current >= -s_axisKeyThreshold)
        Window::pushKey(negative.vk, negative.scancode, false, 0);
    if (previous <= s_axisKeyThreshold && current > s_axisKeyThreshold)
        Window::pushKey(positive.vk, positive.scancode, true, 0);
    if (previous >= -s_axisKeyThreshold && current < -s_axisKeyThreshold)
        Window::pushKey(negative.vk, negative.scancode, true, 0);
}

void Input::poll()
{
    s_kbPoll = 3;
}

void Gamepad::updateKeys()
{
    if (s_kbPoll == 0)
    {
        GamepadState state = readPad();
        axisKeys(s_state.axes[0], state.axes[0], s_keyLeft, s_keyRight);
        axisKeys(s_state.axes[1], state.axes[1], s_keyUp, s_keyDown);
        // D-pad buttons are already folded into the axes; skip them here.
        for (x86::reg32 button = 0; button < PadUp; ++button)
        {
            x86::reg64 mask = x86::reg64(1) << button;
            if ((s_state.buttons & mask) != (state.buttons & mask))
            {
                const KeyCode& key = buttonKey(button);
                bool down = (state.buttons & mask) != 0;
                Window::pushKey(key.vk, key.scancode, down, down ? key.character : 0);
            }
        }
        s_state = state;
    }
    else
    {
        // The game is polling DirectInput: release any keys still held from menu mode.
        for (x86::reg32 button = 0; button < PadUp; ++button)
        {
            x86::reg64 mask = x86::reg64(1) << button;
            if (s_state.buttons & mask)
            {
                const KeyCode& key = buttonKey(button);
                Window::pushKey(key.vk, key.scancode, false, 0);
            }
        }
        axisKeys(s_state.axes[0], 0, s_keyLeft, s_keyRight);
        axisKeys(s_state.axes[1], 0, s_keyUp, s_keyDown);
        memset(&s_state, 0, sizeof(s_state));
        s_kbPoll--;
    }
}

x86::reg32 Input::getButtonCount() const
{
    return 0;
}

x86::reg32 Input::getAxesCount() const
{
    return 0;
}

Gamepad::Gamepad(x86::reg32 gamepadIndex)
{
    NFS2_USE(gamepadIndex);
    if (!s_gp1)
        s_gp1 = this;
}

Gamepad::~Gamepad()
{
    if (s_gp1 == this)
        s_gp1 = nullptr;
}

x86::reg32 Gamepad::getCount()
{
    return 1;
}

x86::reg32 Gamepad::getButtonCount() const
{
    return PadButtonCount;
}

x86::reg32 Gamepad::getAxesCount() const
{
    return s_axisCount;
}

GamepadState Gamepad::getState() const
{
    return readPad();
}

}
