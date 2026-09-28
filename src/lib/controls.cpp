#include <lib/controls.h>
#include <psp2/ctrl.h>
#include <cstring>
#include <iterator>

namespace win32 { namespace controls
{

const char* const JoystickName       = "Vita Gamepad";
const x86::reg32  JoystickDeviceType = 0x0404;

// ---- Layout ----

// DirectInput buttons, numbered as the game's gamepad defaults (sub_490cb0) bind them. config.dat
// stores bindings by button number, so changing an entry moves that action to another Vita input.
static const Input s_joystickButtons[] =
{
    R,                  // 0  accelerate
    L,                  // 1  brake / reverse
    Square,             // 2  handbrake
    DpadUp,             // 3  shift up
    DpadDown,           // 4  shift down
    Triangle,           // 5  camera view
    Cross,              // 6  horn (siren as cop)
    RightStickLeft,     // 7  reset car
    RightStickDown,     // 8  spike strip (cop)
    RightStickUp,       // 9  lights
    RightStickRight,    // 10 not bound by the defaults
};

struct KeyBinding
{
    Input input;
    Key   key;
};

static const KeyBinding s_menuKeys[] =
{
    { Cross,          KeyEnter  },
    { Start,          KeyEnter  },
    { Circle,         KeyEscape },
    { Select,         KeyEscape },
    { Square,         KeySpace  },
    { L,              KeySpace  },
    { R,              KeySpace  },
    { DpadUp,         KeyUp     },
    { DpadDown,       KeyDown   },
    { DpadLeft,       KeyLeft   },
    { DpadRight,      KeyRight  },
    { LeftStickUp,    KeyUp     },
    { LeftStickDown,  KeyDown   },
    { LeftStickLeft,  KeyLeft   },
    { LeftStickRight, KeyRight  },
};

// Race actions the game only reads from the keyboard (fixed keys set in sub_48f970; look behind is
// key B after the gamepad defaults), plus Enter and arrows for the pause menu.
static const KeyBinding s_raceKeys[] =
{
    { Start,          KeyEscape },  // pause
    { Select,         KeyTab    },  // map on/off
    { Circle,         KeyB      },  // look behind
    { Cross,          KeyEnter  },
    { DpadUp,         KeyUp     },
    { DpadDown,       KeyDown   },
    { DpadLeft,       KeyLeft   },
    { DpadRight,      KeyRight  },
    { LeftStickUp,    KeyUp     },
    { LeftStickDown,  KeyDown   },
    { LeftStickLeft,  KeyLeft   },
    { LeftStickRight, KeyRight  },
};

// Left stick shaping, in raw units from the centre (0..127): values up to the dead zone read 0,
// values from the saturation read full; in between y = linear * x + (1 - linear) * x^3.
static const int   s_stickDeadZone   = 12;
static const int   s_stickSaturation = 120;
static const float s_stickLinear     = 0.6f;

// Deflection that turns a stick direction on: left stick per axis (menu arrows), right stick
// along its dominant axis (buttons).
static const int s_leftStickDigital  = 117;
static const int s_rightStickDigital = 64;

// ---- Keys ----

struct NamedKey
{
    const char* name;
    KeyCode     code;
};

// Order of enum Key.
static const NamedKey s_keys[] =
{
    { "enter",  { 0x0D, 0x1C, 0x0D } },
    { "escape", { 0x1B, 0x01, 0x1B } },
    { "space",  { 0x20, 0x39, 0x20 } },
    { "left",   { 0x25, 0x4B, 0 } },
    { "up",     { 0x26, 0x48, 0 } },
    { "right",  { 0x27, 0x4D, 0 } },
    { "down",   { 0x28, 0x50, 0 } },
    { "tab",    { 0x09, 0x0F, 0x09 } },
    { "b",      { 0x42, 0x30, 'b' } },
    { "delete", { 0x2E, 0x53, 0 } },
};
static_assert(std::size(s_keys) == KeyCount, "s_keys must list every Key");
static_assert(std::size(s_joystickButtons) <= 32, "DIJOYSTATE has 32 buttons");

// ---- Pad ----

struct PadButton
{
    uint32_t mask;
    Input    input;
};

static const PadButton s_padButtons[] =
{
    { SCE_CTRL_CROSS,    Cross     },
    { SCE_CTRL_CIRCLE,   Circle    },
    { SCE_CTRL_SQUARE,   Square    },
    { SCE_CTRL_TRIANGLE, Triangle  },
    { SCE_CTRL_LTRIGGER, L         },
    { SCE_CTRL_RTRIGGER, R         },
    { SCE_CTRL_SELECT,   Select    },
    { SCE_CTRL_START,    Start     },
    { SCE_CTRL_UP,       DpadUp    },
    { SCE_CTRL_DOWN,     DpadDown  },
    { SCE_CTRL_LEFT,     DpadLeft  },
    { SCE_CTRL_RIGHT,    DpadRight },
};

// Stick position relative to the centre, -128..127.
static int centred(unsigned char raw)
{
    return int(raw) - 128;
}

static x86::sreg16 shapeStick(int value)
{
    const int magnitude = value < 0 ? -value : value;
    if (magnitude <= s_stickDeadZone)
        return 0;
    float x = float(magnitude - s_stickDeadZone) / float(s_stickSaturation - s_stickDeadZone);
    if (x > 1.0f)
        x = 1.0f;
    const float y = s_stickLinear * x + (1.0f - s_stickLinear) * x * x * x;
    const int result = int(y * 32767.0f + 0.5f);
    return x86::sreg16(value < 0 ? -result : result);
}

PadState readPad()
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    PadState state;
    state.inputs = 0;
    for (const PadButton& button : s_padButtons)
    {
        if (pad.buttons & button.mask)
            state.inputs |= bit(button.input);
    }

    const int lx = centred(pad.lx);
    const int ly = centred(pad.ly);
    if (ly < -s_leftStickDigital) state.inputs |= bit(LeftStickUp);
    if (ly >  s_leftStickDigital) state.inputs |= bit(LeftStickDown);
    if (lx < -s_leftStickDigital) state.inputs |= bit(LeftStickLeft);
    if (lx >  s_leftStickDigital) state.inputs |= bit(LeftStickRight);

    const int rx = centred(pad.rx);
    const int ry = centred(pad.ry);
    const int ax = rx < 0 ? -rx : rx;
    const int ay = ry < 0 ? -ry : ry;
    if (ay > s_rightStickDigital && ay >= ax)
        state.inputs |= bit(ry < 0 ? RightStickUp : RightStickDown);
    else if (ax > s_rightStickDigital && ax > ay)
        state.inputs |= bit(rx < 0 ? RightStickLeft : RightStickRight);

    state.stickX = shapeStick(lx);
    state.stickY = shapeStick(ly);
    return state;
}

// ---- Joystick ----

x86::reg32 joystickButtonCount()
{
    return x86::reg32(std::size(s_joystickButtons));
}

x86::sreg16 joystickAxis(const PadState& pad, JoystickAxis axis)
{
    if (axis == AxisY)
        return pad.stickY;
    // The d-pad steers at full lock and overrides the stick.
    const bool left = (pad.inputs & bit(DpadLeft)) != 0;
    const bool right = (pad.inputs & bit(DpadRight)) != 0;
    if (left != right)
        return left ? -32767 : 32767;
    return pad.stickX;
}

uint32_t joystickButtons(const PadState& pad)
{
    uint32_t buttons = 0;
    for (x86::reg32 button = 0; button < joystickButtonCount(); ++button)
    {
        if (pad.inputs & bit(s_joystickButtons[button]))
            buttons |= uint32_t(1) << button;
    }
    return buttons;
}

// ---- Keys ----

const KeyCode& keyCode(Key key)
{
    return s_keys[key].code;
}

const KeyCode* keyByName(const char* name)
{
    for (const NamedKey& key : s_keys)
    {
        if (strcmp(key.name, name) == 0)
            return &key.code;
    }
    return nullptr;
}

template< size_t N >
static uint32_t keysOf(const KeyBinding (&table)[N], uint32_t inputs)
{
    uint32_t keys = 0;
    for (const KeyBinding& binding : table)
    {
        if (inputs & bit(binding.input))
            keys |= uint32_t(1) << binding.key;
    }
    return keys;
}

uint32_t heldKeys(uint32_t inputs, Mode mode)
{
    return mode == RaceMode ? keysOf(s_raceKeys, inputs) : keysOf(s_menuKeys, inputs);
}

}}
