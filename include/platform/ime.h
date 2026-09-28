#ifndef PLATFORM_IME_H_
#define PLATFORM_IME_H_

#include <cstddef>
#include <cstdint>

struct SceGxmSyncObject;

namespace pf { namespace ime {

// Longest text a dialog accepts; the frontend's edit buffer holds 255 characters.
const uint32_t MaxTextLength = 255;
// Longest title (SCE_IME_DIALOG_MAX_TITLE_LENGTH).
const uint32_t MaxTitleLength = 128;

enum Result
{
    ResultNone,         // no dialog, or its result was taken
    ResultPending,      // requested or shown
    ResultConfirmed,    // closed with Enter or the close button; text available
    ResultCancelled,    // cancelled by the user
    ResultFailed        // not shown, aborted by requestClose(), or lost
};

// Input side, one caller at a time.
// Asks the render thread to show the dialog. Title and text are null-terminated UTF-16, cut to MaxTitleLength and
// maxLength; maxLength is clamped to 1..MaxTextLength. False while a dialog is requested, shown or its result not taken.
bool requestOpen(const char16_t* title, const char16_t* text, uint32_t maxLength);
// Aborts a requested or shown dialog; it finishes with ResultFailed unless it already had a result.
void requestClose();
// Returns a finished dialog's result once and frees the dialog for the next request. Confirmed text is copied to text
// (may be null) as printable ASCII; other results leave it empty.
Result takeResult(char* text, size_t size);

// Render thread.
// True while a dialog is requested or shown and needs frames.
bool pending();
// Outside any scene: starts a requested dialog and closes a finished one. True when it must be drawn now.
bool prepareFrame();
// Draws the dialog over a finished A8B8G8R8 linear display buffer; sync is that buffer's display sync object.
void drawFrame(void* colorSurface, uint32_t width, uint32_t height, uint32_t stride, SceGxmSyncObject* displaySync);
// Once the render thread has stopped.
void shutdown();

}}

#endif
