#ifndef NFS3HP_WIDESCREEN_H_
#define NFS3HP_WIDESCREEN_H_

namespace win32 { class WinApplication; }

namespace nfs3hp { namespace widescreen {

// Shows the race world views on the whole display with a wider horizontal field of view (Hor+); everything else
// stays in the 4:3 game area. Call after the Application is constructed and before it executes.
void install(win32::WinApplication& app);

}}

#endif
