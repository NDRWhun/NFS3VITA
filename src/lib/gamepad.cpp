#include <lib/gamepad.h>
#include <lib/inputscript.h>
#include <lib/window.h>
#include <platform/gxm.h>
#include <platform/ime.h>
#include <platform/sync.h>
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
static const KeyCode s_keyDelete = { 0x2E, 0x53, 0 };   // clears a frontend text field

// Frames between typed keys: the frontend reads one key per frame and drops a key equal to the
// previous one unless a frame without a key came in between.
static const uint32_t s_framesPerKey = 4;

// updateKeys() runs on the message thread and the game thread; s_keysMutex guards everything below.
static pf::Mutex    s_keysMutex("nfs3_pad_keys", false);
static x86::reg32   s_kbPoll = 3;
static GamepadState s_state;
static Gamepad*     s_gp1;

// Confirmed IME text being typed into the game.
static char         s_typeText[pf::ime::MaxTextLength + 1];
static x86::reg32   s_typeNext;
static bool         s_typeClear;
static bool         s_typing;
static uint32_t     s_typeFrame;

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

// Triangle opens the IME instead of posting a key; the d-pad is folded into the axes.
static bool buttonHasKey(x86::reg32 button)
{
    return button < PadUp && button != PadTriangle;
}

static const KeyCode& buttonKey(x86::reg32 button)
{
    switch (button)
    {
    case PadCross:
    case PadStart:  return s_keyEnter;
    case PadCircle:
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

// Posts key-ups for everything still held in menu mode and forgets the pad state.
static void releaseKeys()
{
    for (x86::reg32 button = 0; button < PadUp; ++button)
    {
        if (buttonHasKey(button) && (s_state.buttons & (x86::reg64(1) << button)))
        {
            const KeyCode& key = buttonKey(button);
            Window::pushKey(key.vk, key.scancode, false, 0);
        }
    }
    axisKeys(s_state.axes[0], 0, s_keyLeft, s_keyRight);
    axisKeys(s_state.axes[1], 0, s_keyUp, s_keyDown);
    memset(&s_state, 0, sizeof(s_state));
}

// Types confirmed IME text: Delete empties the frontend text field, then one WM_CHAR per character.
// A key is posted once the previous one has left the message queue and s_framesPerKey frames passed.
static void typeText()
{
    const uint32_t frames = pf::gxm::Device::get().submittedFrameCount();
    if (!s_typing)
    {
        if (!pf::ime::takeText(s_typeText, sizeof(s_typeText)))
            return;
        s_typing = true;
        s_typeClear = true;
        s_typeNext = 0;
        s_typeFrame = frames - s_framesPerKey;
    }
    if (Window::inputPending())
    {
        s_typeFrame = frames;
        return;
    }
    if (frames - s_typeFrame < s_framesPerKey)
        return;
    s_typeFrame = frames;
    if (s_typeClear)
    {
        Window::pushKey(s_keyDelete.vk, s_keyDelete.scancode, true, 0);
        Window::pushKey(s_keyDelete.vk, s_keyDelete.scancode, false, 0);
        s_typeClear = false;
    }
    else if (s_typeText[s_typeNext])
    {
        Window::pushChar(x86::reg8(s_typeText[s_typeNext++]));
    }
    else
    {
        s_typing = false;
    }
}

// The IME dialog is open or its text is still being typed.
static bool imeOwnsPad()
{
    return s_typing || pf::ime::active();
}

void Input::poll()
{
    pf::LockGuard lock(s_keysMutex);
    s_kbPoll = 3;
}

void Gamepad::pushNamedKey(const char* name, bool down)
{
    static const struct { const char* name; const KeyCode* key; } s_names[] = {
        { "enter", &s_keyEnter }, { "escape", &s_keyEscape }, { "space", &s_keySpace },
        { "left", &s_keyLeft }, { "up", &s_keyUp }, { "right", &s_keyRight }, { "down", &s_keyDown },
    };
    for (const auto& entry : s_names)
    {
        if (strcmp(entry.name, name) == 0)
        {
            Window::pushKey(entry.key->vk, entry.key->scancode, down, down ? entry.key->character : 0);
            return;
        }
    }
}

void Gamepad::updateKeys()
{
    pf::LockGuard lock(s_keysMutex);
    InputScript::update();
    typeText();
    if (s_kbPoll == 0)
    {
        GamepadState state = readPad();
        const x86::reg64 triangle = x86::reg64(1) << PadTriangle;
        if (!imeOwnsPad() && (state.buttons & triangle) && !(s_state.buttons & triangle) && pf::ime::requestOpen())
            releaseKeys();
        if (imeOwnsPad())
        {
            // Follow the pad without posting keys, so buttons held when the dialog closes post no key press.
            s_state = state;
            return;
        }
        axisKeys(s_state.axes[0], state.axes[0], s_keyLeft, s_keyRight);
        axisKeys(s_state.axes[1], state.axes[1], s_keyUp, s_keyDown);
        for (x86::reg32 button = 0; button < PadUp; ++button)
        {
            x86::reg64 mask = x86::reg64(1) << button;
            if (buttonHasKey(button) && (s_state.buttons & mask) != (state.buttons & mask))
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
        releaseKeys();
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
