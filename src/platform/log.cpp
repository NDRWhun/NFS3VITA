#include <platform/log.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace pf
{

static int      s_logFile = -1;
static SceUID   s_logMutex = -1;

static void writeLine(const char* level, const char* fmt, va_list args)
{
    char buffer[1024];
    int prefix = snprintf(buffer, sizeof(buffer), "[%s] ", level);
    int len = vsnprintf(buffer + prefix, sizeof(buffer) - prefix - 1, fmt, args);
    if (len < 0)
        return;
    len += prefix;
    if (len > int(sizeof(buffer)) - 2)
        len = int(sizeof(buffer)) - 2;
    buffer[len++] = '\n';
    buffer[len] = 0;

    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    sceClibPrintf("%s", buffer);
    if (s_logFile >= 0)
        write(s_logFile, buffer, len);
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
}

void logInit(const char* logFilePath)
{
    s_logMutex = sceKernelCreateMutex("nfs3_log", 0, 0, nullptr);
    if (logFilePath)
        s_logFile = open(logFilePath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

void logShutdown()
{
    if (s_logFile >= 0)
    {
        close(s_logFile);
        s_logFile = -1;
    }
    if (s_logMutex >= 0)
    {
        sceKernelDeleteMutex(s_logMutex);
        s_logMutex = -1;
    }
}

void logDebug(const char* fmt, ...)
{
#ifdef NFS3_LOG_DEBUG
    va_list args;
    va_start(args, fmt);
    writeLine("debug", fmt, args);
    va_end(args);
#else
    (void)fmt;
#endif
}

void logInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("info", fmt, args);
    va_end(args);
}

void logError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("error", fmt, args);
    va_end(args);
}

void fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("fatal", fmt, args);
    va_end(args);
    logShutdown();
    sceKernelExitProcess(1);
    for (;;) {}
}

}
