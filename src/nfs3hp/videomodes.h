#ifndef NFS3HP_VIDEOMODES_H_
#define NFS3HP_VIDEOMODES_H_

namespace win32 { class WinApplication; }

namespace nfs3hp { namespace videomodes {

// Modern Patch builds: offers a single race resolution, 960x544 (in place of voodoo2a's 856x480 Glide mode) or the
// mode nfs3vita.cfg names with resolution=WIDTHxHEIGHT. Call after the Application is constructed and before it
// executes.
void install(win32::WinApplication& app);

}}

#endif
