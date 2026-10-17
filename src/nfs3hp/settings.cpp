#include "settings.h"
#include <lib/file.h>
#include <platform/config.h>
#include <platform/log.h>
#include <vector>

// config.dat (fedata/config) is a copy of the guest block 0x6fbb40..0x6fdc30, which sub_472980 reads into a
// buffer and copies over the block. A read filter on that file replaces the words of the options set in
// nfs3vita.cfg, so the game starts with them and the rest of the file is untouched. Level options are indices into
// the in-game list (its order in the menu; 0 is the first entry, the most detailed one).

namespace nfs3hp { namespace settings {

namespace
{

const x86::reg32 ConfigBase = 0x6fbb40;
const char* const ConfigFile = "fedata/config/config.dat";

struct Option
{
    const char* key;
    x86::reg32  address;
    int         count;      // list entries; valid values 0..count-1
    bool        inverted;   // the key's 1 is stored as 0 (traffic: 0 = on)
};

// From the frontend list table at 0x5563e8 (name, labels, variable).
const Option Options[] =
{
    { "traffic",   0x6fd4e8, 2, true  },  // On, Off
    { "oppnumber", 0x6fd4d0, 3, false },
    { "viewlevel", 0x6fbc28, 4, false },
    { "cardetail", 0x6fbc1c, 3, false },
    { "sfxlevel",  0x6fbc30, 3, false },  // High, Low, Off
    { "hrzlevel",  0x6fbc2c, 3, false },  // High, Low, Off
    { "carchrome", 0x6fbc24, 2, false },  // Off, On
};

// Rear-view mirror flags of the per-player blocks (stride 0x1a4): racer, cop, then split screen player 1 racer
// and cop, player 2 racer and cop. The split screen lists only offer Off.
const x86::reg32 Mirrors[] = { 0x6fbc5c, 0x6fbe00, 0x6fbfa4, 0x6fc148, 0x6fc2ec, 0x6fc490 };
const int SinglePlayerMirrors = 2;

struct Override
{
    x86::reg32 offset;  // in config.dat
    x86::reg32 value;
};

// Written by install, read-only afterwards.
std::vector<Override> s_overrides;

void add(x86::reg32 address, x86::reg32 value)
{
    s_overrides.push_back(Override{ address - ConfigBase, value });
}

// Patches the bytes of each override word that fall inside the data read.
void filterConfig(x86::reg32 offset, x86::reg8* data, x86::reg32 length)
{
    for (const Override& entry : s_overrides)
    {
        for (x86::reg32 i = 0; i < 4; ++i)
        {
            const x86::reg32 position = entry.offset + i;
            if (position >= offset && position < offset + length)
                data[position - offset] = x86::reg8(entry.value >> (8 * i));
        }
    }
}

}

void install(win32::WinApplication& app)
{
    NFS2_USE(app);
    for (const Option& option : Options)
    {
        if (!pf::Config::has(option.key))
            continue;
        const int value = pf::Config::getInt(option.key, 0);
        if (value < 0 || value >= option.count)
        {
            pf::logError("settings: %s=%d out of range 0..%d, ignored", option.key, value, option.count - 1);
            continue;
        }
        add(option.address, x86::reg32(option.inverted ? 1 - value : value));
    }
    if (pf::Config::has("mirror"))
    {
        const bool on = pf::Config::getBool("mirror", false);
        const int count = on ? SinglePlayerMirrors : int(sizeof(Mirrors) / sizeof(Mirrors[0]));
        for (int i = 0; i < count; ++i)
            add(Mirrors[i], on ? 1 : 0);
    }
    if (s_overrides.empty())
        return;
    win32::File::setReadFilter(ConfigFile, &filterConfig);
    pf::logInfo("settings: %u config.dat words set from nfs3vita.cfg", unsigned(s_overrides.size()));
}

}}
