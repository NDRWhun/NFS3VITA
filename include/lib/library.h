#ifndef LIB_LIBRARY_H_
#define LIB_LIBRARY_H_

#include <lib/winapp.h>
#include <string>
#include <map>

namespace win32
{

class Library
{
public:
    typedef std::pair<x86::reg32, MethodPtr> Symbol;
    Library(const char* name);
    ~Library();

    inline Library& registerSymbol(const char* name, MethodPtr symbol)
    {
        m_methods.insert(std::make_pair(std::string(name), std::make_pair(getSymbolIndex(), symbol)));
        return *this;
    }

    Symbol operator[](const std::string& symbolName)
    {
        return m_methods[symbolName];
    }

    // Pseudo-address of an exported symbol, 0 if the library has none.
    x86::reg32 find(const std::string& symbolName) const
    {
        const auto it = m_methods.find(symbolName);
        return it == m_methods.end() ? 0 : it->second.first;
    }

    // By file name, ignoring case and any directory in front of it.
    static Library* findLibrary(std::string name);
    void registerSymbols(WinApplication* app);

private:
    static x86::reg32 getSymbolIndex();

private:
    Library* m_next;
    const std::string m_name;
    std::map<std::string, Symbol > m_methods;
};

class LibraryHandle : public GenericResource
{
public:
    LibraryHandle(const char* name);
    ~LibraryHandle();

    inline operator void*() const { return m_library; }
    inline Library* getLibrary() { return m_library; }
    void registerSymbols(win32::WinApplication* application);

private:
    Library* m_library;
};

}

#endif

