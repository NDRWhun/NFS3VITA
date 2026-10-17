#ifndef NFS3HP_SETTINGS_H_
#define NFS3HP_SETTINGS_H_

namespace win32 { class WinApplication; }

namespace nfs3hp { namespace settings {

// Game options from nfs3vita.cfg that override the values the game loads from config.dat; options without a key
// keep the in-game choice. Call after the Application is constructed and before it executes.
void install(win32::WinApplication& app);

}}

#endif
