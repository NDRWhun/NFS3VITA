#include <lib/file.h>
#include <lib/registry.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <fpu.h>
#include <mmx.h>
#include <nfs3hp.h>
#include <nfs3hp_layout.h>
#include <winapi/kernel32.h>
#include "fog.h"
#include "gamespeed.h"
#include "loadingscreen.h"
#include "mastertimer.h"
#include "menulists.h"
#include "settings.h"
#include "thrashdraw.h"
#include "videomodes.h"
#include "widescreen.h"
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <sys/stat.h>
#include <cstdio>

// Process parameters picked up by the VitaSDK runtime.
extern "C" {
int          _newlib_heap_size_user      = 64 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize  = 4 * 1024 * 1024;
}

// Game data, settings and log.
#define DATA_DIRECTORY "ux0:data/nfs3vita"

static const char* s_dataDirectory = DATA_DIRECTORY "/";
static const char* s_logFile = DATA_DIRECTORY "/nfs3vita.log";
static const char* s_installWin = DATA_DIRECTORY "/install.win";

// The retail installer wrote install.win; the game refuses to start without it.
// One path per line (index = meaning, all read by nfs3.exe):
//  0 language        1 "remote" flag   2 temp dir       3 tracks          4 unused
//  5 carmodel        6 render/pc       7 dashhud        8 audio/pc        9 audio/sfx
// 10-14 speech per language (english german french spanish italian)
// 15 fedata/art     16 fedata/text    17 fedata/text   18 fedata/save    19 fedata/stats
// 20 fedata/config  21 unused         22 art/slides    23 art/track      24 art/showcase
// 25 fedata/movies
// Drive-letter paths resolve to the data directory.
static void ensureInstallWin()
{
    struct stat st;
    if (stat(s_installWin, &st) == 0)
        return;
    static const char content[] =
        "english\r\n"
        "local\r\n"
        "C:\\\r\n"
        "C:\\gamedata\\tracks\\\r\n"
        "C:\\\r\n"
        "C:\\gamedata\\carmodel\\\r\n"
        "C:\\gamedata\\render\\pc\\\r\n"
        "C:\\gamedata\\dashhud\\\r\n"
        "C:\\gamedata\\audio\\pc\\\r\n"
        "C:\\gamedata\\audio\\sfx\\\r\n"
        "C:\\gamedata\\audio\\speech\\english\\\r\n"
        "C:\\gamedata\\audio\\speech\\english\\\r\n"
        "C:\\gamedata\\audio\\speech\\english\\\r\n"
        "C:\\gamedata\\audio\\speech\\english\\\r\n"
        "C:\\gamedata\\audio\\speech\\english\\\r\n"
        "C:\\fedata\\art\\\r\n"
        "C:\\fedata\\text\\\r\n"
        "C:\\fedata\\text\\\r\n"
        "C:\\fedata\\save\\\r\n"
        "C:\\fedata\\stats\\\r\n"
        "C:\\fedata\\config\\\r\n"
        "C:\\\r\n"
        "C:\\fedata\\art\\slides\\\r\n"
        "C:\\fedata\\art\\track\\\r\n"
        "C:\\fedata\\art\\showcase\\\r\n"
        "C:\\fedata\\movies\\\r\n";
    FILE* file = fopen(s_installWin, "wb");
    if (!file)
    {
        pf::logError("cannot create %s", s_installWin);
        return;
    }
    fwrite(content, 1, sizeof(content) - 1, file);
    fclose(file);
    pf::logInfo("created default %s", s_installWin);
}

// The CD check probes "<drive>:NFS3.EXE"; drive paths resolve to the data directory.
static void ensureCdMarker()
{
    static const char* marker = DATA_DIRECTORY "/nfs3.exe";
    struct stat st;
    if (stat(marker, &st) == 0)
        return;
    FILE* file = fopen(marker, "wb");
    if (file)
    {
        fclose(file);
        pf::logInfo("created CD marker %s", marker);
    }
}

// The original exe reads its renderer from the registry; the port has the Voodoo 2 driver only.
static void addRegistryKeys(win32::WinApplication& app)
{
    static const char* key = "SOFTWARE\\Electronic Arts\\Need For Speed III";
    app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, key, "3D Device Description", new win32::RegistryValue("3Dfx Voodoo 2"));
    app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, key, "3D Card", new win32::RegistryValue("3Dfx Voodoo 2"));
    app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, key, "Thrash Driver", new win32::RegistryValue("voodoo2"));
    app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, key, "Group", new win32::RegistryValue("3Dfx"));
    app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, key, "D3D Device", new win32::RegistryValue(x86::reg32(0)));
    app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, key, "Triple Buffer", new win32::RegistryValue(x86::reg32(0)));
    app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, key, "Hardware Acceleration", new win32::RegistryValue(x86::reg32(1)));
}

int main(int, char**)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    mkdir("ux0:data", 0777);
    mkdir(DATA_DIRECTORY, 0777);
    mkdir(DATA_DIRECTORY "/carspecs", 0777); // player car setups written by the game
    pf::logInit(s_logFile);
    pf::logInfo("NFS3VITA starting");
    x86::mmxSelfTest();
    x86::fpuSelfTest();
    pf::Config::load(DATA_DIRECTORY "/nfs3vita.cfg");
    pf::perf::init(pf::Config::getBool("perf_log", false));

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    // The Modern Patch reads nfs3.ini and drivers\<ThrashDriver>\thrash.ini (installed with its files) instead of
    // install.win, the CD and the registry.
    const bool modernPatch = nfs3hp::layout::binaries == nfs3hp::layout::Binaries::ModernPatch161;
    pf::logInfo("binaries: %s", modernPatch ? "Modern Patch 1.6.1" : "original");
    if (!modernPatch)
    {
        ensureInstallWin();
        ensureCdMarker();
    }
    // The Modern Patch races at 960x544, so only its 640x480 frontend is stretched; the original exe's race HUD is
    // 4:3 and would be stretched too, so it stays pillarboxed unless asked for.
    const bool stretch = pf::Config::getBool("stretch", modernPatch);
    pf::gxm::Device::get().setStretch(stretch);
    nfs3hp::loadingscreen::setStretch(modernPatch && stretch);
    if (!pf::gxm::Device::get().init())
        pf::fatal("graphics initialisation failed");

    win32::File::setDataDirectory(s_dataDirectory);
    win32::File::setCdDirectory(s_dataDirectory);
    {
        nfs3hp::Application app("nfs3.exe");
        win32::kernel32::setResourceSection(nfs3hp::layout::imageBase, nfs3hp::layout::resourceSection);
        if (modernPatch)
        {
            // The patch lays out 16:9 itself and has the option lists the original lacks.
            nfs3hp::videomodes::install(app);
        }
        else
        {
            addRegistryKeys(app);
            if (pf::Config::getBool("widescreen", true))
                nfs3hp::widescreen::install(app);
            else
                pf::logInfo("widescreen: off");
            nfs3hp::menulists::install(app);
        }
        if (pf::perf::enabled())
            nfs3hp::gamespeed::install(app);
        nfs3hp::fog::install(app);
        nfs3hp::thrashdraw::install(app);
        nfs3hp::settings::install(app);
        if (pf::Config::getBool("native_timer", true))
            nfs3hp::mastertimer::install(app);
        else
            pf::logInfo("master timer: guest tick callback");
        app.execute();
    }
    pf::logInfo("NFS3VITA exiting");
    pf::gxm::Device::get().shutdown();
    pf::logShutdown();
    sceKernelExitProcess(0);
    return 0;
}
