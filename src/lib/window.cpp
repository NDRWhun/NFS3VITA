#include <lib/window.h>
#include <lib/gamepad.h>
#include <platform/log.h>
#include <platform/msgqueue.h>
#include <cstring>

namespace win32
{

static const x86::reg32 WM_QUIT     = 0x0012;
static const x86::reg32 WM_KEYDOWN  = 0x0100;
static const x86::reg32 WM_KEYUP    = 0x0101;
static const x86::reg32 WM_CHAR     = 0x0102;
static const x86::reg32 WM_USER     = 0x0400;
static const x86::reg32 WM_USER_END = 0xC000;

enum MessageType
{
    MSG_USER,       // WM_USER range, code = message
    MSG_USER1,      // other posted messages, code = message
    MSG_KEY,        // code = down, data1 = virtual key, data2 = lParam
    MSG_CHAR,       // data1 = character, data2 = lParam
    MSG_QUIT,
    MSG_WAKE        // no payload; waiters re-check termination
};

static const x86::reg32 s_inputPollMs = 16;

static pf::MessageQueue s_queue;
static WindowClass* s_class;

WindowClass::WindowClass(x86::reg32 windowProc)
    :   m_wndProc(windowProc)
{
    s_class = this;
}

WindowClass::~WindowClass()
{
}

Window::Window(const char *title, int /*x*/, int /*y*/, int w, int h)
    :   m_width(w)
    ,   m_height(h)
{
    pf::logInfo("window \"%s\" %dx%d", title ? title : "", w, h);
}

Window::~Window()
{
}

x86::reg32 Window::getMessage(const x86::CPU& cpu, MSG* result, Window *window, x86::reg32 filterMin, x86::reg32 msgMax)
{
    NFS2_USE(window);
    NFS2_USE(filterMin);
    NFS2_USE(msgMax);
    memset(result, 0, sizeof(MSG));
    pf::Message message;
    for (;;)
    {
        if (!s_queue.wait(message, s_inputPollMs))
        {
            if (cpu.terminate)
                return 0;
            Gamepad::updateKeys();
            continue;
        }
        if (cpu.terminate)
            return 0;
        switch (message.type)
        {
        case MSG_USER:
        case MSG_USER1:
            result->message = message.code;
            result->wParam = message.data1;
            result->lParam = message.data2;
            result->hWindow = message.window;
            return result->message != WM_QUIT;
        case MSG_KEY:
            result->message = message.code ? WM_KEYDOWN : WM_KEYUP;
            result->wParam = message.data1;
            result->lParam = message.data2;
            return 1;
        case MSG_CHAR:
            result->message = WM_CHAR;
            result->wParam = message.data1;
            result->lParam = message.data2;
            return 1;
        case MSG_QUIT:
            result->message = WM_QUIT;
            return 0;
        case MSG_WAKE:
        default:
            break;
        }
    }
}

x86::reg32 Window::postMessage(x86::reg32 hWnd, x86::reg32 message, x86::reg32 wParam, x86::reg32 lParam)
{
    pf::Message m;
    m.type = (message >= WM_USER && message < WM_USER_END) ? MSG_USER : MSG_USER1;
    m.code = message;
    m.data1 = wParam;
    m.data2 = lParam;
    m.window = hWnd;
    s_queue.push(m);
    return 1;
}

x86::reg32 Window::getMessageHandler()
{
    return s_class->m_wndProc;
}

void Window::pushKey(x86::reg32 virtualKey, x86::reg8 scancode, bool down, x86::reg32 charCode)
{
    pf::Message m;
    m.type = MSG_KEY;
    m.code = down ? 1 : 0;
    m.data1 = virtualKey;
    m.data2 = 1 | (x86::reg32(scancode) << 16);
    m.window = 0;
    s_queue.push(m);
    if (down && charCode)
    {
        m.type = MSG_CHAR;
        m.code = 0;
        m.data1 = charCode;
        s_queue.push(m);
    }
}

void Window::postQuit()
{
    pf::Message m = { MSG_QUIT, 0, 0, 0, 0 };
    s_queue.push(m);
}

void Window::wake()
{
    pf::Message m = { MSG_WAKE, 0, 0, 0, 0 };
    s_queue.push(m);
}

}
