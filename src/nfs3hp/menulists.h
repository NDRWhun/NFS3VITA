#ifndef NFS3HP_MENULISTS_H_
#define NFS3HP_MENULISTS_H_

namespace win32 { class WinApplication; }

namespace nfs3hp { namespace menulists {

// Adds the option lists that the staged (Modern Patch) frontend menus name but this nfs3.exe lacks. Call after
// the Application is constructed and before it executes.
void install(win32::WinApplication& app);

}}

#endif
