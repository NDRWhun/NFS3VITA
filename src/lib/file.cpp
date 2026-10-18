#include <lib/file.h>
#include <platform/log.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace win32
{

// Guest current directory: "X:\" plus components, always ending in a backslash.
static std::string s_currentDirectory = "C:\\";
static std::string s_dataDirectory = "ux0:data/nfs3vita/";
static std::string s_cdDirectory = "ux0:data/nfs3vita/";

static bool isSeparator(char c)
{
    return c == '\\' || c == '/';
}

// Resolves "." and ".." in a guest path that starts with "X:"; the result ends with a backslash when the path did.
static std::string resolveDots(const std::string& path)
{
    std::string result = path.substr(0, 2);
    result[0] = char(toupper(result[0]));
    std::vector<std::string> parts;
    std::string::size_type start = 2;
    while (start <= path.size())
    {
        std::string::size_type end = start;
        while (end < path.size() && !isSeparator(path[end]))
            end++;
        std::string part = path.substr(start, end - start);
        if (part == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (!part.empty() && part != ".")
        {
            parts.push_back(part);
        }
        start = end + 1;
    }
    for (std::vector<std::string>::const_iterator it = parts.begin(); it != parts.end(); ++it)
        result += "\\" + *it;
    if (parts.empty() || (!path.empty() && isSeparator(path.back())))
        result += "\\";
    return result;
}

// resolveDots with a trailing backslash.
static std::string normalizeGuestDirectory(const std::string& path)
{
    std::string result = resolveDots(path);
    if (!isSeparator(result.back()))
        result += "\\";
    return result;
}

// Makes a guest path absolute ("X:\...") using the current directory.
static std::string absoluteGuestPath(const std::string& path)
{
    if (path.size() >= 2 && path[1] == ':')
        return path;
    if (!path.empty() && isSeparator(path[0]))
        return s_currentDirectory.substr(0, 2) + path;
    return s_currentDirectory + path;
}

// Guest path -> host path. Drive-letter paths land in the data (or CD) directory.
static std::string moveToRoot(const std::string& path)
{
    std::string device = path;
    for (size_t i = 0; i < device.size(); ++i)
        device[i] = char(tolower(device[i]));
    // No null device on the Vita: use a scratch file. Console output goes to the debug tty.
    if (device == "nul" || device == "conin$")
        return s_dataDirectory + "nul.tmp";
    if (device == "conout$")
        return "tty0:";
    std::string full = resolveDots(absoluteGuestPath(path));
    std::string rest = full.substr(2);
    while (!rest.empty() && isSeparator(rest[0]))
        rest.erase(0, 1);
    const std::string& root = (toupper(full[0]) == 'D') ? s_cdDirectory : s_dataDirectory;
    return root + rest;
}

// Lower-cases, converts separators and collapses repeats; the Vita filesystem is case-insensitive.
static std::string normalizePath(const std::string& input)
{
    std::string path;
    path.reserve(input.size());
    for (std::string::const_iterator it = input.begin(); it != input.end(); ++it)
    {
        char c = char(tolower(*it));
        if (c == '\\')
            c = '/';
        if (c == '/' && !path.empty() && path.back() == '/')
            continue;
        path += c;
    }
    return path;
}

// Case-insensitive match supporting '*' and '?'.
static bool wildcardMatch(const char* pattern, const char* name)
{
    const char* star = nullptr;
    const char* starName = nullptr;
    while (*name)
    {
        if (*pattern == '*')
        {
            star = pattern++;
            starName = name;
        }
        else if (*pattern == '?' || tolower(*pattern) == tolower(*name))
        {
            pattern++;
            name++;
        }
        else if (star)
        {
            pattern = star + 1;
            name = ++starName;
        }
        else
        {
            return false;
        }
    }
    while (*pattern == '*')
        pattern++;
    return *pattern == 0;
}

FileEnumerator::FileEnumerator(const char* filename)
    :   m_pattern(normalizePath(moveToRoot(filename)))
    ,   m_index(-1)
{
    pf::logDebug("Enumerate %s", m_pattern.c_str());
    /* Win32 "*.*" means every file */
    size_t pos = m_pattern.find("*.*");
    if (pos != std::string::npos)
        m_pattern.replace(pos, 3, "*");
    std::string dirPath, filePattern;
    size_t lastSlash = m_pattern.rfind('/');
    if (lastSlash != std::string::npos)
    {
        dirPath = m_pattern.substr(0, lastSlash);
        filePattern = m_pattern.substr(lastSlash + 1);
    }
    else
    {
        dirPath = s_dataDirectory;
        filePattern = m_pattern;
    }
    DIR* d = opendir(dirPath.c_str());
    if (d)
    {
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr)
        {
            if (wildcardMatch(filePattern.c_str(), entry->d_name))
            {
                std::string fullPath = dirPath + "/" + entry->d_name;
                struct stat buffer;
                memset(&buffer, 0, sizeof(buffer));
                stat(fullPath.c_str(), &buffer);
                WIN32_FIND_DATAA data;
                memset(&data, 0, sizeof(data));
                data.dwFileAttributes = S_ISDIR(buffer.st_mode) ? 16 : 0;
                data.nFileSizeHigh = 0;
                data.nFileSizeLow = x86::reg32(buffer.st_size);
                strncpy(data.cFileName, entry->d_name, sizeof(data.cFileName)-1);
                m_files.push_back(data);
            }
        }
        closedir(d);
    }
}

FileEnumerator::~FileEnumerator()
{
}

bool FileEnumerator::findNext(WIN32_FIND_DATAA *data)
{
    m_index++;
    if (size_t(m_index) >= m_files.size())
    {
        return false;
    }
    else
    {
        memcpy(data, &m_files[m_index], sizeof(*data));
        return true;
    }
}

struct ReadFilterEntry
{
    std::string             suffix;
    File::ReadFilter        filter;
};

// Written before guest code runs, read-only afterwards.
static std::vector<ReadFilterEntry> s_readFilters;

void File::setReadFilter(const char* pathSuffix, ReadFilter filter)
{
    s_readFilters.push_back(ReadFilterEntry{ pathSuffix, filter });
}

void File::initReadFilter()
{
    m_readFilter = nullptr;
    for (const ReadFilterEntry& entry : s_readFilters)
    {
        if (m_filename.size() >= entry.suffix.size()
            && m_filename.compare(m_filename.size() - entry.suffix.size(), entry.suffix.size(), entry.suffix) == 0)
            m_readFilter = entry.filter;
    }
}

File::File(int file, const char* name)
    :   m_file(dup(file))
    ,   m_filename(name)
{
    initReadFilter();
}

File::File(const File& other)
    :   m_file(open(other.m_filename.c_str(), O_RDONLY, 0664))
    ,   m_filename(other.m_filename)
    ,   m_readFilter(other.m_readFilter)
{
    pf::logDebug("opening file %s for Map", m_filename.c_str());
}

File::File(const char* path, x86::reg32 mode, x86::reg32 flags, x86::reg32 creationFlags)
    :   m_file(-1)
    ,   m_filename(normalizePath(moveToRoot(path)))
{
    NFS2_USE(flags);
    initReadFilter();
    // Access 0 only queries the file: opened for reading.
    const bool reads = (mode & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
    const bool writes = (mode & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    int openFlags = writes ? (reads ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch(creationFlags)
    {
    case CREATE_NEW:
        openFlags |= O_CREAT | O_EXCL;
        break;
    case CREATE_ALWAYS:
        openFlags |= O_CREAT | O_TRUNC;
        break;
    case OPEN_ALWAYS:
        openFlags |= O_CREAT;
        break;
    case OPEN_EXISTING:
        break;
    case TRUNCATE_EXISTING:
        openFlags |= O_TRUNC;
        break;
    default:
        pf::logError("CreateFile %s: unknown creation disposition %u", m_filename.c_str(), unsigned(creationFlags));
        return;
    }
    // The scratch file standing in for the null device may not exist yet.
    if (m_filename == s_dataDirectory + "nul.tmp")
        openFlags |= O_CREAT;

    m_file = open(m_filename.c_str(), openFlags, 0664);
    if (m_file == -1)
    {
        pf::logError("unable to open file %s", m_filename.c_str());
    }
    else
    {
        pf::logDebug("opening file %s", m_filename.c_str());
    }
}

File::~File()
{
    if (m_file != -1)
    {
        close(m_file);
        pf::logDebug("closing file %s", m_filename.c_str());
    }
}

x86::reg32 File::seek(x86::reg32 mode, x86::sreg32 offset)
{
    // Vita newlib's lseek dereferences a null descriptor for an invalid fd (unlike read/write/fstat),
    // e.g. the stdin handle: fd 0 cannot be dup'ed. Fail like Windows does for a console handle.
    if (m_file < 0)
        return x86::reg32(-1);
    int seekMode = 0;
    switch(mode)
    {
    case 0:
        seekMode = SEEK_SET;
        break;
    case 1:
        seekMode = SEEK_CUR;
        break;
    case 2:
        seekMode = SEEK_END;
        break;
    default:
        NFS2_ASSERT(false);
        break;
    }
    x86::reg32 result = x86::reg32(lseek(m_file, offset, seekMode));
    return result;
}

x86::reg32 File::read(void* buffer, x86::reg32 max, x86::reg32* totalRead)
{
    if (m_file < 0)
    {
        *totalRead = 0;
        return 0;
    }
    const off_t offset = m_readFilter ? lseek(m_file, 0, SEEK_CUR) : 0;
    *totalRead = x86::reg32(::read(m_file, buffer, max));
    if (m_readFilter && offset >= 0 && *totalRead != x86::reg32(-1))
        m_readFilter(x86::reg32(offset), static_cast<x86::reg8*>(buffer), *totalRead);
    return *totalRead != x86::reg32(-1);
}

x86::reg32 File::write(const void* buffer, x86::reg32 max, x86::reg32* totalWritten)
{
    *totalWritten = x86::reg32(::write(m_file, buffer, max));
    return *totalWritten == max;
}

x86::reg32 File::truncate()
{
    pf::logDebug("Truncating file %s", m_filename.c_str());
    if (m_file < 0)
        return 0;
    return ftruncate(m_file, lseek(m_file, 0, SEEK_CUR)) == 0;
}

x86::reg32 File::size() const
{
    struct stat buffer;
    memset(&buffer, 0, sizeof(buffer));
    fstat(m_file, &buffer);
    return x86::reg32(buffer.st_size);
}

// Win32 form: no trailing backslash except for a drive root.
std::string File::getCurrentDirectory()
{
    std::string result = s_currentDirectory;
    if (result.size() > 3)
        result.pop_back();
    return result;
}

bool File::setCurrentDirectory(const char* path)
{
    std::string directory = normalizeGuestDirectory(absoluteGuestPath(path));
    std::string host = normalizePath(moveToRoot(directory));
    struct stat buffer;
    memset(&buffer, 0, sizeof(buffer));
    if (stat(host.c_str(), &buffer) != 0 || !S_ISDIR(buffer.st_mode))
    {
        pf::logError("SetCurrentDirectory %s: not found (%s)", path, host.c_str());
        return false;
    }
    s_currentDirectory = directory;
    pf::logDebug("current directory %s", s_currentDirectory.c_str());
    return true;
}

void File::setDataDirectory(const char* path)
{
    s_dataDirectory = path;
    if (s_dataDirectory.empty() || s_dataDirectory.back() != '/')
        s_dataDirectory.append("/");
}

void File::setCdDirectory(const char* path)
{
    s_cdDirectory = path;
    if (s_cdDirectory.empty() || s_cdDirectory.back() != '/')
        s_cdDirectory.append("/");
}

std::string File::hostPath(const char* path)
{
    std::string host = normalizePath(moveToRoot(path));
    while (host.size() > 1 && host.back() == '/')
        host.pop_back();
    return host;
}

x86::reg32 File::remove(const char *filename)
{
    std::string f = normalizePath(moveToRoot(filename));
    if (unlink(f.c_str()) != 0)
    {
        pf::logError("unable to delete file %s", filename);
        return 0;
    }
    else
    {
        pf::logDebug("Deleting file %s", filename);
        return 1;
    }
}

}
