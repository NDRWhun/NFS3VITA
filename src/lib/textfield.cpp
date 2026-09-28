#include <lib/textfield.h>
#include <lib/gamepad.h>
#include <lib/window.h>
#include <platform/ime.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <atomic>

namespace win32
{

namespace
{

// Frontend popup of nfs3.exe. 0x444c20 opens the text-entry popup (type 3): it copies the field into the edit buffer
// and highlights button 0 (OK). 0x444f50 handles its keys: Delete empties the buffer, accepted characters are appended
// while under the limit, Enter returns the highlighted button, Escape cancels. 0x445f10 closes any popup.
const x86::reg32 PopupShown     = 0x558b10;    // byte
const x86::reg32 PopupType      = 0x558b14;
const x86::reg32 TitleLineCount = 0x558b1c;
const x86::reg32 TitleLines     = 0x558b20;    // array of string addresses
const x86::reg32 SelectedButton = 0x558b3c;
const x86::reg32 EditLimit      = 0x558b54;
const x86::reg32 EditText       = 0x558b58;    // 256 bytes, null-terminated
const x86::reg32 EditField      = 0x558c58;    // address of the field being edited
const x86::reg32 PopupTypeText  = 3;
const x86::reg32 MaxTitleLines  = 4;
// Key ring written by the window procedure (0x4cc8e0) and read once per frontend pass (0x4cc970).
const x86::reg32 KeyReadIndex   = 0x7f7474;
const x86::reg32 KeyWriteIndex  = 0x7f747c;

// Frames without undelivered input before the next key: the frontend reads one key per pass and drops a key equal to
// the previous one unless a pass without a key came in between.
const uint32_t QuietFramesPerKey = 3;

enum Phase
{
    PhaseIdle,      // no popup, or the shown one has been handled
    PhaseOpening,   // popup found; the dialog is requested once the IME is free
    PhaseEditing,   // dialog requested or shown
    PhaseTyping,    // result being posted: Delete and the text, then Enter; or Escape
    PhaseClosing    // Enter or Escape posted; waits until the game has read it
};

pf::Mutex         s_mutex("nfs3_text_field", false);
std::atomic<bool> s_ownsPad{false};
Phase             s_phase = PhaseIdle;
bool              s_shown;          // a text-entry popup was shown at the last update
x86::reg32        s_field;          // its field address
bool              s_confirmed;      // the dialog returned text, else it was cancelled
bool              s_cleared;        // Delete posted
char              s_text[pf::ime::MaxTextLength + 1];
uint32_t          s_next;           // next character of s_text to post
uint32_t          s_quietFrames;    // consecutive updates with no key waiting in the message queue or the key ring

// Windows-1252, the encoding of the game's text files, as UTF-16; 0 for characters to drop.
char16_t fromWindows1252(x86::reg8 c)
{
    static const char16_t s_upper[32] = {
        0x20ac, 0,      0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
        0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
    };
    if (c < 0x20 || c == 0x7f)
        return 0;
    if (c >= 0x80 && c < 0xa0)
        return s_upper[c - 0x80];
    return char16_t(c);
}

// Appends a null-terminated guest string, up to capacity characters in total.
void appendText(WinApplication* app, x86::reg32 address, char16_t* text, uint32_t& length, uint32_t capacity)
{
    for (; address && length < capacity; ++address)
    {
        const x86::reg8 c = app->getMemory<x86::reg8>(address);
        if (!c)
            break;
        const char16_t unit = fromWindows1252(c);
        if (unit)
            text[length++] = unit;
    }
    text[length] = 0;
}

// Requests the dialog with the popup's title lines, edit text and limit; true once the IME accepted the request.
bool requestDialog(WinApplication* app)
{
    static const char16_t s_separator[] = u" - ";
    char16_t title[pf::ime::MaxTitleLength + 1];
    char16_t text[pf::ime::MaxTextLength + 1];
    uint32_t titleLength = 0;
    title[0] = 0;
    const x86::reg32 lineCount = app->getMemory<x86::reg32>(TitleLineCount);
    const x86::reg32 lines = app->getMemory<x86::reg32>(TitleLines);
    for (x86::reg32 line = 0; line < lineCount && line < MaxTitleLines; ++line)
    {
        for (uint32_t i = 0; titleLength && s_separator[i] && titleLength < pf::ime::MaxTitleLength; ++i)
            title[titleLength++] = s_separator[i];
        appendText(app, app->getMemory<x86::reg32>(lines + line * 4), title, titleLength, pf::ime::MaxTitleLength);
    }

    x86::reg32 limit = app->getMemory<x86::reg32>(EditLimit);
    if (limit > pf::ime::MaxTextLength)
        limit = pf::ime::MaxTextLength;
    uint32_t textLength = 0;
    appendText(app, EditText, text, textLength, limit);
    return pf::ime::requestOpen(title, text, limit);
}

void pressKey(const char* name)
{
    Gamepad::pushNamedKey(name, true);
    Gamepad::pushNamedKey(name, false);
}

void startTyping(bool confirmed)
{
    s_confirmed = confirmed;
    s_cleared = !confirmed;
    s_next = 0;
    s_phase = PhaseTyping;
}

// Posts the next key of the result once the game has read the previous one.
void typeNext(WinApplication* app)
{
    if (s_quietFrames < QuietFramesPerKey)
        return;
    s_quietFrames = 0;
    if (!s_cleared)
    {
        pressKey("delete");
        s_cleared = true;
    }
    else if (s_confirmed && s_text[s_next])
    {
        Window::pushChar(x86::reg8(s_text[s_next++]));
    }
    else if (s_confirmed && x86::reg32(app->getMemory<x86::reg32>(SelectedButton)) != 0)
    {
        // Enter presses the highlighted button; Left moves the highlight towards OK.
        pressKey("left");
    }
    else
    {
        pressKey(s_confirmed ? "enter" : "escape");
        s_phase = PhaseClosing;
    }
}

}

void TextField::update(WinApplication* app)
{
    pf::LockGuard lock(s_mutex);
    const bool shown = x86::reg8(app->getMemory<x86::reg8>(PopupShown)) != 0
        && x86::reg32(app->getMemory<x86::reg32>(PopupType)) == PopupTypeText;
    const x86::reg32 field = shown ? x86::reg32(app->getMemory<x86::reg32>(EditField)) : 0;
    const bool quiet = !Window::inputPending()
        && x86::reg32(app->getMemory<x86::reg32>(KeyReadIndex)) == x86::reg32(app->getMemory<x86::reg32>(KeyWriteIndex));
    s_quietFrames = !quiet ? 0 : s_quietFrames < QuietFramesPerKey ? s_quietFrames + 1 : s_quietFrames;

    if (shown != s_shown || field != s_field)
    {
        // Another popup or none: the current one is dropped, a dialog still open for it is aborted.
        if (s_phase == PhaseEditing)
            pf::ime::requestClose();
        s_phase = shown ? PhaseOpening : PhaseIdle;
        s_shown = shown;
        s_field = field;
        if (shown)
            pf::logInfo("text field 0x%08x opened", field);
    }

    switch (s_phase)
    {
    case PhaseIdle:
        // Drops the result of a dialog aborted for a closed popup.
        pf::ime::takeResult(nullptr, 0);
        break;
    case PhaseOpening:
        pf::ime::takeResult(nullptr, 0);
        if (requestDialog(app))
            s_phase = PhaseEditing;
        break;
    case PhaseEditing:
        switch (pf::ime::takeResult(s_text, sizeof(s_text)))
        {
        case pf::ime::ResultPending:
            break;
        case pf::ime::ResultConfirmed:
            startTyping(true);
            break;
        case pf::ime::ResultCancelled:
            startTyping(false);
            break;
        default:
            // No dialog: the pad edits the popup.
            s_phase = PhaseIdle;
            break;
        }
        break;
    case PhaseTyping:
        typeNext(app);
        break;
    case PhaseClosing:
        if (s_quietFrames >= QuietFramesPerKey)
            s_phase = PhaseIdle;
        break;
    }
    s_ownsPad.store(s_phase != PhaseIdle);
}

bool TextField::ownsPad()
{
    return s_ownsPad.load();
}

}
