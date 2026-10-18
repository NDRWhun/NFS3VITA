#include <lib/registry.h>

namespace win32
{

RegistryValue::RegistryValue(x86::reg32 value)
    :   m_type(REG_DWORD)
{
    m_value.dword = value;
}

RegistryValue::RegistryValue(const std::string& value)
    :   m_type(REG_SZ)
{
    NFS2_ASSERT(value.size() < sizeof(m_value.str));
    memcpy(m_value.str, value.c_str(), value.size()+1);
}

// RegQueryValueEx: without data only the type and size are returned; data that does not fit gives ERROR_MORE_DATA
// with the size needed.
x86::reg32 RegistryValue::query(x86::reg32* type, void* data, x86::reg32* dataSize)
{
    static const x86::reg32 ERROR_SUCCESS = 0;
    static const x86::reg32 ERROR_INVALID_PARAMETER = 87;
    static const x86::reg32 ERROR_MORE_DATA = 234;
    if (data && !dataSize)
        return ERROR_INVALID_PARAMETER;
    if (type)
        *type = m_type;
    const void* value = m_type == REG_DWORD ? static_cast<const void*>(&m_value.dword) : m_value.str;
    const x86::reg32 size = m_type == REG_DWORD ? x86::reg32(sizeof(m_value.dword))
                                                : x86::reg32(strlen(m_value.str) + 1);
    if (!dataSize)
        return ERROR_SUCCESS;
    const x86::reg32 room = *dataSize;
    *dataSize = size;
    if (!data)
        return ERROR_SUCCESS;
    if (room < size)
        return ERROR_MORE_DATA;
    memcpy(data, value, size);
    return ERROR_SUCCESS;
}

RegistryKey::RegistryKey()
{
}
RegistryKey::~RegistryKey()
{
}

void RegistryKey::addChild(const std::string& name, RegistryKey* key)
{
    m_children[name] = key;
}

RegistryKey* RegistryKey::getChild(const std::string& name) const
{
    std::map< std::string, RegistryKey*, ci_less >::const_iterator it = m_children.find(name);
    if (it != m_children.end())
    {
        return it->second;
    }
    else
    {
        return nullptr;
    }
}

void RegistryKey::addValue(const std::string& name, RegistryValue* value)
{
    m_values[name] = value;
}

RegistryValue* RegistryKey::getValue(const std::string& name) const
{
    std::map< std::string, RegistryValue*, ci_less >::const_iterator it = m_values.find(name);
    if (it != m_values.end())
    {
        return it->second;
    }
    else
    {
        return nullptr;
    }
}

}