#ifndef NFS3HP_MASTERTIMER_H_
#define NFS3HP_MASTERTIMER_H_

namespace win32 { class WinApplication; }

namespace nfs3hp { namespace mastertimer {

// Runs the 128 Hz master timer callback (sub_48ed30) natively on the timer thread instead of as guest code under
// the execution lock. Call after the Application is constructed and before it executes.
void install(win32::WinApplication& app);

}}

#endif
