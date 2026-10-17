#include <lib/window.h>
#include <lib/gamepad.h>
#include <platform/log.h>
#include <platform/msgqueue.h>
#include <atomic>
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
static const x86::reg32 s_idleWaitMs = 100;     // threads not polling still check their terminate flag

static pf::MessageQueue s_queue;
static WindowClass* s_class;
static std::atomic<x86::reg32> s_inputPosted{0};  // MSG_KEY / MSG_CHAR pushed
static std::atomic<x86::reg32> s_inputTaken{0};   // MSG_KEY / MSG_CHAR returned by getMessage
static std::atomic<bool>       s_polling{false};  // a thread in getMessage polls the pad

// Holds the pad polling role while a thread waits in getMessage; one poller is enough.
class PollRole
{
public:
    PollRole() : m_owned(false) {}
    ~PollRole()
    {
        if (m_owned)
            s_polling.store(false);
    }
    bool acquire()
    {
        if (!m_owned)
            m_owned = !s_polling.exchange(true);
        return m_owned;
    }
private:
    bool m_owned;
};

static void pushInput(const pf::Message& message)
{
    s_inputPosted.fetch_add(1);
    s_queue.push(message);
}

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
    PollRole role;
    for (;;)
    {
        const bool poller = role.acquire();
        if (!s_queue.wait(message, poller ? s_inputPollMs : s_idleWaitMs))
        {
            if (cpu.terminate)
                return 0;
            if (poller)
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
            s_inputTaken.fetch_add(1);
            result->message = message.code ? WM_KEYDOWN : WM_KEYUP;
            result->wParam = message.data1;
            result->lParam = message.data2;
            return 1;
        case MSG_CHAR:
            s_inputTaken.fetch_add(1);
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
    pushInput(m);
    if (down && charCode)
    {
        m.type = MSG_CHAR;
        m.code = 0;
        m.data1 = charCode;
        pushInput(m);
    }
}

void Window::pushChar(x86::reg32 charCode)
{
    // lParam: repeat count 1, no scancode.
    pf::Message m = { MSG_CHAR, 0, charCode, 1, 0 };
    pushInput(m);
}

bool Window::inputPending()
{
    return s_inputTaken.load() != s_inputPosted.load();
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
