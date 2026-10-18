#include <winapi/kernel32.h>
#include <x86.h>
#include <lib/memmap.h>
#include <lib/mutex.h>
#include <lib/event.h>
#include <lib/file.h>
#include <lib/library.h>
#include <lib/timer.h>
#include <lib/window.h>
#include <lib/thread.h>
#include <platform/log.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace win32 { namespace kernel32
{

struct OSVERSIONINFOA
{
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR  szCSDVersion[128];
};

struct OSVERSIONINFOEXA
{
    OSVERSIONINFOA versionInfoA;
    WORD  wServicePackMajor;
    WORD  wServicePackMinor;
    WORD  wSuiteMask;
    BYTE  wProductType;
    BYTE  wReserved;
};

// Pseudo handles of the calling process and thread; they need no closing.
static const HANDLE CurrentProcessHandle = 0xffffffff;
static const HANDLE CurrentThreadHandle = 0xfffffffe;

static const DWORD WAIT_FAILED = 0xffffffff;
static const DWORD MEM_COMMIT = 0x1000;
static const DWORD MEM_RELEASE = 0x8000;

// Copies text with its terminator if it fits in size characters; false (nothing copied) otherwise.
static bool copyString(char* buffer, DWORD size, const char* text)
{
    const size_t length = strlen(text);
    if (!buffer || length >= size)
        return false;
    memcpy(buffer, text, length + 1);
    return true;
}

// Win32 convention for returned paths: the length without the terminator when the text fits, else the size
// needed with it (nothing copied).
static DWORD returnPath(char* buffer, DWORD size, const std::string& path)
{
    if (!copyString(buffer, size, path.c_str()))
        return DWORD(path.size() + 1);
    return DWORD(path.size());
}

BOOL CloseHandle(WinApplication* app, x86::CPU& cpu,
                 HANDLE hObject)
{
    NFS2_USE(cpu);
    if (!hObject || hObject == CurrentProcessHandle || hObject == CurrentThreadHandle)
        return 1;
    return app->freeResource(hObject) ? 1 : 0;
}

LONG CompareFileTime(WinApplication* app, x86::CPU& cpu,
                     const FILETIME *lpFileTime1, const FILETIME *lpFileTime2)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (lpFileTime1->dwHighDateTime < lpFileTime2->dwHighDateTime)
    {
        return -1;
    }
    else if (lpFileTime1->dwHighDateTime == lpFileTime2->dwHighDateTime)
    {
        if (lpFileTime1->dwLowDateTime < lpFileTime2->dwLowDateTime)
        {
            return -1;
        }
        else if (lpFileTime1->dwLowDateTime == lpFileTime2->dwLowDateTime)
        {
            return 0;
        }
        else
        {
            return 1;
        }
    }
    else
    {
        return 1;
    }
}

BOOL CreateDirectoryA(WinApplication* app, x86::CPU& cpu,
                      LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpSecurityAttributes);
    const std::string host = File::hostPath(lpPathName);
    if (mkdir(host.c_str(), 0777) != 0)
    {
        pf::logError("CreateDirectoryA %s failed (%s)", lpPathName, host.c_str());
        return 0;
    }
    return 1;
}

HANDLE CreateEventA(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName)
{
    NFS2_USE(cpu);
    NFS2_USE(lpEventAttributes);
    win32::Event* event = new win32::Event(lpName, bManualReset != 0, bInitialState != 0);
    const HANDLE handle = app->allocateResource(event);
    event->registerHandle(handle);
    return handle;
}

HANDLE CreateFileA(WinApplication* app, x86::CPU& cpu,
                   LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    NFS2_USE(cpu);
    NFS2_USE(dwShareMode);
    NFS2_USE(lpSecurityAttributes);
    NFS2_USE(hTemplateFile);
    win32::File* file = new win32::File(lpFileName, dwDesiredAccess, dwFlagsAndAttributes, dwCreationDisposition);
    if (*file)
    {
        return app->allocateResource(file);
    }
    else
    {
        delete file;
        return HANDLE(-1);
    }
}

HANDLE CreateFileMappingA(WinApplication* app, x86::CPU& cpu,
                          HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
                          DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
                          LPCSTR lpName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpFileMappingAttributes);
    NFS2_USE(dwMaximumSizeHigh);
    NFS2_USE(dwMaximumSizeLow);
    NFS2_USE(lpName);
    NFS2_ASSERT(flProtect & 0x2);
    const File* source = dynamic_cast<File*>(app->getResource(hFile));
    if (!source)
        return 0;
    File* file = new File(*source);
    if (!*file)
    {
        delete file;
        return 0;
    }
    return app->allocateResource(file);
}

HANDLE CreateMutexA(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName)
{
    NFS2_USE(cpu);
    NFS2_USE(lpMutexAttributes);
    win32::Mutex* mutex = new win32::Mutex(lpName);
    // New, so this never blocks.
    if (bInitialOwner)
        mutex->lock();
    return app->allocateResource(mutex);
}

HANDLE CreateThread(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                    LPTHREAD_START_ROUTINE lpStartAddress, x86::reg32 lpParameter, DWORD dwCreationFlags,
                    LPDWORD lpThreadId)
{
    NFS2_USE(cpu);
    NFS2_USE(lpThreadAttributes);
    NFS2_ASSERT(dwStackSize <= 1024*1024);
    win32::Thread* thread = new win32::Thread(app, lpStartAddress, lpParameter, dwCreationFlags);
    if ((dwCreationFlags & 0x4) == 0) // CREATE_SUSPENDED
    {
        thread->resume();
    }
    if (lpThreadId) *lpThreadId = thread->threadId();
    return app->allocateResource(thread);
}

void DebugBreak(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_ASSERT(false);
}

void DeleteCriticalSection(WinApplication* app, x86::CPU& cpu,
                           LPCRITICAL_SECTION lpCriticalSection)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    app->freeResource((*lpCriticalSection)->getResourceIndex());
}

BOOL DeleteFileA(WinApplication* app, x86::CPU& cpu,
                 LPCSTR lpFileName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return File::remove(lpFileName);
}

BOOL DuplicateHandle(WinApplication* app, x86::CPU& cpu,
                     HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                     LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions)
{
    NFS2_USE(cpu);
    NFS2_USE(hSourceProcessHandle);
    NFS2_USE(hTargetProcessHandle);
    NFS2_USE(dwDesiredAccess);
    NFS2_USE(bInheritHandle);
    NFS2_USE(dwOptions);
    if (!lpTargetHandle)
        return 0;
    // Only thread handles are duplicated; a handle of the current thread carries no state.
    if (hSourceHandle == CurrentThreadHandle)
    {
        *lpTargetHandle = app->allocateResource(new win32::Thread());
        return 1;
    }
    win32::Thread* thread = dynamic_cast<win32::Thread*>(app->getResource(hSourceHandle));
    if (!thread)
    {
        pf::logError("DuplicateHandle: 0x%x is not a thread handle", hSourceHandle);
        return 0;
    }
    *lpTargetHandle = app->allocateResource(thread->duplicate());
    return 1;
}

void EnterCriticalSection(WinApplication* app, x86::CPU& cpu,
                          LPCRITICAL_SECTION lpCriticalSection)
{
    // Blocks only with the execution lock released: the owner may need that lock to leave the section.
    if (!(*lpCriticalSection)->tryLock())
    {
        app->unlockContext(cpu);
        (*lpCriticalSection)->lock();
        app->lockContext(cpu);
    }
}

BOOL EscapeCommFunction(WinApplication* app, x86::CPU& cpu,
                        HANDLE hFile, DWORD dwFunc)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_USE(dwFunc);
    NFS2_ASSERT(false);
    return 0;
}

void ExitProcess(WinApplication* app, x86::CPU& cpu,
                 UINT uExitCode)
{
    NFS2_USE(app);
    NFS2_USE(uExitCode);
    win32::Window::postMessage(0, 0x0012, 0, 0);
    cpu.terminate = true;
}

void ExitThread(WinApplication* app, x86::CPU& cpu,
                DWORD dwExitCode)
{
    NFS2_USE(app);
    NFS2_USE(dwExitCode);
    cpu.terminate = true;
}

BOOL FindClose(WinApplication* app, x86::CPU& cpu,
               HANDLE hFindFile)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return app->freeResource(hFindFile) ? 1 : 0;
}

HANDLE FindFirstFileA(WinApplication* app, x86::CPU& cpu,
                      LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    NFS2_USE(cpu);
    win32::FileEnumerator* enumerator = new win32::FileEnumerator(lpFileName);
    if (enumerator->findNext(lpFindFileData))
    {
        return app->allocateResource(enumerator);
    }
    else
    {
        delete enumerator;
        return -1;
    }
}

BOOL FindNextFileA(WinApplication* app, x86::CPU& cpu,
                   HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    NFS2_USE(cpu);
    FileEnumerator* enumerator = dynamic_cast<FileEnumerator*>(app->getResource(hFindFile));
    return enumerator && enumerator->findNext(lpFindFileData);
}

BOOL FlushFileBuffers(WinApplication* app, x86::CPU& cpu,
                      HANDLE hFile)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_ASSERT(false);
    return 0;
}

BOOL FreeEnvironmentStringsA(WinApplication* app, x86::CPU& cpu,
                             LPSTR lpszEnvironmentBlock)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpszEnvironmentBlock
    );
    return 1;
}

BOOL FreeLibrary(WinApplication* app, x86::CPU& cpu,
                 HMODULE hLibModule)
{
    NFS2_USE(cpu);
    return app->freeResource(hLibModule) ? 1 : 0;
}

UINT GetACP(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_ASSERT(false);
    return 0;
}

BOOL GetCPInfo(WinApplication* app, x86::CPU& cpu,
               UINT CodePage, LPCPINFO lpCPInfo)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(CodePage);
    lpCPInfo->MaxCharSize = 1;
    lpCPInfo->DefaultChar[0] = '?';
    lpCPInfo->DefaultChar[1] = 0;
    lpCPInfo->LeadByte[0] = 0;
    lpCPInfo->LeadByte[1] = 0;
    lpCPInfo->LeadByte[2] = 0;
    lpCPInfo->LeadByte[3] = 0;
    lpCPInfo->LeadByte[4] = 0;
    lpCPInfo->LeadByte[5] = 0;
    lpCPInfo->LeadByte[6] = 0;
    lpCPInfo->LeadByte[7] = 0;
    lpCPInfo->LeadByte[8] = 0;
    lpCPInfo->LeadByte[9] = 0;
    lpCPInfo->LeadByte[10] = 0;
    lpCPInfo->LeadByte[11] = 0;
    return 1;
}

BOOL GetCommModemStatus(WinApplication* app, x86::CPU& cpu,
                        HANDLE hFile, LPDWORD lpModemStat)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_USE(lpModemStat);
    NFS2_ASSERT(false);
    return 0;
}

BOOL GetCommProperties(WinApplication* app, x86::CPU& cpu,
                       HANDLE hFile, LPCOMMPROP lpCommProp)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_USE(lpCommProp);
    NFS2_ASSERT(false);
    return 0;
}

BOOL GetCommState(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDCB lpDCB)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_USE(lpDCB);
    NFS2_ASSERT(false);
    return 0;
}

x86::reg32 GetCommandLineA(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(cpu);
    return app->getAppName();
}

x86::reg32 GetCommandLineW(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return 0;
}

BOOL GetConsoleMode(WinApplication* app, x86::CPU& cpu,
                    HANDLE hConsoleHandle, LPDWORD lpMode)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hConsoleHandle);
    *lpMode = 0;
    return 1;
}

DWORD GetCurrentDirectoryA(WinApplication* app, x86::CPU& cpu,
                           DWORD nBufferLength, LPSTR lpBuffer)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return returnPath(lpBuffer, nBufferLength, File::getCurrentDirectory());
}

HANDLE GetCurrentProcess(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return CurrentProcessHandle;
}

DWORD GetCurrentProcessId(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_ASSERT(false);
    return 0;
}

DWORD GetCurrentThreadId(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(cpu);
    return app->getMemory<x86::reg32>(cpu.efs + 0x4);
}

HANDLE GetCurrentThread(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return CurrentThreadHandle;
}

BOOL GetDiskFreeSpaceA(WinApplication* app, x86::CPU& cpu,
                       LPCSTR lpRootPathName, LPDWORD lpSectorsPerCluster, LPDWORD lpBytesPerSector,
                       LPDWORD lpNumberOfFreeClusters, LPDWORD lpTotalNumberOfClusters)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpRootPathName);
    if (lpSectorsPerCluster) *lpSectorsPerCluster = 1;
    if (lpBytesPerSector) *lpBytesPerSector = 512;
    if (lpNumberOfFreeClusters) *lpNumberOfFreeClusters = 512*1024;
    if (lpTotalNumberOfClusters) *lpTotalNumberOfClusters = 1024*1024;
    return 1;
}

UINT GetDriveTypeA(WinApplication* app, x86::CPU& cpu,
                   LPCSTR lpRootPathName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    switch(*lpRootPathName)
    {
    case 'C':
        return 3; //DISK
    case 'D':
        return 5; //CDROM
    default:
        return 1; //INVALID
    }
}

x86::reg32 GetEnvironmentStrings(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return app->getEnv();
}

DWORD GetFileAttributesA(WinApplication* app, x86::CPU& cpu,
                         LPCSTR lpFileName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    struct stat status;
    if (stat(File::hostPath(lpFileName).c_str(), &status) != 0)
        return 0xffffffff;
    // FILE_ATTRIBUTE_DIRECTORY or FILE_ATTRIBUTE_NORMAL.
    return S_ISDIR(status.st_mode) ? 0x10 : 0x80;
}

BOOL GetFileTime(WinApplication* app, x86::CPU& cpu,
                 HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    if (lpCreationTime)
    {
        lpCreationTime->dwLowDateTime = 0;
        lpCreationTime->dwHighDateTime = 0;
    }
    if (lpLastAccessTime)
    {
        lpLastAccessTime->dwLowDateTime = 0;
        lpLastAccessTime->dwHighDateTime = 0;
    }
    if (lpLastWriteTime)
    {
        lpLastWriteTime->dwLowDateTime = 0;
        lpLastWriteTime->dwHighDateTime = 0;
    }
    return 0;
}

DWORD GetFileSize(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDWORD lpFileSizeHigh)
{
    NFS2_USE(cpu);
    win32::File* file = dynamic_cast<win32::File*>(app->getResource(hFile));
    if (!file)
        return 0xffffffff;
    if (lpFileSizeHigh)
        *lpFileSizeHigh = 0;
    return file->size();
}

DWORD GetFileType(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile)
{
    NFS2_USE(cpu);
    win32::File* f = dynamic_cast<win32::File*>(app->getResource(hFile));
    return f ? 1 : 0;
}

DWORD GetFullPathNameA(WinApplication* app, x86::CPU& cpu,
                       LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, x86::reg32* lpFilePart)
{
    if (!lpFileName)
        return 0;
    const x86::reg32 lpBufferAddress = app->getMemory<x86::reg32>(cpu.esp + 12);
    // Absolute names pass through; relative names are rooted in the guest current directory.
    std::string path;
    if (strlen(lpFileName) >= 2 && lpFileName[1] == ':')
    {
        path = lpFileName;
    }
    else
    {
        path = File::getCurrentDirectory();
        if (path.back() != '\\')
            path += '\\';
        path += lpFileName;
    }
    const DWORD result = returnPath(lpBuffer, nBufferLength, path);
    if (result < nBufferLength && lpFilePart)
    {
        const size_t lastSeparator = path.rfind('\\');
        *lpFilePart = lpBufferAddress + x86::reg32(lastSeparator == std::string::npos ? 0 : lastSeparator + 1);
    }
    return result;
}

DWORD GetLastError(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return 0;
}

void GetLocalTime(WinApplication* app, x86::CPU& cpu,
                  LPSYSTEMTIME lpSystemTime)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    win32::getSystemTime(lpSystemTime);
}

DWORD GetLogicalDriveStringsA(WinApplication* app, x86::CPU& cpu,
                              DWORD nBufferLength, LPSTR lpBuffer)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (nBufferLength)
    {
        lpBuffer[0] = 'C';
        lpBuffer[1] = 'D';
        lpBuffer[2] = 0;
    }
    return 2;
}

DWORD GetLogicalDrives(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return 0x2 | 0x4;
}

DWORD GetModuleFileNameA(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, LPSTR lpFilename, DWORD nSize)
{
    NFS2_USE(cpu);
    // Only the executable (handle 0, or 1 from GetModuleHandleA) has a file name.
    if (hModule > 1 || !lpFilename || nSize == 0)
        return 0;
    // A buffer too small gets nSize - 1 characters and the terminator, and the result is nSize.
    const std::string path = std::string("C:\\") + app->getAppNameRaw();
    const DWORD length = DWORD(std::min<size_t>(path.size(), nSize - 1));
    memcpy(lpFilename, path.c_str(), length);
    lpFilename[length] = 0;
    return length < path.size() ? nSize : length;
}

DWORD GetModuleFileNameW(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, LPWSTR lpFilename, DWORD nSize)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hModule);
    NFS2_USE(lpFilename);
    NFS2_USE(nSize);
    NFS2_ASSERT(false);
    return 0;
}

HMODULE GetModuleHandleA(WinApplication* app, x86::CPU& cpu,
                         LPCSTR lpModuleName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    // Only the executable has a module handle; libraries are loaded with LoadLibraryA.
    return lpModuleName ? 0 : 1;
}

UINT GetOEMCP(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return 850;
}

BOOL GetOverlappedResult(WinApplication* app, x86::CPU& cpu,
                         HANDLE hFile, LPOVERLAPPED lpOverlapped,
                         LPDWORD lpNumberOfBytesTransferred, BOOL bWait)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_USE(lpOverlapped);
    NFS2_USE(lpNumberOfBytesTransferred);
    NFS2_USE(bWait);
    NFS2_ASSERT(false);
    return 0;
}

FARPROC GetProcAddress(WinApplication* app, x86::CPU& cpu,
                       HMODULE hModule, LPCSTR lpProcName)
{
    NFS2_USE(cpu);
    win32::LibraryHandle* libraryHandle = dynamic_cast<win32::LibraryHandle*>(app->getResource(hModule));
    NFS2_ASSERT(libraryHandle);
    const x86::reg32 address = libraryHandle->getLibrary()->find(lpProcName);
    if (!address)
        pf::logInfo("GetProcAddress: no %s", lpProcName);
    return address;
}

HANDLE GetStdHandle(WinApplication* app, x86::CPU& cpu,
                    DWORD nStdHandle)
{
    NFS2_USE(cpu);
    // STD_INPUT_HANDLE (-10), STD_OUTPUT_HANDLE (-11), STD_ERROR_HANDLE (-12): one handle each, made again only
    // after the game closes it.
    static const char* const names[3] = { "stdin", "stdout", "stderr" };
    static HANDLE handles[3] = { 0, 0, 0 };
    const x86::reg32 index = x86::reg32(-10) - nStdHandle;
    if (index >= 3)
        return HANDLE(-1);
    if (!handles[index] || !dynamic_cast<win32::File*>(app->getResource(handles[index])))
        handles[index] = app->allocateResource(new win32::File(int(index), names[index]));
    return handles[index];
}

void GetSystemInfo(WinApplication* app, x86::CPU& cpu,
                   LPSYSTEM_INFO lpSystemInfo)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    lpSystemInfo->wProcessorArchitecture = 0;
    lpSystemInfo->wReserved = 0;
    lpSystemInfo->dwPageSize = 4096;
    lpSystemInfo->lpMinimumApplicationAddress = 0x10000;
    lpSystemInfo->lpMaximumApplicationAddress = 0x7ffeffff;
    lpSystemInfo->dwActiveProcessorMask = 1;
    lpSystemInfo->dwNumberOfProcessors = 1;
    lpSystemInfo->dwProcessorType = 2;
    lpSystemInfo->dwAllocationGranularity = 4096;
    lpSystemInfo->wProcessorLevel = 5;
    lpSystemInfo->wProcessorRevision = 0xff50;
}

int GetThreadPriority(WinApplication* app, x86::CPU& cpu,
                      HANDLE hThread)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hThread);
    NFS2_ASSERT(false);
    return 0;
}

DWORD GetTickCount(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    DWORD result = timeGetTickCount();
    return result;
}

DWORD GetTimeZoneInformation(WinApplication* app, x86::CPU& cpu,
                             LPTIME_ZONE_INFORMATION lpTimeZoneInformation)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpTimeZoneInformation);
    NFS2_ASSERT(false);
    return 0;
}

DWORD GetVersion(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return 0x80000304;
}

BOOL GetVersionExA(WinApplication* app, x86::CPU& cpu,
                   LPOSVERSIONINFOA lpVersionInformation)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    lpVersionInformation->dwMajorVersion = 5;
    lpVersionInformation->dwMinorVersion = 1;
    lpVersionInformation->dwBuildNumber = 2600;
    lpVersionInformation->dwPlatformId = 2;
    strcpy(lpVersionInformation->szCSDVersion, "Service Pack 3");
    if (lpVersionInformation->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA))
    {
        OSVERSIONINFOEXA* info = reinterpret_cast<OSVERSIONINFOEXA*>(lpVersionInformation);
        info->wProductType = 1;
        info->wServicePackMajor = 3;
        info->wServicePackMinor = 0;
    }
    return 1;
}

BOOL GetVolumeInformationA(WinApplication* app, x86::CPU& cpu,
                           LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
                           LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
                           LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer,
                           DWORD nFileSystemNameSize)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpRootPathName);
    if (lpVolumeNameBuffer && !copyString(lpVolumeNameBuffer, nVolumeNameSize, "DISK"))
        return 0;
    if (lpFileSystemNameBuffer && !copyString(lpFileSystemNameBuffer, nFileSystemNameSize, "FAT"))
        return 0;
    if (lpVolumeSerialNumber)
        *lpVolumeSerialNumber = 0x12345678;
    if (lpMaximumComponentLength)
        *lpMaximumComponentLength = 255;
    if (lpFileSystemFlags)
        *lpFileSystemFlags = 1|2|4;
    return 1;
}

UINT GetWindowsDirectoryA(WinApplication* app, x86::CPU& cpu,
                          LPSTR lpBuffer, UINT uSize)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpBuffer);
    NFS2_USE(uSize);
    NFS2_ASSERT(false);
    return 0;
}

void GlobalMemoryStatus(WinApplication* app, x86::CPU& cpu,
                        LPMEMORYSTATUS lpBuffer)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    lpBuffer->dwLength = sizeof(MEMORYSTATUS);
    lpBuffer->dwMemoryLoad = 0;
    lpBuffer->dwTotalPhys = 268435456;
    lpBuffer->dwAvailPhys = 268435456;
    lpBuffer->dwTotalPageFile = 268435456;
    lpBuffer->dwAvailPageFile = 268435456;
    lpBuffer->dwTotalVirtual = 0x7fffffff;
    lpBuffer->dwAvailVirtual = 0x7fffffff;
}

void InitializeCriticalSection(WinApplication* app, x86::CPU& cpu,
                               LPCRITICAL_SECTION lpCriticalSection)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    *lpCriticalSection = new win32::Mutex("");
    app->allocateResource(*lpCriticalSection);
}

void LeaveCriticalSection(WinApplication* app, x86::CPU& cpu,
                          LPCRITICAL_SECTION lpCriticalSection)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    // Keeps the execution lock: a thread that was waiting for the section takes the lock at its owner's next
    // release or preemption point.
    (*lpCriticalSection)->unlock();
}

HMODULE LoadLibraryA(WinApplication* app, x86::CPU& cpu,
                     LPCSTR lpLibFileName)
{
    NFS2_USE(cpu);
    win32::LibraryHandle* lib = new win32::LibraryHandle(lpLibFileName);
    if (*lib)
        return app->allocateResource(lib);
    pf::logInfo("LoadLibraryA: %s is not available", lpLibFileName);
    delete lib;
    return 0;
}

x86::reg32 MapViewOfFile(WinApplication* app, x86::CPU& cpu,
                         HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
                         DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap)
{
    NFS2_USE(dwDesiredAccess);
    File* file = dynamic_cast<File*>(app->getResource(hFileMappingObject));
    if (!file || dwFileOffsetHigh != 0)
        return 0;
    // Size 0 maps from the offset to the end of the file.
    const x86::reg32 fileSize = file->size();
    if (dwNumberOfBytesToMap == 0)
        dwNumberOfBytesToMap = fileSize > dwFileOffsetLow ? fileSize - dwFileOffsetLow : 0;
    if (dwNumberOfBytesToMap == 0)
        return 0;
    app->unlockContext(cpu);
    MemMap* result = new MemMap(dwNumberOfBytesToMap);
    x86::reg8* data = &app->getMemory<x86::reg8>(result->getBlockStart());
    x86::reg32 bytesRead = 0;
    const bool loaded = file->seek(0, x86::sreg32(dwFileOffsetLow)) == dwFileOffsetLow
                     && file->read(data, dwNumberOfBytesToMap, &bytesRead);
    if (loaded)
    {
        // Past the end of the file the view reads as zeros.
        memset(data + bytesRead, 0, result->getBlockSize() - bytesRead);
    }
    app->lockContext(cpu);
    if (!loaded)
    {
        delete result;
        return 0;
    }
    return result->getBlockStart();
}

BOOL MoveFileA(WinApplication* app, x86::CPU& cpu,
               LPCSTR lpExistingFileName, LPCSTR lpNewFileName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpExistingFileName);
    NFS2_USE(lpNewFileName);
    NFS2_ASSERT(false);
    return 0;
}

int MultiByteToWideChar(WinApplication* app, x86::CPU& cpu,
                        UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr,
                        int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(CodePage);
    NFS2_USE(dwFlags);
    // Bytes map to the same code points. cbMultiByte -1 converts up to and with the terminator; cchWideChar 0 asks
    // for the length; a buffer too small fails.
    if (!lpMultiByteStr || cbMultiByte == 0 || cchWideChar < 0)
        return 0;
    const int length = cbMultiByte < 0 ? int(strlen(lpMultiByteStr)) + 1 : cbMultiByte;
    if (cchWideChar == 0)
        return length;
    if (!lpWideCharStr || length > cchWideChar)
        return 0;
    for (int i = 0; i < length; ++i)
        lpWideCharStr[i] = WCHAR(x86::reg8(lpMultiByteStr[i]));
    return length;
}

void OutputDebugStringA(WinApplication* app, x86::CPU& cpu,
                        LPCSTR lpOutputString)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    pf::logInfo("%s", lpOutputString);
}

BOOL PeekConsoleInputA(WinApplication* app, x86::CPU& cpu,
                       HANDLE hConsoleInput, PINPUT_RECORD lpBuffer,
                       DWORD nLength, LPDWORD lpNumberOfEventsRead)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hConsoleInput);
    NFS2_USE(lpBuffer);
    NFS2_USE(nLength);
    NFS2_USE(lpNumberOfEventsRead);
    *lpNumberOfEventsRead = 0;
    return 1;
}

BOOL PulseEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    NFS2_USE(cpu);
    Event* event = dynamic_cast<Event*>(app->getResource(hEvent));
    return event ? event->pulse() : 0;
}

BOOL PurgeComm(WinApplication* app, x86::CPU& cpu,
               HANDLE hFile, DWORD dwFlags)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_USE(dwFlags);
    NFS2_ASSERT(false);
    return 0;
}

BOOL ReadConsoleInputA(WinApplication* app, x86::CPU& cpu,
                       HANDLE hConsoleInput, PINPUT_RECORD lpBuffer, DWORD nLength,
                       LPDWORD lpNumberOfEventsRead)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hConsoleInput);
    NFS2_USE(lpBuffer);
    NFS2_USE(nLength);
    *lpNumberOfEventsRead = 0;
    return 1;
}

BOOL ReadFile(WinApplication* app, x86::CPU& cpu,
              HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
              LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    NFS2_USE(cpu);
    NFS2_USE(lpOverlapped);
    win32::File* f = dynamic_cast<win32::File*>(app->getResource(hFile));
    if (!f)
    {
        return 0;
    }
    else
    {
        app->unlockContext(cpu);
        BOOL result = f->read(lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead);
        app->lockContext(cpu);
        return result;
    }
}

BOOL RemoveDirectoryA(WinApplication* app, x86::CPU& cpu, LPCSTR lpPathName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpPathName);
    NFS2_ASSERT(false);
    return 0;
}

BOOL ResetEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    NFS2_USE(cpu);
    Event* event = dynamic_cast<Event*>(app->getResource(hEvent));
    if (!event)
        return 0;
    event->reset();
    return 1;
}

DWORD ResumeThread(WinApplication* app, x86::CPU& cpu,
                   HANDLE hThread)
{
    NFS2_USE(cpu);
    win32::Thread* thread = dynamic_cast<win32::Thread*>(app->getResource(hThread));
    if (!thread)
        return 0xffffffff;
    thread->resume();
    return 1;
}

void RtlUnwind(WinApplication* app, x86::CPU& cpu,
               PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(TargetFrame);
    NFS2_USE(TargetIp);
    NFS2_USE(ExceptionRecord);
    NFS2_USE(ReturnValue);
    NFS2_ASSERT(false);
}

BOOL SetCommState(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDCB lpDCB)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_USE(lpDCB);
    NFS2_ASSERT(false);
    return 0;
}

BOOL SetCommTimeouts(WinApplication* app, x86::CPU& cpu,
                     HANDLE hFile, LPCOMMTIMEOUTS lpCommTimeouts)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hFile);
    NFS2_USE(lpCommTimeouts);
    NFS2_ASSERT(false);
    return 0;
}

BOOL SetConsoleCtrlHandler(WinApplication* app, x86::CPU& cpu,
                           PHANDLER_ROUTINE HandlerRoutine, BOOL Add)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(HandlerRoutine);
    NFS2_USE(Add);
    NFS2_ASSERT(false);
    return 0;
}

BOOL SetConsoleMode(WinApplication* app, x86::CPU& cpu,
                    HANDLE hConsoleHandle, DWORD  dwMode)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hConsoleHandle);
    NFS2_USE(dwMode);
    return 1;
}

BOOL SetCurrentDirectoryA(WinApplication* app, x86::CPU& cpu,
                          LPCSTR lpPathName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!lpPathName || !*lpPathName)
        return 0;
    return File::setCurrentDirectory(lpPathName) ? 1 : 0;
}

BOOL SetEndOfFile(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile)
{
    NFS2_USE(cpu);
    File* file = dynamic_cast<File*>(app->getResource(hFile));
    return file ? file->truncate() : 0;
}

BOOL SetEnvironmentVariableA(WinApplication* app, x86::CPU& cpu,
                             LPCSTR lpName, LPCSTR lpValue)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpName);
    NFS2_USE(lpValue);
    return 0;
}

BOOL SetEnvironmentVariableW(WinApplication* app, x86::CPU& cpu,
                             LPCWSTR lpName, LPCWSTR lpValue)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpName);
    NFS2_USE(lpValue);
    NFS2_ASSERT(false);
    return 0;
}

UINT SetErrorMode(WinApplication* app, x86::CPU& cpu, UINT uMode)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(uMode);
    return 0;
}

BOOL SetEvent(WinApplication* app, x86::CPU& cpu,
              HANDLE hEvent)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (win32::Event::setHandle(hEvent))
        return 1;
    // Windows fails the call for a closed handle; a thread may signal one its owner just closed.
    pf::logError("SetEvent: no event with handle 0x%x", hEvent);
    return 0;
}

BOOL SetFileAttributesA(WinApplication* app, x86::CPU& cpu,
                        LPCSTR lpFileName, DWORD  dwFileAttributes)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpFileName);
    NFS2_USE(dwFileAttributes);
    return 0;
}

DWORD SetFilePointer(WinApplication* app, x86::CPU& cpu,
                     HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    NFS2_USE(cpu);
    win32::File* file = dynamic_cast<win32::File*>(app->getResource(hFile));
    NFS2_ASSERT(lpDistanceToMoveHigh == 0);
    return file ? file->seek(dwMoveMethod, lDistanceToMove) : 0xffffffff;
}

void SetLastError(WinApplication* app, x86::CPU& cpu,
                  DWORD dwErrCode)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(dwErrCode);
}

BOOL SetStdHandle(WinApplication* app, x86::CPU& cpu,
                  DWORD nStdHandle, HANDLE hHandle)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(nStdHandle);
    NFS2_USE(hHandle);
    NFS2_ASSERT(false);
    return 0;
}

BOOL SetThreadPriority(WinApplication* app, x86::CPU& cpu,
                       HANDLE hThread, int nPriority)
{
    NFS2_USE(cpu);
    win32::Thread* thread = dynamic_cast<win32::Thread*>(app->getResource(hThread));
    if (thread)
        thread->setPriority(nPriority);
    return 1;
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(WinApplication* app, x86::CPU& cpu,
                                                         LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpTopLevelExceptionFilter);
    return 0;
}

DWORD SleepEx(WinApplication* app, x86::CPU& cpu,
              DWORD dwMilliseconds, BOOL bAlertable)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(bAlertable);
    app->unlockContext(cpu);
    win32::Thread::sleep(dwMilliseconds);
    app->lockContext(cpu);
    return 0;
}

BOOL TerminateThread(WinApplication* app, x86::CPU& cpu,
                     HANDLE hThread, DWORD dwExitCode)
{
    NFS2_USE(cpu);
    NFS2_USE(dwExitCode);
    // The pseudo handle of the calling thread is not a thread object: the call fails, the thread goes on.
    Thread* thread = dynamic_cast<Thread*>(app->getResource(hThread));
    if (!thread)
        return 0;
    app->unlockContext(cpu);
    thread->terminate();
    app->lockContext(cpu);
    return 1;
}

DWORD TlsAlloc(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_ASSERT(false);
    return 0;
}

BOOL TlsFree(WinApplication* app, x86::CPU& cpu,
             DWORD dwTlsIndex)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(dwTlsIndex);
    NFS2_ASSERT(false);
    return 0;
}

x86::reg32 TlsGetValue(WinApplication* app, x86::CPU& cpu,
                       DWORD dwTlsIndex)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(dwTlsIndex);
    NFS2_ASSERT(false);
    return 0;
}

BOOL TlsSetValue(WinApplication* app, x86::CPU& cpu,
                 DWORD dwTlsIndex, LPVOID lpTlsValue)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(dwTlsIndex);
    NFS2_USE(lpTlsValue);
    NFS2_ASSERT(false);
    return 0;
}

LONG UnhandledExceptionFilter(WinApplication* app, x86::CPU& cpu,
                              struct _EXCEPTION_POINTERS *ExceptionInfo)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(ExceptionInfo);
    NFS2_ASSERT(false);
    return 0;
}

BOOL UnmapViewOfFile(WinApplication* app, x86::CPU& cpu,
    Packed<const void> lpBaseAddress)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    MemMap* memBlock = lpBaseAddress ? MemMap::findBlock(lpBaseAddress) : nullptr;
    if (!memBlock)
        return 0;
    delete memBlock;
    return 1;
}

// Every block is reserved and committed at once: committing pages of a block returns them as they are.
x86::reg32 VirtualAlloc(WinApplication* app, x86::CPU& cpu,
                        Packed<void> lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    NFS2_USE(cpu);
    NFS2_USE(flProtect);
    if (dwSize == 0)
        return 0;
    if (lpAddress)
    {
        const MemMap* block = MemMap::findBlock(lpAddress);
        if (!(flAllocationType & MEM_COMMIT) || !block
            || x86::reg64(lpAddress) + dwSize > x86::reg64(block->getBlockStart()) + block->getBlockSize())
            return 0;
        return lpAddress & ~4095u;
    }
    MemMap* map = new MemMap(dwSize);
    memset(&app->getMemory<x86::reg8>(map->getBlockStart()), 0, dwSize);
    return map->getBlockStart();
}

BOOL VirtualFree(WinApplication* app, x86::CPU& cpu,
                 Packed<void> lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(dwSize);
    MemMap* memBlock = lpAddress ? MemMap::findBlock(lpAddress) : nullptr;
    if (!memBlock)
        return 0;
    // MEM_DECOMMIT keeps the block: its pages stay committed.
    if (dwFreeType == MEM_RELEASE)
        delete memBlock;
    return 1;
}

BOOL VirtualProtect(WinApplication* app, x86::CPU& cpu,
                    Packed<void> lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpAddress);
    NFS2_USE(dwSize);
    NFS2_USE(flNewProtect);
    NFS2_USE(lpflOldProtect);
    return 1;
}

SIZE_T VirtualQuery(WinApplication* app, x86::CPU& cpu,
                    Packed<const void> lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    MemMap* memBlock = MemMap::findBlock(lpAddress);
    if (!memBlock || !lpBuffer || dwLength < sizeof(MEMORY_BASIC_INFORMATION))
        return 0;
    lpBuffer->BaseAddress = memBlock->getBlockStart();
    lpBuffer->AllocationBase = memBlock->getBlockStart();
    lpBuffer->AllocationProtect = 0x04; //READWRITE
    lpBuffer->RegionSize = memBlock->getBlockSize();
    lpBuffer->State = 0x1000; //COMMIT
    lpBuffer->Protect = 0x04; //READWRITE
    lpBuffer->Type = 0x20000; //PRIVATE
    return sizeof(*lpBuffer);
}

// Only events are waited on; any other handle fails the wait (WAIT_FAILED).
DWORD WaitForMultipleObjectsEx(WinApplication* app, x86::CPU& cpu,
                               DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll,
                               DWORD dwMilliseconds, BOOL bAlertable)
{
    NFS2_USE(bAlertable);
    app->unlockContext(cpu);
    const DWORD result = DWORD(win32::Event::wait(cpu, lpHandles, nCount, bWaitAll != 0, x86::sreg32(dwMilliseconds)));
    app->lockContext(cpu);
    return result;
}

DWORD WaitForSingleObject(WinApplication* app, x86::CPU& cpu,
                          HANDLE hHandle, DWORD dwMilliseconds)
{
    app->unlockContext(cpu);
    const DWORD result = DWORD(win32::Event::wait(cpu, &hHandle, 1, false, x86::sreg32(dwMilliseconds)));
    app->lockContext(cpu);
    return result;
}

int WideCharToMultiByte(WinApplication* app, x86::CPU& cpu,
                        UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
                        LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(CodePage);
    NFS2_USE(dwFlags);
    NFS2_USE(lpWideCharStr);
    NFS2_USE(cchWideChar);
    NFS2_USE(lpMultiByteStr);
    NFS2_USE(cbMultiByte);
    NFS2_USE(lpDefaultChar);
    NFS2_USE(lpUsedDefaultChar);
    NFS2_ASSERT(false);
    return 0;
}

BOOL WriteConsoleA(WinApplication* app, x86::CPU& cpu,
                   HANDLE hConsoleOutput, const void* lpBuffer, DWORD nNumberOfCharsToWrite,
                   LPDWORD lpNumberOfCharsWritten, LPVOID  lpReserved)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hConsoleOutput);
    NFS2_USE(lpBuffer);
    NFS2_USE(nNumberOfCharsToWrite);
    NFS2_USE(lpNumberOfCharsWritten);
    NFS2_USE(lpReserved);
    NFS2_ASSERT(false);
    return 0;
}

BOOL WriteFile(WinApplication* app, x86::CPU& cpu,
               HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
    NFS2_USE(cpu);
    NFS2_ASSERT(lpOverlapped == nullptr);
    win32::File* f = dynamic_cast<win32::File*>(app->getResource(hFile));
    if (!f)
    {
        return 0;
    }
    else
    {
        app->unlockContext(cpu);
        x86::reg32 result = f->write(lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten);
        app->lockContext(cpu);
        return result;
    }
}

// ---- Private profiles (ini files) ----------------------------------------------------------------------------

namespace
{

typedef std::vector<std::pair<std::string, std::string>> ProfileSection;
typedef std::vector<std::pair<std::string, ProfileSection>> Profile;

std::string trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return std::string();
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// The sections of an ini file in file order with their key=value lines, keys and values trimmed and values
// without surrounding quotes; a section named twice is one section. Empty when the file does not exist.
Profile readProfile(const char* fileName)
{
    Profile profile;
    if (!fileName)
        return profile;
    File file(fileName, GENERIC_READ, 0, OPEN_EXISTING);
    if (!file)
        return profile;
    std::string text(file.size(), '\0');
    x86::reg32 length = 0;
    if (!text.empty())
        file.read(&text[0], x86::reg32(text.size()), &length);
    text.resize(length);
    // A UTF-8 byte order mark is not part of the first line.
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
        text.erase(0, 3);
    size_t section = std::string::npos;
    size_t position = 0;
    while (position < text.size())
    {
        size_t end = text.find('\n', position);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = trim(text.substr(position, end - position));
        position = end + 1;
        if (line.empty() || line[0] == ';')
            continue;
        if (line[0] == '[')
        {
            const size_t close = line.find(']');
            section = std::string::npos;
            if (close == std::string::npos)
                continue;
            const std::string name = trim(line.substr(1, close - 1));
            for (size_t i = 0; i < profile.size() && section == std::string::npos; ++i)
            {
                if (strcasecmp(profile[i].first.c_str(), name.c_str()) == 0)
                    section = i;
            }
            if (section == std::string::npos)
            {
                section = profile.size();
                profile.emplace_back(name, ProfileSection());
            }
            continue;
        }
        const size_t equals = line.find('=');
        if (section == std::string::npos || equals == std::string::npos)
            continue;
        std::string value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        profile[section].second.emplace_back(trim(line.substr(0, equals)), value);
    }
    return profile;
}

const ProfileSection* findProfileSection(const Profile& profile, const char* section)
{
    for (const auto& entry : profile)
    {
        if (strcasecmp(entry.first.c_str(), section) == 0)
            return &entry.second;
    }
    return nullptr;
}

bool readProfileValue(const char* fileName, const char* section, const char* key, std::string* value)
{
    const Profile profile = readProfile(fileName);
    const ProfileSection* entries = findProfileSection(profile, section);
    if (!entries)
        return false;
    for (const auto& entry : *entries)
    {
        if (strcasecmp(entry.first.c_str(), key) == 0)
        {
            *value = entry.second;
            return true;
        }
    }
    return false;
}

// Stores the strings, each terminated, then a final terminator. A list that does not fit is cut to size - 2
// characters, terminated twice, and size - 2 is returned, as Windows does.
DWORD copyProfileList(const std::vector<std::string>& items, LPSTR buffer, DWORD size)
{
    if (!buffer || size < 2)
        return 0;
    DWORD length = 0;
    for (const std::string& item : items)
    {
        if (length + item.size() + 2 > size)
        {
            memcpy(buffer + length, item.c_str(), size - 2 - length);
            buffer[size - 2] = 0;
            buffer[size - 1] = 0;
            return size - 2;
        }
        memcpy(buffer + length, item.c_str(), item.size() + 1);
        length += DWORD(item.size() + 1);
    }
    buffer[length] = 0;
    if (length == 0)
        buffer[1] = 0;
    return length;
}

}

UINT GetPrivateProfileIntA(WinApplication* app, x86::CPU& cpu,
                          LPCSTR lpAppName, LPCSTR lpKeyName, int nDefault, LPCSTR lpFileName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    std::string value;
    if (!lpAppName || !lpKeyName || !readProfileValue(lpFileName, lpAppName, lpKeyName, &value))
        return UINT(nDefault);
    return UINT(strtol(value.c_str(), nullptr, 10));
}

// "key=value" per entry.
DWORD GetPrivateProfileSectionA(WinApplication* app, x86::CPU& cpu,
                                LPCSTR lpAppName, LPSTR lpReturnedString, DWORD nSize, LPCSTR lpFileName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    std::vector<std::string> lines;
    const Profile profile = readProfile(lpFileName);
    const ProfileSection* entries = lpAppName ? findProfileSection(profile, lpAppName) : nullptr;
    if (entries)
    {
        for (const auto& entry : *entries)
            lines.push_back(entry.first + "=" + entry.second);
    }
    return copyProfileList(lines, lpReturnedString, nSize);
}

// A null section name lists the section names, a null key name the keys of the section.
DWORD GetPrivateProfileStringA(WinApplication* app, x86::CPU& cpu,
                               LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString,
                               DWORD nSize, LPCSTR lpFileName)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!lpReturnedString || nSize == 0)
        return 0;
    if (!lpAppName || !lpKeyName)
    {
        std::vector<std::string> names;
        const Profile profile = readProfile(lpFileName);
        if (!lpAppName)
        {
            for (const auto& section : profile)
                names.push_back(section.first);
        }
        else if (const ProfileSection* entries = findProfileSection(profile, lpAppName))
        {
            for (const auto& entry : *entries)
                names.push_back(entry.first);
        }
        return copyProfileList(names, lpReturnedString, nSize);
    }
    std::string value;
    if (!readProfileValue(lpFileName, lpAppName, lpKeyName, &value))
        value = trim(lpDefault ? lpDefault : "");
    const DWORD length = std::min(DWORD(value.size()), nSize - 1);
    memcpy(lpReturnedString, value.c_str(), length);
    lpReturnedString[length] = 0;
    return length;
}

// ---- Process heap: one guest memory block per allocation ------------------------------------------------------

namespace
{

const DWORD HEAP_ZERO_MEMORY = 0x8;

class ProcessHeap : public GenericResource
{
public:
    std::map<x86::reg32, SIZE_T> sizes;     // requested size per allocation
};

ProcessHeap* processHeap(WinApplication* app, HANDLE heap)
{
    ProcessHeap* result = dynamic_cast<ProcessHeap*>(app->getResource(heap));
    NFS2_ASSERT(result);
    return result;
}

}

HANDLE GetProcessHeap(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(cpu);
    static HANDLE s_handle = 0;
    if (!s_handle)
        s_handle = app->allocateResource(new ProcessHeap());
    return s_handle;
}

x86::reg32 HeapAlloc(WinApplication* app, x86::CPU& cpu,
                     HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes)
{
    NFS2_USE(cpu);
    MemMap* block = new MemMap(dwBytes ? dwBytes : 1);
    const x86::reg32 address = block->getBlockStart();
    if (dwFlags & HEAP_ZERO_MEMORY)
        memset(&app->getMemory<x86::reg8>(address), 0, dwBytes);
    processHeap(app, hHeap)->sizes[address] = dwBytes;
    return address;
}

BOOL HeapFree(WinApplication* app, x86::CPU& cpu,
              HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem)
{
    NFS2_USE(cpu);
    NFS2_USE(dwFlags);
    if (!lpMem)
        return 1;
    if (!processHeap(app, hHeap)->sizes.erase(lpMem))
        return 0;
    delete MemMap::findBlock(lpMem);
    return 1;
}

x86::reg32 HeapReAlloc(WinApplication* app, x86::CPU& cpu,
                       HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem, SIZE_T dwBytes)
{
    ProcessHeap* heap = processHeap(app, hHeap);
    const auto old = heap->sizes.find(lpMem);
    if (old == heap->sizes.end())
        return 0;
    const SIZE_T oldSize = old->second;
    const x86::reg32 address = HeapAlloc(app, cpu, hHeap, dwFlags & ~HEAP_ZERO_MEMORY, dwBytes);
    memcpy(&app->getMemory<x86::reg8>(address), &app->getMemory<x86::reg8>(lpMem), std::min(oldSize, dwBytes));
    if ((dwFlags & HEAP_ZERO_MEMORY) && dwBytes > oldSize)
        memset(&app->getMemory<x86::reg8>(address + oldSize), 0, dwBytes - oldSize);
    HeapFree(app, cpu, hHeap, 0, lpMem);
    return address;
}

SIZE_T HeapSize(WinApplication* app, x86::CPU& cpu,
                HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem)
{
    NFS2_USE(cpu);
    NFS2_USE(dwFlags);
    ProcessHeap* heap = processHeap(app, hHeap);
    const auto entry = heap->sizes.find(lpMem);
    return entry == heap->sizes.end() ? SIZE_T(-1) : entry->second;
}

// ---- Resources of the executable ------------------------------------------------------------------------------

namespace
{

x86::reg32 s_imageBase = 0;
x86::reg32 s_resourceSection = 0;

// OffsetToData of the entry of a resource directory (PE IMAGE_RESOURCE_DIRECTORY) with an integer id, or with a
// name when id is a string address (compared without case, as Windows does); 0 when there is none.
x86::reg32 findResourceEntry(WinApplication* app, x86::reg32 directory, x86::reg32 id)
{
    const x86::reg32 count = x86::reg32(app->getMemory<x86::reg16>(directory + 12)) +
                             app->getMemory<x86::reg16>(directory + 14);
    const bool byName = id >= 0x10000;
    const char* name = byName ? &app->getMemory<char>(id) : nullptr;
    for (x86::reg32 i = 0; i < count; ++i)
    {
        const x86::reg32 entry = directory + 16 + 8 * i;
        const x86::reg32 nameWord = app->getMemory<x86::reg32>(entry);
        bool match;
        if (nameWord & 0x80000000)
        {
            // Counted UTF-16 name.
            const x86::reg32 text = s_resourceSection + (nameWord & 0x7fffffff);
            const x86::reg32 length = app->getMemory<x86::reg16>(text);
            match = byName && strlen(name) == length;
            for (x86::reg32 c = 0; match && c < length; ++c)
            {
                const x86::reg32 stored = app->getMemory<x86::reg16>(text + 2 + 2 * c);
                match = x86::reg32(toupper(x86::reg8(name[c]))) == (stored < 0x80 ? x86::reg32(toupper(int(stored))) : stored);
            }
        }
        else
        {
            match = !byName && nameWord == id;
        }
        if (match)
            return app->getMemory<x86::reg32>(entry + 4);
    }
    return 0;
}

}

void setResourceSection(x86::reg32 imageBase, x86::reg32 address)
{
    s_imageBase = imageBase;
    s_resourceSection = address;
}

// The handle is the guest address of the resource's data entry (RVA, size, code page, reserved).
x86::reg32 FindResourceA(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, x86::reg32 lpName, x86::reg32 lpType)
{
    NFS2_USE(cpu);
    NFS2_USE(hModule);
    if (!s_resourceSection)
        return 0;
    // Type, name and language levels; the first language is taken.
    const x86::reg32 type = findResourceEntry(app, s_resourceSection, lpType);
    if (!(type & 0x80000000))
        return 0;
    const x86::reg32 name = findResourceEntry(app, s_resourceSection + (type & 0x7fffffff), lpName);
    if (!(name & 0x80000000))
        return 0;
    const x86::reg32 languages = s_resourceSection + (name & 0x7fffffff);
    if (app->getMemory<x86::reg16>(languages + 12) + app->getMemory<x86::reg16>(languages + 14) == 0)
        return 0;
    const x86::reg32 data = app->getMemory<x86::reg32>(languages + 16 + 4);
    return data & 0x80000000 ? 0 : s_resourceSection + data;
}

x86::reg32 LoadResource(WinApplication* app, x86::CPU& cpu,
                        HMODULE hModule, x86::reg32 hResInfo)
{
    NFS2_USE(cpu);
    NFS2_USE(hModule);
    return hResInfo ? s_imageBase + app->getMemory<x86::reg32>(hResInfo) : 0;
}

x86::reg32 LockResource(WinApplication* app, x86::CPU& cpu,
                        x86::reg32 hResData)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return hResData;
}

DWORD SizeofResource(WinApplication* app, x86::CPU& cpu,
                     HMODULE hModule, x86::reg32 hResInfo)
{
    NFS2_USE(cpu);
    NFS2_USE(hModule);
    return hResInfo ? x86::reg32(app->getMemory<x86::reg32>(hResInfo + 4)) : 0;
}

static const HFILE HFILE_ERROR = HFILE(-1);

HFILE _lclose(WinApplication* app, x86::CPU& cpu, HFILE hFile)
{
    NFS2_USE(cpu);
    return app->freeResource(hFile) ? hFile : HFILE_ERROR;
}

// OF_READ (0), OF_WRITE (1) or OF_READWRITE (2); share flags are ignored.
HFILE _lopen(WinApplication* app, x86::CPU& cpu, LPCSTR lpPathName, int iReadWrite)
{
    NFS2_USE(cpu);
    static const DWORD access[3] = { GENERIC_READ, GENERIC_WRITE, GENERIC_READ | GENERIC_WRITE };
    const x86::reg32 mode = x86::reg32(iReadWrite) & 3;
    if (mode > 2)
        return HFILE_ERROR;
    win32::File* file = new win32::File(lpPathName, access[mode], 0, OPEN_EXISTING);
    if (!*file)
    {
        delete file;
        return HFILE_ERROR;
    }
    return app->allocateResource(file);
}


}}
