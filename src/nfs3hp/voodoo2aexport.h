#ifndef NFS3HP_VOODOO2AEXPORT_H_
#define NFS3HP_VOODOO2AEXPORT_H_

#include <voodoo2a.h>
#include <lib/library.h>
#include <platform/log.h>

namespace nfs3hp {

// Pseudo-address of a voodoo2a export; fatal if there is none.
inline x86::reg32 voodoo2aExport(const char* name)
{
    const x86::reg32 address = voodoo2a::s_registry->find(name);
    if (!address)
        pf::fatal("voodoo2a has no export %s", name);
    return address;
}

}

#endif
