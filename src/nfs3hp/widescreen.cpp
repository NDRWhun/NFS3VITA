#include "widescreen.h"
#include <voodoo2a.h>
#include <lib/gliderenderer.h>
#include <lib/winapp.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <cmath>
#include <cstring>

// The game builds each view's projection and then calls THRASH_clip with the view's clip rectangle (sub_4bed20:
// sub_4dbce0, then sub_4bebe0). The HUD, mirror frame, menus and other 2D phases start with a clip-only call.
// For the race world views the hook scales the horizontal focal length by gameAreaWidth / displayWidth (the image
// is squeezed horizontally) and maps their draws onto the whole display: the centre shows the 4:3 image unchanged
// and the sides show more of the world (Hor+). The game culls in screen space against the clip rectangle, so the
// wider field is culled correctly. Invariant: MapFull <=> the projection globals hold the squeezed values.
// All state is used on the game thread under the execution lock (THRASH_clip is only called by guest code).

namespace nfs3hp { namespace widescreen {

namespace
{

// nfs3.exe guest addresses, see docs/WIDESCREEN.md.
const x86::reg32 ProjectionAngle = 0x564448;    // written first by sub_4fd2b0 on every projection build, read only there
const x86::reg32 FreshMarker     = 0x7fc0de01;  // stored there by every clip; a projection build overwrites it
const x86::reg32 FocalX          = 0x56009c;    // float (w/2)/tan(hfov/2), sub_4bf260
const x86::reg32 HalfWidth       = 0x5600ac;    // float w/2; the zoom camera path of sub_4dbde0 sets FocalX from it after the clip
const x86::reg32 FixedFocalX     = 0x56443c;    // fixed-point focal x, sub_4fd2b0
const x86::reg32 ScreenWidth     = 0x7cda68;    // clamp bounds of sub_4bebe0
const x86::reg32 ScreenHeight    = 0x7cda6c;
const x86::reg32 CurrentMode     = 0x55fec0;    // video mode indices
const x86::reg32 RaceMode        = 0x55fec4;
const x86::reg32 FrontendMode    = 0x55fec8;
const x86::reg32 WorldViews[]    = { 0x7cdb88, 0x7cda80, 0x7cdaa0 };   // main, split screen top and bottom
const x86::reg32 ViewClipRect    = 0x10;        // x, y, w, h in a view struct

struct ClipWindow
{
    int32_t x0, y0, x1, y1;
    bool operator==(const ClipWindow& other) const
    {
        return x0 == other.x0 && y0 == other.y0 && x1 == other.x1 && y1 == other.y1;
    }
};

// Guest words of the projection globals the hook scales.
struct Projection
{
    x86::reg32 focalX, halfWidth, fixedFocalX;
};

win32::MethodPtr    s_originalClip = nullptr;
float               s_scale = 1.0f;         // game area width / display width
bool                s_squeezed = false;     // the projection globals hold squeezed values
Projection          s_original = {};        // values before squeezing
Projection          s_squeezedValues = {};  // values the squeeze stored
bool                s_raceLogged = false;

x86::reg32 word(win32::WinApplication& app, x86::reg32 address)
{
    return app.getMemory<x86::reg32>(address);
}

int32_t signedWord(win32::WinApplication& app, x86::reg32 address)
{
    return int32_t(word(app, address));
}

float toFloat(x86::reg32 value)
{
    float result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

x86::reg32 toWord(float value)
{
    x86::reg32 result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

x86::reg32 scaleFloat(x86::reg32 value, float factor)
{
    return toWord(toFloat(value) * factor);
}

x86::reg32 scaleFixed(x86::reg32 value, float factor)
{
    return x86::reg32(int32_t(lroundf(float(int32_t(value)) * factor)));
}

Projection readProjection(win32::WinApplication& app)
{
    return Projection{ word(app, FocalX), word(app, HalfWidth), word(app, FixedFocalX) };
}

void writeProjection(win32::WinApplication& app, const Projection& projection)
{
    app.getMemory<x86::reg32>(FocalX) = projection.focalX;
    app.getMemory<x86::reg32>(HalfWidth) = projection.halfWidth;
    app.getMemory<x86::reg32>(FixedFocalX) = projection.fixedFocalX;
}

// Only called right after a projection build, so the globals hold the values the game computed.
void squeeze(win32::WinApplication& app)
{
    s_original = readProjection(app);
    s_squeezedValues.focalX = scaleFloat(s_original.focalX, s_scale);
    s_squeezedValues.halfWidth = scaleFloat(s_original.halfWidth, s_scale);
    s_squeezedValues.fixedFocalX = scaleFixed(s_original.fixedFocalX, s_scale);
    writeProjection(app, s_squeezedValues);
    s_squeezed = true;
}

// Restores the values the squeeze stored; a value the game rewrote since (the zoom camera's focal x) is scaled back.
void unsqueeze(win32::WinApplication& app)
{
    const Projection current = readProjection(app);
    const float inverse = 1.0f / s_scale;
    Projection restored;
    restored.focalX = current.focalX == s_squeezedValues.focalX ? s_original.focalX
                                                                : scaleFloat(current.focalX, inverse);
    restored.halfWidth = current.halfWidth == s_squeezedValues.halfWidth ? s_original.halfWidth
                                                                         : scaleFloat(current.halfWidth, inverse);
    restored.fixedFocalX = current.fixedFocalX == s_squeezedValues.fixedFocalX ? s_original.fixedFocalX
                                                                               : scaleFixed(current.fixedFocalX, inverse);
    writeProjection(app, restored);
    s_squeezed = false;
}

// The window THRASH_clip receives for a view, clamped like sub_4bebe0 does.
ClipWindow clipWindow(win32::WinApplication& app, x86::reg32 view)
{
    const int32_t width = signedWord(app, ScreenWidth);
    const int32_t height = signedWord(app, ScreenHeight);
    int32_t x = signedWord(app, view + ViewClipRect);
    int32_t y = signedWord(app, view + ViewClipRect + 4);
    int32_t w = signedWord(app, view + ViewClipRect + 8);
    int32_t h = signedWord(app, view + ViewClipRect + 12);
    x = x < 0 ? 0 : (x < width - 1 ? x : width - 1);
    y = y < 0 ? 0 : (y < height - 1 ? y : height - 1);
    w = w < width - x ? w : width - x;
    h = h < height - y ? h : height - y;
    return ClipWindow{ x, y, x + w, y + h };
}

bool isWorldView(win32::WinApplication& app, const ClipWindow& window)
{
    for (x86::reg32 view : WorldViews)
    {
        if (clipWindow(app, view) == window)
            return true;
    }
    return false;
}

// Replaces _THRASH_clip@16 (voodoo2a sub_a848b0).
void thrashClip(win32::WinApplication* app, x86::CPU& cpu)
{
    // stdcall: return slot at [esp], then x0, y0, x1, y1; the original pops them.
    const ClipWindow window = { signedWord(*app, cpu.esp + 4), signedWord(*app, cpu.esp + 8),
                                signedWord(*app, cpu.esp + 12), signedWord(*app, cpu.esp + 16) };
    const bool fresh = word(*app, ProjectionAngle) != FreshMarker;
    app->getMemory<x86::reg32>(ProjectionAngle) = FreshMarker;
    if (fresh)
        s_squeezed = false;

    // The frontend's 3D view is built at the frontend resolution, races at the race resolution; a race at the
    // frontend resolution cannot be told apart and stays 4:3.
    const x86::reg32 raceMode = word(*app, RaceMode);
    const bool inRace = word(*app, CurrentMode) == raceMode && raceMode != word(*app, FrontendMode);
    const bool world = fresh && inRace && isWorldView(*app, window);
    if (world)
    {
        squeeze(*app);
        if (!s_raceLogged)
        {
            pf::logInfo("widescreen: race at %dx%d, world view %d,%d-%d,%d on the whole display, focal x %.1f -> %.1f",
                        signedWord(*app, ScreenWidth), signedWord(*app, ScreenHeight), window.x0, window.y0,
                        window.x1, window.y1, double(toFloat(s_original.focalX)),
                        double(toFloat(s_squeezedValues.focalX)));
            s_raceLogged = true;
        }
    }
    else if (s_squeezed)
    {
        unsqueeze(*app);
    }
    if (!inRace)
        s_raceLogged = false;

    win32::GlideRenderer::setActiveMapping(s_squeezed ? pf::gxm::MapFull : pf::gxm::MapBox);
    s_originalClip(app, cpu);
}

}

void install(win32::WinApplication& app)
{
    s_scale = float(pf::gxm::Device::get().viewport().w) / float(pf::gxm::DisplayWidth);
    const x86::reg32 clip = (*voodoo2a::s_registry)["_THRASH_clip@16"].first;
    s_originalClip = app.replaceMethod(clip, &thrashClip);
    pf::logInfo("widescreen: on, race world views use the whole display (focal x scale %.4f)", double(s_scale));
}

}}
