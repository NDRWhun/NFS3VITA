#ifndef NFS3HP_FOG_H_
#define NFS3HP_FOG_H_

namespace win32 { class WinApplication; }

namespace nfs3hp { namespace fog {

// Skips the THRASH fog table build, whose result only reaches the Glide fog calls the host ignores. Call after the
// Application is constructed and before it executes.
void install(win32::WinApplication& app);

}}

#endif
