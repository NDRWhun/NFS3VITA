#ifndef NFS3HP_THRASHDRAW_H_
#define NFS3HP_THRASHDRAW_H_

namespace win32 { class WinApplication; }

namespace nfs3hp { namespace thrashdraw {

// Replaces the THRASH triangle exports (drawtri, drawquad, drawtrifan, drawquadmesh) with versions that write the
// game's vertex records straight into the frame's GXM vertex buffer. Call after the Application is constructed and
// before it executes.
void install(win32::WinApplication& app);

}}

#endif
