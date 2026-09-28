#ifndef LIB_WINDOW_H_
#define LIB_WINDOW_H_

#include <lib/winapp.h>
#include <winapi/types.h>

namespace win32
{

class WindowClass : public GenericResource
{
    friend class Window;
public:
    WindowClass(x86::reg32 windowProc);
    ~WindowClass();

private:
    x86::reg32 m_wndProc;
};

// The single game window; owns the Win32 message queue fed by the input pump.
class Window : public GenericResource
{
    friend class Renderer;
public:
    Window(const char* title, int x, int y, int w, int h);
    ~Window();

    static x86::reg32 getMessage(const x86::CPU& cpu, MSG* result, Window* window, x86::reg32 filterMin, x86::reg32 msgMax);
    static x86::reg32 postMessage(x86::reg32 hWnd, x86::reg32 message, x86::reg32 wParam, x86::reg32 lParam);
    static x86::reg32 getMessageHandler();

    // Input pump entry points.
    static void pushKey(x86::reg32 virtualKey, x86::reg8 scancode, bool down, x86::reg32 charCode);
    // WM_CHAR without a key message, as for IME text.
    static void pushChar(x86::reg32 charCode);
    // True while a posted key or character message has not been returned by getMessage yet.
    static bool inputPending();
    static void postQuit();
    static void wake();

private:
    int m_width;
    int m_height;
};

}

#endif
