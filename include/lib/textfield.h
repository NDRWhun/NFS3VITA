#ifndef LIB_TEXTFIELD_H_
#define LIB_TEXTFIELD_H_

#include <lib/winapp.h>

namespace win32
{

// Edits the frontend's text-entry popup with the system IME. The dialog opens when a popup appears, with the popup's
// title, the field's text and its length limit. Confirmed text replaces the field and is accepted with Enter; a
// cancelled dialog sends Escape. A popup that closes by itself aborts the dialog.
class TextField
{
public:
    // Game thread, from the Glide present path: it runs the frontend (the only writer of the popup state) and is
    // outside guest code there.
    static void update(WinApplication* app);
    // True from a popup's detection until the dialog's result has been read by the game; any thread.
    static bool ownsPad();
};

}

#endif
