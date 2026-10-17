#include <lib/file.h>
#include <lib/inputscript.h>
#include <lib/registry.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#ifdef NFS3_PROFILE
#include <platform/prof.h>
#endif
#include <fpu.h>
#include <mmx.h>
#include <nfs3hp.h>
#include "fog.h"
#include "gamespeed.h"
#include "mastertimer.h"
#include "menulists.h"
#include "settings.h"
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

static const char* s_dataDirectory = "ux0:data/nfs3vita/";
static const char* s_logFile = "ux0:data/nfs3vita/nfs3vita.log";
static const char* s_installWin = "ux0:data/nfs3vita/install.win";

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
    static const char* marker = "ux0:data/nfs3vita/nfs3.exe";
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

int main(int, char**)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    mkdir("ux0:data", 0777);
    mkdir("ux0:data/nfs3vita", 0777);
    mkdir("ux0:data/nfs3vita/carspecs", 0777); // player car setups written by the game
    pf::logInit(s_logFile);
    pf::logInfo("NFS3VITA starting");
    x86::mmxSelfTest();
    x86::fpuSelfTest();
    pf::Config::load("ux0:data/nfs3vita/nfs3vita.cfg");
    pf::perf::init(pf::Config::getBool("perf_log", true));
    win32::InputScript::load("ux0:data/nfs3vita/autoinput.txt");

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    ensureInstallWin();
    ensureCdMarker();
    pf::gxm::Device::get().setFramebuffer16Bgr(pf::Config::getBool("vita3k_bgr565", false));
    if (!pf::gxm::Device::get().init())
        pf::fatal("graphics initialisation failed");

    win32::File::setDataDirectory(s_dataDirectory);
    win32::File::setCdDirectory(s_dataDirectory);
    {
        nfs3hp::Application app("nfs3.exe");
        app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, "SOFTWARE\\Electronic Arts\\Need For Speed III", "3D Device Description", new win32::RegistryValue("3Dfx Voodoo 2"));
        app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, "SOFTWARE\\Electronic Arts\\Need For Speed III", "3D Card", new win32::RegistryValue("3Dfx Voodoo 2"));
        app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, "SOFTWARE\\Electronic Arts\\Need For Speed III", "Thrash Driver", new win32::RegistryValue("voodoo2"));
        app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, "SOFTWARE\\Electronic Arts\\Need For Speed III", "Group", new win32::RegistryValue("3Dfx"));
        app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, "SOFTWARE\\Electronic Arts\\Need For Speed III", "D3D Device", new win32::RegistryValue(x86::reg32(0)));
        app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, "SOFTWARE\\Electronic Arts\\Need For Speed III", "Triple Buffer", new win32::RegistryValue(x86::reg32(0)));
        app.addRegistryKey(win32::HKEY_LOCAL_MACHINE, "SOFTWARE\\Electronic Arts\\Need For Speed III", "Hardware Acceleration", new win32::RegistryValue(x86::reg32(1)));
        if (pf::Config::getBool("widescreen", true))
            nfs3hp::widescreen::install(app);
        else
            pf::logInfo("widescreen: off");
        if (pf::perf::enabled())
            nfs3hp::gamespeed::install(app);
        nfs3hp::fog::install(app);
        nfs3hp::menulists::install(app);
        nfs3hp::settings::install(app);
        if (pf::Config::getBool("native_timer", true))
            nfs3hp::mastertimer::install(app);
        else
            pf::logInfo("master timer: guest tick callback");
#ifdef NFS3_PROFILE
        pf::prof::start(app);
#endif
        app.execute();
#ifdef NFS3_PROFILE
        pf::prof::stop();
#endif
    }
    pf::logInfo("NFS3VITA exiting");
    pf::gxm::Device::get().shutdown();
    pf::logShutdown();
    sceKernelExitProcess(0);
    return 0;
}
