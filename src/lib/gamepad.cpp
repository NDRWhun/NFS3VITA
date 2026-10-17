#include <lib/gamepad.h>
#include <lib/textfield.h>
#include <lib/window.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <cstring>

namespace win32
{

// Race keys apply while the game polled the joystick this recently (the race input thread polls at
// 32 Hz; the Controls and Dead Zone menus poll every frame).
static const uint64_t s_raceModeUs = 250000;

// updateKeys() runs on the message thread and the game thread; s_keysMutex guards everything below.
static pf::LwMutex     s_keysMutex("nfs3_pad_keys");
static bool            s_polled;            // DirectInput Poll seen at s_pollUs
static uint64_t        s_pollUs;
static controls::Mode  s_mode = controls::MenuMode;
static uint32_t        s_keysHeld;          // bit per controls::Key posted down
static uint32_t        s_blocked;           // controls::Input bits that post nothing until released
static Gamepad*        s_gp1;
static bool            s_textFieldOwned;    // TextField::ownsPad() at the last update

static void pushKey(const KeyCode& key, bool down)
{
    Window::pushKey(key.vk, key.scancode, down, down ? key.character : 0);
}

// Posts the key changes from s_keysHeld to held: releases first, then presses.
static void postKeys(uint32_t held)
{
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool down = pass == 1;
        const uint32_t changed = (held ^ s_keysHeld) & (down ? held : s_keysHeld);
        for (uint32_t key = 0; key < controls::KeyCount; ++key)
        {
            if (changed & (uint32_t(1) << key))
                pushKey(controls::keyCode(controls::Key(key)), down);
        }
    }
    s_keysHeld = held;
}

// Posts key-ups for all held keys; the inputs still pressed post nothing until released.
static void releaseKeys(uint32_t inputs)
{
    postKeys(0);
    s_blocked = inputs;
}

static bool raceMode()
{
    return s_polled && pf::ticksUs() - s_pollUs < s_raceModeUs;
}

void Input::poll()
{
    pf::LwLockGuard lock(s_keysMutex);
    s_polled = true;
    s_pollUs = pf::ticksUs();
}

void Gamepad::pushNamedKey(const char* name, bool down)
{
    const KeyCode* key = controls::keyByName(name);
    if (key)
        pushKey(*key, down);
}

void Gamepad::updateKeys()
{
    pf::LwLockGuard lock(s_keysMutex);
    const controls::PadState pad = controls::readPad();
    const bool textFieldOwned = TextField::ownsPad();
    if (textFieldOwned && !s_textFieldOwned)
        releaseKeys(pad.inputs);
    s_textFieldOwned = textFieldOwned;
    if (textFieldOwned)
    {
        // Post no keys, so buttons held when the text field returns the pad post no key press.
        s_blocked |= pad.inputs;
        return;
    }
    const controls::Mode mode = raceMode() ? controls::RaceMode : controls::MenuMode;
    if (mode != s_mode)
    {
        releaseKeys(pad.inputs);
        s_mode = mode;
    }
    s_blocked &= pad.inputs;
    postKeys(controls::heldKeys(pad.inputs & ~s_blocked, mode));
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
    // DirectInput defaults: range 0..65535, no dead zone, saturation at full deflection.
    for (AxisProperties& axis : m_axisProperties)
        axis = { 0, 65535, 0, 10000 };
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
    return controls::joystickButtonCount();
}

x86::reg32 Gamepad::getAxesCount() const
{
    return controls::JoystickAxisCount;
}

GamepadState Gamepad::getState() const
{
    const controls::PadState pad = controls::readPad();
    GamepadState state;
    memset(&state, 0, sizeof(state));
    for (x86::reg32 axis = 0; axis < controls::JoystickAxisCount; ++axis)
        state.axes[axis] = controls::joystickAxis(pad, controls::JoystickAxis(axis));
    state.buttons = controls::joystickButtons(pad);
    return state;
}

}
