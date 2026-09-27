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

namespace win32
{

static std::string s_currentDirectory = "c:\\nfs2se\\";
static std::string s_dataDirectory = "ux0:data/nfs3vita/";
static std::string s_cdDirectory = "ux0:data/nfs3vita/";

static std::string moveToRoot(const std::string& path)
{
    if (path.size() >= 2 && path[1] == ':')
    {
        /* Absolute Win32 path (e.g. D:\DATA\...) - map to CD directory */
        if (path.size() >= 3 && path[2] == '\\')
            return s_cdDirectory + std::string(path.begin() + 3, path.end());
        else
            return s_cdDirectory + std::string(path.begin() + 2, path.end());
    }
    else if (path == "nul")
    {
        /* No null device on the Vita: use a scratch file */
        return s_dataDirectory + "nul.tmp";
    }
    else
    {
        /* Relative path - map to local install directory */
        return s_dataDirectory + path;
    }
}

// Lower-cases and converts separators; the Vita filesystem is case-insensitive.
static std::string normalizePath(std::string path)
{
    std::transform(path.begin(), path.end(), path.begin(), ::tolower);
    for (std::string::iterator it = path.begin(); it != path.end(); ++it)
        if (*it == '\\') *it = '/';
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

File::File(int file, const char* name)
    :   m_file(dup(file))
    ,   m_filename(name)
{
}

File::File(const File& other)
    :   m_file(open(other.m_filename.c_str(), O_RDONLY, 0664))
    ,   m_filename(other.m_filename)
{
    pf::logDebug("opening file %s for Map", m_filename.c_str());
}

File::File(const char* path, x86::reg32 mode, x86::reg32 flags, x86::reg32 creationFlags)
    :   m_file(-1)
    ,   m_filename(normalizePath(moveToRoot(path)))
{
    NFS2_USE(flags);
    int openFlags = 0;
    switch(mode)
    {
    case GENERIC_READ:
        openFlags |= O_RDONLY;
        break;
    case GENERIC_WRITE:
        openFlags |= O_WRONLY;
        break;
    case GENERIC_READ|GENERIC_WRITE:
        openFlags |= O_RDWR;
        break;
    default:
        NFS2_ASSERT(false);
    }
    switch(creationFlags)
    {
    case CREATE_NEW:
        openFlags |= O_CREAT;
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
        NFS2_ASSERT(false);
    }

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
    *totalRead = x86::reg32(::read(m_file, buffer, max));
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
    return ftruncate(m_file, lseek(m_file, 0, SEEK_CUR)) == 0;
}

x86::reg32 File::size() const
{
    struct stat buffer;
    memset(&buffer, 0, sizeof(buffer));
    fstat(m_file, &buffer);
    return x86::reg32(buffer.st_size);
}

const std::string& File::getCurrentDirectory()
{
    return s_currentDirectory;
}

void File::setCurrentDirectory(const char* path)
{
    s_currentDirectory = path;
    if (s_currentDirectory.back() != '\\')
        s_currentDirectory.append("\\");
}

void File::setDataDirectory(const char* path)
{
    s_dataDirectory = path;
    if (s_dataDirectory.back() != '/')
        s_dataDirectory.append("/");
}

void File::setCdDirectory(const char* path)
{
    s_cdDirectory = path;
    if (s_cdDirectory.back() != '/')
        s_cdDirectory.append("/");
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
