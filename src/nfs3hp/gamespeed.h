#ifndef NFS3HP_GAMESPEED_H_
#define NFS3HP_GAMESPEED_H_

namespace win32 { class WinApplication; }

namespace nfs3hp { namespace gamespeed {

// Adds a "speed:" line to every perf line during races: race time against wall time, plus the input frame and
// master timer rates that drive it (docs/GAMESPEED.md). Needs perf_log.
void install(win32::WinApplication& app);

}}

#endif
