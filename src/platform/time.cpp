#include <platform/time.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

namespace pf
{

uint64_t ticksUs()
{
    return sceKernelGetProcessTimeWide();
}

uint32_t ticksMs()
{
    return uint32_t(sceKernelGetProcessTimeWide() / 1000);
}

void sleepMs(uint32_t milliseconds)
{
    // Sleep(0) must still let other guest threads take the execution lock.
    sleepUs(milliseconds ? milliseconds * 1000 : 50);
}

void sleepUs(uint32_t microseconds)
{
    sceKernelDelayThread(microseconds);
}

}
