#ifndef PLATFORM_IME_H_
#define PLATFORM_IME_H_

#include <cstddef>
#include <cstdint>

struct SceGxmSyncObject;

namespace pf { namespace ime {

// Longest frontend text field; player names take 10 and the game drops characters past a field's limit.
const uint32_t MaxTextLength = 29;

// Input side, any thread.
// Asks the render thread to open the system IME dialog; false while a dialog is open or its text unread.
bool requestOpen();
// True from requestOpen() until the dialog has closed and its text has been taken.
bool active();
// Copies the confirmed text as printable ASCII; true once per confirmed dialog.
bool takeText(char* text, size_t size);

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
