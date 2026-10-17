#include "menulists.h"
#include <lib/memmap.h>
#include <lib/winapp.h>
#include <platform/log.h>
#include <cstring>

// The frontend finds a menu item's option list by name in the table at 0x5563e8 (20-byte entries: name, label
// word list, two callbacks, variable; a null name ends it; sub_442b90, sub_442bf0, sub_442c80). For a missing
// name sub_442c80 logs "list not found" and calls sub_4f0120, which waits for a key forever: the Graphics, Advanced
// Graphics, Camera and HUD menus of the staged Modern Patch data (graphics, psgraph, advgrph, camera, pscamera,
// hud .mnu) freeze the game. Five entries that no .mnu names and no code looks up get the missing names instead.
// mirrorlevel drives the racer's rear-view mirror (Off, On); the others have no feature in this exe and offer a
// single Off entry on a scratch variable.

namespace nfs3hp { namespace menulists {

namespace
{

const x86::reg32 LabelsOffOn = 0x5561be;    // lang 229, 228: Off, On
const x86::reg32 LabelsOff   = 0x5561d0;    // lang 229: Off
const x86::reg32 Mirror      = 0x6fbc5c;    // racer's rear-view mirror flag (list "mirror")

struct List
{
    x86::reg32  entry;      // reused table entry
    const char* unused;     // its current name, checked before it is replaced
    const char* name;
    bool        mirror;
};

const List Lists[] =
{
    { 0x55658c, "downforce",       "mirrorlevel",   true  },
    { 0x5565a0, "sensitivity",     "fog",           false },
    { 0x5565b4, "brakebias",       "rearcamera",    false },
    { 0x5565c8, "bgearratios",     "hudtypesingle", false },
    { 0x556564, "DownloadWebSite", "hudtypesplit",  false },
};

}

void install(win32::WinApplication& app)
{
    // Names and the scratch variable; freed with the guest memory at shutdown.
    win32::MemMap* block = new win32::MemMap(4096);
    const x86::reg32 scratch = block->getBlockStart();
    app.getMemory<x86::reg32>(scratch) = 0;
    x86::reg32 next = scratch + 4;
    int added = 0;
    for (const List& list : Lists)
    {
        const char* current = &app.getMemory<char>(app.getMemory<x86::reg32>(list.entry));
        if (strcmp(current, list.unused) != 0)
        {
            pf::logError("menu lists: entry 0x%x is \"%s\", not \"%s\"; %s not added", list.entry, current,
                         list.unused, list.name);
            continue;
        }
        strcpy(&app.getMemory<char>(next), list.name);
        app.getMemory<x86::reg32>(list.entry) = next;
        app.getMemory<x86::reg32>(list.entry + 4) = list.mirror ? LabelsOffOn : LabelsOff;
        app.getMemory<x86::reg32>(list.entry + 8) = 0;
        app.getMemory<x86::reg32>(list.entry + 12) = 0;
        app.getMemory<x86::reg32>(list.entry + 16) = list.mirror ? Mirror : scratch;
        next += x86::reg32(strlen(list.name) + 1);
        ++added;
    }
    pf::logInfo("menu lists: %d of %d Modern Patch lists added", added, int(sizeof(Lists) / sizeof(Lists[0])));
}

}}
