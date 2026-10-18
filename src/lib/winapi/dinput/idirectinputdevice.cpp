#include <winapi/dinput/idirectinputdevice.h>
#include <winapi/wrapper.h>
#include <lib/controls.h>
#include <lib/gamepad.h>
#include <cstddef>
#include <cstdio>


namespace win32 { namespace dinput
{


x86::reg32 IDirectInputDevice::s_iDirectInputDeviceVtable = 0;

static const HRESULT DI_OK                = 0;
static const HRESULT DIERR_UNSUPPORTED    = 0x80004001;
static const HRESULT DIERR_OBJECTNOTFOUND = 0x80070002;
static const HRESULT DIERR_INVALIDPARAM   = 0x80070057;

// MAKEDIPROP(n): the property "GUID" argument is the number itself.
enum DirectInputProperty
{
    DIPROP_RANGE      = 4,
    DIPROP_DEADZONE   = 5,
    DIPROP_SATURATION = 6
};

enum DirectInputObjectType
{
    DIDFT_ABSAXIS   = 0x00000002,
    DIDFT_AXIS      = 0x00000003,
    DIDFT_PSHBUTTON = 0x00000004,
    DIDFT_BUTTON    = 0x0000000C
};

static const x86::reg32 s_propertyUnits = 10000;   // dead zone and saturation: 10000 = full deflection
static const LONG       s_absentAxis    = 32767;   // centre of the default range 0..65535

enum DirectInutDeviceForceFeedBackCommand
{
    DISFFC_RESET            = 0x00000001,
    DISFFC_STOPALL          = 0x00000002,
    DISFFC_PAUSE            = 0x00000004,
    DISFFC_CONTINUE         = 0x00000008,
    DISFFC_SETACTUATORSON   = 0x00000010,
    DISFFC_SETACTUATORSOFF  = 0x00000020
};

enum DirectInutDeviceFlags
{
    DIDC_ATTACHED           = 0x00000001,
    DIDC_POLLEDDEVICE       = 0x00000002,
    DIDC_EMULATED           = 0x00000004,
    DIDC_POLLEDDATAFORMAT   = 0x00000008,
    DIDC_FORCEFEEDBACK      = 0x00000100,
    DIDC_FFATTACK           = 0x00000200,
    DIDC_FFFADE             = 0x00000400,
    DIDC_SATURATION         = 0x00000800,
    DIDC_POSNEGCOEFFICIENTS = 0x00001000,
    DIDC_POSNEGSATURATION   = 0x00002000,
    DIDC_DEADBAND           = 0x00004000,
    DIDC_STARTDELAY         = 0x00008000,
    DIDC_ALIAS              = 0x00010000,
    DIDC_PHANTOM            = 0x00020000,
    DIDC_HIDDEN             = 0x00040000
};

enum DirectInputDeviceHow
{
    DIPH_DEVICE     = 0,
    DIPH_BYOFFSET   = 1,
    DIPH_BYID       = 2,
    DIPH_BYUSAGE    = 3
};

enum DirectInputDeviceForceFeedbackState
{
    DIGFFS_EMPTY           = 0x00000001,
    DIGFFS_STOPPED         = 0x00000002,
    DIGFFS_PAUSED          = 0x00000004,
    DIGFFS_ACTUATORSON     = 0x00000010,
    DIGFFS_ACTUATORSOFF    = 0x00000020,
    DIGFFS_POWERON         = 0x00000040,
    DIGFFS_POWEROFF        = 0x00000080,
    DIGFFS_SAFETYSWITCHON  = 0x00000100,
    DIGFFS_SAFETYSWITCHOFF = 0x00000200,
    DIGFFS_USERFFSWITCHON  = 0x00000400,
    DIGFFS_USERFFSWITCHOFF = 0x00000800,
    DIGFFS_DEVICELOST      = 0x80000000
};

struct DIJOYSTATE
{
    LONG lX;
    LONG lY;
    LONG lZ;
    LONG lRx;
    LONG lRy;
    LONG lRz;
    LONG rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE rgbButtons[32];
};

struct IDirectInputDevice::DIPROPHEADER
{
    DWORD dwSize;
    DWORD dwHeaderSize;
    DWORD dwObj;
    DWORD dwHow;
};

struct IDirectInputDevice::DIPROPRANGE
{
    IDirectInputDevice::DIPROPHEADER diph;
    LONG lMin;
    LONG lMax;
};

struct IDirectInputDevice::DIPROPPOINTER
{
    IDirectInputDevice::DIPROPHEADER    diph;
    Packed<UINT>    uData;
};

struct IDirectInputDevice::DIPROPDWORD
{
    IDirectInputDevice::DIPROPHEADER diph;
    DWORD dwData;
};

// Joystick axis addressed by dwObj/dwHow, or -1.
static int findAxis(DWORD obj, DWORD how)
{
    x86::reg32 axis;
    if (how == DIPH_BYOFFSET && obj % sizeof(LONG) == 0)
        axis = obj / sizeof(LONG);
    else if (how == DIPH_BYID && (obj & DIDFT_AXIS))
        axis = (obj >> 8) & 0xffff;
    else
        return -1;
    return axis < controls::JoystickAxisCount ? int(axis) : -1;
}

// Joystick button addressed by dwObj/dwHow, or -1.
static int findButton(DWORD obj, DWORD how)
{
    const DWORD firstOffset = offsetof(DIJOYSTATE, rgbButtons);
    x86::reg32 button;
    if (how == DIPH_BYOFFSET && obj >= firstOffset)
        button = obj - firstOffset;
    else if (how == DIPH_BYID && (obj & DIDFT_BUTTON))
        button = (obj >> 8) & 0xffff;
    else
        return -1;
    return button < controls::joystickButtonCount() ? int(button) : -1;
}

// Axes [first, last) a property header addresses; false if the device has no such object.
static bool propertyAxes(DWORD obj, DWORD how, x86::reg32& first, x86::reg32& last)
{
    if (how == DIPH_DEVICE)
    {
        first = 0;
        last = controls::JoystickAxisCount;
        return obj == 0;
    }
    const int axis = findAxis(obj, how);
    first = x86::reg32(axis);
    last = first + 1;
    return axis >= 0;
}

static bool isAxisProperty(x86::reg32 property)
{
    return property == DIPROP_RANGE || property == DIPROP_DEADZONE || property == DIPROP_SATURATION;
}

// DirectInput axis semantics: dead zone and saturation are fractions of the distance from the
// centre (10000 = full deflection); the result is scaled to the axis range.
static LONG axisPosition(const Gamepad::AxisProperties& properties, x86::sreg16 value)
{
    const int64_t full = 32767;
    int64_t magnitude = value < 0 ? -int64_t(value) : int64_t(value);
    if (magnitude > full)
        magnitude = full;
    const int64_t deadZone = int64_t(properties.deadZone) * full / s_propertyUnits;
    const int64_t saturation = int64_t(properties.saturation) * full / s_propertyUnits;
    if (magnitude <= deadZone)
        magnitude = 0;
    else if (magnitude >= saturation)
        magnitude = full;
    else
        magnitude = (magnitude - deadZone) * full / (saturation - deadZone);
    const int64_t position = value < 0 ? full - magnitude : full + magnitude;
    const int64_t min = properties.min;
    const int64_t max = properties.max;
    return LONG(min + position * (max - min) / (2 * full));
}

DEFINE_GUID(IID_IDirectInputDevice2A,	0x5944E682,0xC92E,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);

IDirectInputDevice::IDirectInputDevice(Input* input)
    :   IUnknown(IID_IDirectInputDevice2A)
{
    m_vtable = s_iDirectInputDeviceVtable;
    m_resource = input;
}

HRESULT IDirectInputDevice::GetCapabilities(WinApplication* app, x86::CPU& cpu,
                                            LPDIDEVCAPS lpDIDevCaps)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_ASSERT(lpDIDevCaps->dwSize == sizeof(DIDEVCAPS));
    memset(lpDIDevCaps, 0, sizeof(DIDEVCAPS));
    lpDIDevCaps->dwSize = sizeof(DIDEVCAPS);
    lpDIDevCaps->dwFlags = DIDC_ATTACHED | DIDC_POLLEDDATAFORMAT /* | DIDC_FORCEFEEDBACK */;
    lpDIDevCaps->dwDevType = dynamic_cast<Gamepad*>(m_resource) ? controls::JoystickDeviceType : 4;
    lpDIDevCaps->dwButtons = dynamic_cast<Input*>(m_resource)->getButtonCount();
    lpDIDevCaps->dwAxes = dynamic_cast<Input*>(m_resource)->getAxesCount();
    return 0;
}

HRESULT IDirectInputDevice::EnumObjects(WinApplication* app, x86::CPU& cpu,
                                        LPDIENUMDEVICEOBJECTSCALLBACK lpCallback, x86::reg32 pvRef, DWORD dwFlags)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpCallback);
    NFS2_USE(pvRef);
    NFS2_USE(dwFlags);
    NFS2_ASSERT(false);
    return 1;
}

// Joystick axis range, dead zone and saturation; the device-wide form reports axis 0.
HRESULT IDirectInputDevice::GetProperty(WinApplication* app, x86::CPU& cpu,
                                        Packed<const GUID> rguidProp, LPDIPROPHEADER pdiph)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    Gamepad* gamepad = dynamic_cast<Gamepad*>(m_resource);
    const x86::reg32 property = rguidProp;
    if (!gamepad || !isAxisProperty(property))
        return DIERR_UNSUPPORTED;
    const DWORD size = property == DIPROP_RANGE ? sizeof(DIPROPRANGE) : sizeof(DIPROPDWORD);
    if (!pdiph || pdiph->dwHeaderSize != sizeof(DIPROPHEADER) || pdiph->dwSize != size)
        return DIERR_INVALIDPARAM;
    x86::reg32 first, last;
    if (!propertyAxes(pdiph->dwObj, pdiph->dwHow, first, last))
        return DIERR_OBJECTNOTFOUND;
    const Gamepad::AxisProperties& axis = gamepad->axisProperties(first);
    if (property == DIPROP_RANGE)
    {
        DIPROPRANGE* range = reinterpret_cast<DIPROPRANGE*>(pdiph);
        range->lMin = axis.min;
        range->lMax = axis.max;
    }
    else
    {
        reinterpret_cast<DIPROPDWORD*>(pdiph)->dwData = property == DIPROP_DEADZONE ? axis.deadZone : axis.saturation;
    }
    return DI_OK;
}

// Joystick axis range, dead zone and saturation; other properties are accepted and ignored.
HRESULT IDirectInputDevice::SetProperty(WinApplication* app, x86::CPU& cpu,
                                        Packed<const GUID> rguidProp, LPCDIPROPHEADER pdiph)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    Gamepad* gamepad = dynamic_cast<Gamepad*>(m_resource);
    const x86::reg32 property = rguidProp;
    if (!gamepad || !isAxisProperty(property))
        return DI_OK;
    const DWORD size = property == DIPROP_RANGE ? sizeof(DIPROPRANGE) : sizeof(DIPROPDWORD);
    if (!pdiph || pdiph->dwHeaderSize != sizeof(DIPROPHEADER) || pdiph->dwSize != size)
        return DIERR_INVALIDPARAM;
    x86::reg32 first, last;
    if (!propertyAxes(pdiph->dwObj, pdiph->dwHow, first, last))
        return DIERR_OBJECTNOTFOUND;
    if (property == DIPROP_RANGE)
    {
        const DIPROPRANGE* range = reinterpret_cast<const DIPROPRANGE*>(pdiph);
        if (range->lMin >= range->lMax)
            return DIERR_INVALIDPARAM;
        for (x86::reg32 axis = first; axis < last; ++axis)
        {
            gamepad->axisProperties(axis).min = range->lMin;
            gamepad->axisProperties(axis).max = range->lMax;
        }
    }
    else
    {
        const DWORD value = reinterpret_cast<const DIPROPDWORD*>(pdiph)->dwData;
        if (value > s_propertyUnits)
            return DIERR_INVALIDPARAM;
        for (x86::reg32 axis = first; axis < last; ++axis)
        {
            Gamepad::AxisProperties& properties = gamepad->axisProperties(axis);
            (property == DIPROP_DEADZONE ? properties.deadZone : properties.saturation) = value;
        }
    }
    return DI_OK;
}

HRESULT IDirectInputDevice::Acquire(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return 0;
}

HRESULT IDirectInputDevice::Unacquire(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return 0;
}

HRESULT IDirectInputDevice::GetDeviceState(WinApplication* app, x86::CPU& cpu,
                                           DWORD cbData, LPVOID lpvData)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    Gamepad* gamepad = dynamic_cast<Gamepad*>(m_resource);
    GamepadState pad;
    memset(&pad, 0, sizeof(pad));
    // A pad read without waiting: the execution lock stays held.
    if (gamepad)
        pad = gamepad->getState();
    memset(lpvData, 0, cbData);
    if (gamepad)
    {
        NFS2_ASSERT(cbData == sizeof(DIJOYSTATE));
        DIJOYSTATE* state = reinterpret_cast<DIJOYSTATE*>(lpvData);
        // Axes the device lacks read as centred, so bindings left on them stay idle.
        auto position = [&](x86::reg32 axis) {
            return axis < controls::JoystickAxisCount ? axisPosition(gamepad->axisProperties(axis), pad.axes[axis]) : s_absentAxis;
        };
        state->lX = position(0);
        state->lY = position(1);
        state->lZ = position(2);
        state->lRx = position(3);
        state->lRy = position(4);
        state->lRz = position(5);
        state->rglSlider[0] = position(6);
        state->rglSlider[1] = position(7);
        for (DWORD& pov : state->rgdwPOV)
            pov = 0xffffffff;
        for (x86::reg32 button = 0; button < controls::joystickButtonCount(); ++button)
            state->rgbButtons[button] = (pad.buttons & (x86::reg64(1) << button)) ? 0x80 : 0x00;
    }
    return DI_OK;
}

HRESULT IDirectInputDevice::GetDeviceData(WinApplication* app, x86::CPU& cpu,
                                          DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(cbObjectData);
    NFS2_USE(rgdod);
    NFS2_USE(pdwInOut);
    NFS2_USE(dwFlags);
    NFS2_ASSERT(dynamic_cast<Mouse*>(m_resource));
    *pdwInOut = 0;
    return 0;
}

HRESULT IDirectInputDevice::SetDataFormat(WinApplication* app, x86::CPU& cpu,
                                          LPCDIDATAFORMAT lpdf)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpdf);
    return 0;
}

HRESULT IDirectInputDevice::SetEventNotification(WinApplication* app, x86::CPU& cpu,
                                                 HANDLE hEvent)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hEvent);
    return 0;
}

HRESULT IDirectInputDevice::SetCooperativeLevel(WinApplication* app, x86::CPU& cpu,
                                                HWND hwnd, DWORD dwFlags)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hwnd);
    NFS2_USE(dwFlags);
    return 0;
}

HRESULT IDirectInputDevice::GetObjectInfo(WinApplication* app, x86::CPU& cpu,
                                          LPDIDEVICEOBJECTINSTANCE pdidoi, DWORD dwObj, DWORD dwHow)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_ASSERT(pdidoi->dwSize == sizeof(DIDEVICEOBJECTINSTANCE));
    memset(pdidoi, 0, pdidoi->dwSize);
    pdidoi->dwSize = sizeof(DIDEVICEOBJECTINSTANCE);
    if (!dynamic_cast<Gamepad*>(m_resource))
        return DI_OK;
    // The game counts the joystick's axes and buttons by probing their DIJOYSTATE offsets.
    const int axis = findAxis(dwObj, dwHow);
    if (axis >= 0)
    {
        pdidoi->dwOfs = DWORD(axis) * sizeof(LONG);
        pdidoi->dwType = DIDFT_ABSAXIS | (DWORD(axis) << 8);
        snprintf(pdidoi->tszName, sizeof(pdidoi->tszName), "%c Axis", 'X' + axis);
        return DI_OK;
    }
    const int button = findButton(dwObj, dwHow);
    if (button >= 0)
    {
        pdidoi->dwOfs = offsetof(DIJOYSTATE, rgbButtons) + DWORD(button);
        pdidoi->dwType = DIDFT_PSHBUTTON | (DWORD(button) << 8);
        snprintf(pdidoi->tszName, sizeof(pdidoi->tszName), "Button %d", button + 1);
        return DI_OK;
    }
    return DIERR_OBJECTNOTFOUND;
}

HRESULT IDirectInputDevice::GetDeviceInfo(WinApplication* app, x86::CPU& cpu,
                                          LPDIDEVICEINSTANCE pdidi)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(pdidi);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::RunControlPanel(WinApplication* app, x86::CPU& cpu,
                                            HWND hwndOwner, DWORD dwFlags)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hwndOwner);
    NFS2_USE(dwFlags);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::Initialize(WinApplication* app, x86::CPU& cpu,
                                       HINSTANCE hinst, DWORD dwVersion, REFGUID rguid)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hinst);
    NFS2_USE(dwVersion);
    NFS2_USE(rguid);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::CreateEffect(WinApplication* app, x86::CPU& cpu,
                                         REFGUID rguid, LPCDIEFFECT lpeff, Packed<IUnknown>* ppdeff, LPUNKNOWN punkOuter)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(rguid);
    NFS2_USE(lpeff);
    NFS2_USE(ppdeff);
    NFS2_USE(punkOuter);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::EnumEffects(WinApplication* app, x86::CPU& cpu,
                                        LPDIENUMEFFECTSCALLBACK lpCallback, LPVOID pvRef, DWORD dwEffType)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpCallback);
    NFS2_USE(pvRef);
    NFS2_USE(dwEffType);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::GetEffectInfo(WinApplication* app, x86::CPU& cpu,
                                          LPDIEffectInfo pdei, REFGUID rguid)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(pdei);
    NFS2_USE(rguid);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::GetForceFeedbackState(WinApplication* app, x86::CPU& cpu,
                                                  LPDWORD pdwOut)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(pdwOut);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::SendForceFeedbackCommand(WinApplication* app, x86::CPU& cpu,
                                                     DWORD dwFlags)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(dwFlags);
    return 0;
}

HRESULT IDirectInputDevice::EnumCreatedEffectObjects(WinApplication* app, x86::CPU& cpu,
                                                     LPDIENUMCREATEDEFFECTOBJECTSCALLBACK lpCallback, LPVOID pvRef, DWORD fl)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpCallback);
    NFS2_USE(pvRef);
    NFS2_USE(fl);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::Escape(WinApplication* app, x86::CPU& cpu,
                                   LPDIEFFESCAPE pesc)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(pesc);
    NFS2_ASSERT(false);
    return 1;
}

HRESULT IDirectInputDevice::Poll(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    // Short host lock that is never held while waiting for the execution lock.
    Gamepad::poll();
    return 0;
}

HRESULT IDirectInputDevice::SendDeviceData(WinApplication* app, x86::CPU& cpu,
                                           DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD fl)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(cbObjectData);
    NFS2_USE(rgdod);
    NFS2_USE(pdwInOut);
    NFS2_USE(fl);
    NFS2_ASSERT(false);
    return 1;
}

x86::reg32 IDirectInputDevice::init(WinApplication *app, x86::reg32 memblockIndex, x86::reg8* memBlock)
{
    x86::reg32* vtable = reinterpret_cast<x86::reg32*>(memBlock);
    app->registerMethod(0x400048, {"GetCapabilities", &Wrapper<decltype(&IDirectInputDevice::GetCapabilities), &IDirectInputDevice::GetCapabilities>::stdcall});
    app->registerMethod(0x400049, {"EnumObjects", &Wrapper<decltype(&IDirectInputDevice::EnumObjects), &IDirectInputDevice::EnumObjects>::stdcall});
    app->registerMethod(0x40004a, {"GetProperty", &Wrapper<decltype(&IDirectInputDevice::GetProperty), &IDirectInputDevice::GetProperty>::stdcall});
    app->registerMethod(0x40004b, {"SetProperty", &Wrapper<decltype(&IDirectInputDevice::SetProperty), &IDirectInputDevice::SetProperty>::stdcall});
    app->registerMethod(0x40004c, {"Acquire", &Wrapper<decltype(&IDirectInputDevice::Acquire), &IDirectInputDevice::Acquire>::stdcall});
    app->registerMethod(0x40004d, {"Unacquire", &Wrapper<decltype(&IDirectInputDevice::Unacquire), &IDirectInputDevice::Unacquire>::stdcall});
    app->registerMethod(0x40004e, {"GetDeviceState", &Wrapper<decltype(&IDirectInputDevice::GetDeviceState), &IDirectInputDevice::GetDeviceState>::stdcall});
    app->registerMethod(0x40004f, {"GetDeviceData", &Wrapper<decltype(&IDirectInputDevice::GetDeviceData), &IDirectInputDevice::GetDeviceData>::stdcall});
    app->registerMethod(0x400050, {"SetDataFormat", &Wrapper<decltype(&IDirectInputDevice::SetDataFormat), &IDirectInputDevice::SetDataFormat>::stdcall});
    app->registerMethod(0x400051, {"SetEventNotification", &Wrapper<decltype(&IDirectInputDevice::SetEventNotification), &IDirectInputDevice::SetEventNotification>::stdcall});
    app->registerMethod(0x400052, {"SetCooperativeLevel", &Wrapper<decltype(&IDirectInputDevice::SetCooperativeLevel), &IDirectInputDevice::SetCooperativeLevel>::stdcall});
    app->registerMethod(0x400053, {"GetObjectInfo", &Wrapper<decltype(&IDirectInputDevice::GetObjectInfo), &IDirectInputDevice::GetObjectInfo>::stdcall});
    app->registerMethod(0x400054, {"GetDeviceInfo", &Wrapper<decltype(&IDirectInputDevice::GetDeviceInfo), &IDirectInputDevice::GetDeviceInfo>::stdcall});
    app->registerMethod(0x400055, {"RunControlPanel", &Wrapper<decltype(&IDirectInputDevice::RunControlPanel), &IDirectInputDevice::RunControlPanel>::stdcall});
    app->registerMethod(0x400056, {"Initialize", &Wrapper<decltype(&IDirectInputDevice::Initialize), &IDirectInputDevice::Initialize>::stdcall});
    app->registerMethod(0x400057, {"CreateEffect", &Wrapper<decltype(&IDirectInputDevice::CreateEffect), &IDirectInputDevice::CreateEffect>::stdcall});
    app->registerMethod(0x400058, {"EnumEffects", &Wrapper<decltype(&IDirectInputDevice::EnumEffects), &IDirectInputDevice::EnumEffects>::stdcall});
    app->registerMethod(0x400059, {"GetEffectInfo", &Wrapper<decltype(&IDirectInputDevice::GetEffectInfo), &IDirectInputDevice::GetEffectInfo>::stdcall});
    app->registerMethod(0x40005a, {"GetForceFeedbackState", &Wrapper<decltype(&IDirectInputDevice::GetForceFeedbackState), &IDirectInputDevice::GetForceFeedbackState>::stdcall});
    app->registerMethod(0x40005b, {"SendForceFeedbackCommand", &Wrapper<decltype(&IDirectInputDevice::SendForceFeedbackCommand), &IDirectInputDevice::SendForceFeedbackCommand>::stdcall});
    app->registerMethod(0x40005c, {"EnumCreatedEffectObjects", &Wrapper<decltype(&IDirectInputDevice::EnumCreatedEffectObjects), &IDirectInputDevice::EnumCreatedEffectObjects>::stdcall});
    app->registerMethod(0x40005d, {"Escape", &Wrapper<decltype(&IDirectInputDevice::Escape), &IDirectInputDevice::Escape>::stdcall});
    app->registerMethod(0x40005e, {"Poll", &Wrapper<decltype(&IDirectInputDevice::Poll), &IDirectInputDevice::Poll>::stdcall});
    app->registerMethod(0x40005f, {"SendDeviceData", &Wrapper<decltype(&IDirectInputDevice::SendDeviceData), &IDirectInputDevice::SendDeviceData>::stdcall});

    s_iDirectInputDeviceVtable = memblockIndex;
    vtable[0] = 0x400000;    // IUnknown_QueryInterface
    vtable[1] = 0x400001;    // IUnknown_AddRef
    vtable[2] = 0x400002;    // IUnknwon_Release
    vtable[3] = 0x400048;    // IDirectInputDevice_GetCapabilities
    vtable[4] = 0x400049;    // IDirectInputDevice_EnumObjects
    vtable[5] = 0x40004a;    // IDirectInputDevice_GetProperty
    vtable[6] = 0x40004b;    // IDirectInputDevice_SetProperty
    vtable[7] = 0x40004c;    // IDirectInputDevice_Acquire
    vtable[8] = 0x40004d;    // IDirectInputDevice_Unacquire
    vtable[9] = 0x40004e;    // IDirectInputDevice_GetDeviceState
    vtable[10] = 0x40004f;    // IDirectInputDevice_GetDeviceData
    vtable[11] = 0x400050;    // IDirectInputDevice_SetDataFormat
    vtable[12] = 0x400051;    // IDirectInputDevice_SetEventNotification
    vtable[13] = 0x400052;    // IDirectInputDevice_SetCooperativeLevel
    vtable[14] = 0x400053;    // IDirectInputDevice_GetObjectInfo
    vtable[15] = 0x400054;    // IDirectInputDevice_GetDeviceInfo
    vtable[16] = 0x400055;    // IDirectInputDevice_RunControlPanel
    vtable[17] = 0x400056;    // IDirectInputDevice_Initialize
    vtable[18] = 0x400057;    // IDirectInputDevice_CreateEffect
    vtable[19] = 0x400058;    // IDirectInputDevice_EnumEffects
    vtable[20] = 0x400059;    // IDirectInputDevice_GetEffectInfo
    vtable[21] = 0x40005a;    // IDirectInputDevice_GetForceFeedbackState
    vtable[22] = 0x40005b;    // IDirectInputDevice_SendForceFeedbackCommand
    vtable[23] = 0x40005c;    // IDirectInputDevice_EnumCreatedEffectObjects
    vtable[24] = 0x40005d;    // IDirectInputDevice_Escape
    vtable[25] = 0x40005e;    // IDirectInputDevice_Poll
    vtable[26] = 0x40005f;    // IDirectInputDevice_SendDeviceData
    return 27 * sizeof(x86::reg32);
}

}}
