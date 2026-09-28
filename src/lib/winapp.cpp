#include <lib/winapp.h>
#include <lib/mutex.h>
#include <lib/event.h>
#include <lib/library.h>
#include <lib/registry.h>
#include <lib/thread.h>
#include <lib/timer.h>
#include <winapi/types.h>
#include <x86.h>

#include <winapi/dsetup.h>
#include <winapi/ddraw.h>
#include <winapi/glide2x.h>
#include <platform/log.h>
#include <algorithm>
#include <atomic>
#include <vector>

namespace win32
{

static x86::reg32 s_resourceIndex;
static std::atomic<int> s_resourceCount{0};

// The method table doubles while methods register; about 7,800 methods end in 16,384 slots.
static const x86::reg32 s_initialMethodSlotBits = 10;

[[noreturn]] static void missingMethod(x86::reg32 address)
{
    pf::logError("dynamic_call: no method registered at guest address 0x%08x", address);
    __builtin_trap();
}

// Empty slots hold address 0, so only a call to guest address 0 reaches this.
static void callAddressZero(WinApplication* application, x86::CPU& cpu)
{
    NFS2_USE(application);
    NFS2_USE(cpu);
    missingMethod(0);
}

GenericResource::GenericResource()
    :   m_resourceIndex(0)
{
    s_resourceCount.fetch_add(1);
}

GenericResource::~GenericResource()
{
    NFS2_ASSERT(m_resourceIndex == 0);
    s_resourceCount.fetch_sub(1);
}

void GenericResource::setResourceIndex(x86::reg32 resourceIndex)
{
    NFS2_ASSERT(!resourceIndex || !m_resourceIndex);
    m_resourceIndex = resourceIndex;
}

x86::reg32 GenericResource::getResourceIndex() const
{
    return m_resourceIndex;
}


WinApplication::WinApplication(const char* appName, x86::reg32 baseAddress, const std::vector<Section>& sections)
    :   m_appName(nullptr)
    ,   m_appNameW(nullptr)
    ,   m_env(nullptr)
    ,   m_executionContext(new Mutex("ExecutionContext"))
    ,   m_resourceContext(new Mutex("Resource"))
    ,   m_methodSlots(x86::reg32(1) << s_initialMethodSlotBits, MethodSlot{0, &callAddressZero})
    ,   m_methodShift(32 - s_initialMethodSlotBits)
    ,   m_cpu{}
{
    MemMap::init(baseAddress, sections);
    m_appName = new MemMap(4096);
    m_appNameW = new MemMap(4096);
    m_env = new MemMap(4096);
    dsetup::s_dSetupRegistry->registerSymbols(this);
    ddraw::s_dDrawRegistry->registerSymbols(this);
    glide2x::s_glide2xRegistry->registerSymbols(this);
    Event::init();
    Timer::init();
    strcpy(&getMemory<char>(m_appName->getBlockStart()), appName);
    getMemory<char>(m_env->getBlockStart()) = 0;
    WCHAR* wchar = &getMemory<WCHAR>(m_appNameW->getBlockStart());
    *wchar++ = 0xfffe;
    for (const char* appNameW = appName; *appNameW; ++wchar, ++appNameW)
    {
        *wchar = WCHAR(*appNameW);
    }
    *wchar = 0;
    allocateResourceFixed(new RegistryKey(), HKEY_CLASSES_ROOT);
    allocateResourceFixed(new RegistryKey(), HKEY_CURRENT_USER);
    allocateResourceFixed(new RegistryKey(), HKEY_LOCAL_MACHINE);
    allocateResourceFixed(new RegistryKey(), HKEY_USERS);
    allocateResourceFixed(new RegistryKey(), HKEY_PERFORMANCE_DATA);
    allocateResourceFixed(new RegistryKey(), HKEY_PERFORMANCE_TEXT);
    allocateResourceFixed(new RegistryKey(), HKEY_PERFORMANCE_NLSTEXT);
    allocateResourceFixed(new RegistryKey(), HKEY_CURRENT_CONFIG);
    allocateResourceFixed(new RegistryKey(), HKEY_DYN_DATA);
}

WinApplication::~WinApplication()
{
    delete m_appNameW;
    delete m_appName;
    // Timer callbacks and guest threads stop first so no guest code runs while resources are freed.
    Timer::fini();
    std::vector<Thread*> threads;
    m_resourceContext->lock();
    for (std::map<x86::reg32, GenericResource*>::const_iterator it = m_resources.begin(); it != m_resources.end(); ++it)
    {
        if (Thread* thread = dynamic_cast<Thread*>(it->second))
            threads.push_back(thread);
    }
    m_resourceContext->unlock();
    for (std::vector<Thread*>::iterator it = threads.begin(); it != threads.end(); ++it)
        (*it)->terminate();
    // Highest handles first: later objects depend on earlier ones (sound buffers on their device).
    m_resourceContext->lock();
    while (!m_resources.empty())
    {
        std::map<x86::reg32, GenericResource*>::iterator it = --m_resources.end();
        GenericResource* resource = it->second;
        m_resources.erase(it);
        resource->setResourceIndex(0);
        delete resource;
    }
    m_resourceContext->unlock();
    Event::fini();
    MemMap::fini();
    delete m_resourceContext;
    delete m_executionContext;
    NFS2_ASSERT(s_resourceCount.load() == 0);
}

void WinApplication::addRegistryKey(x86::reg32 root, const char* keyname, const char* valuename, RegistryValue* value)
{
    RegistryKey* key = dynamic_cast<RegistryKey*>(getResource(root));
    NFS2_ASSERT(key);
    const char* subk = keyname;
    while (*subk)
    {
        const char* subkEnd;
        for (subkEnd = subk; *subkEnd && *subkEnd != '\\'; ++subkEnd)
            /* nothing */;
        std::string subkey = std::string(subk, subkEnd);
        RegistryKey* newkey = key->getChild(subkey);
        if (!newkey)
        {
            newkey = new RegistryKey();
            allocateResource(newkey);
            key->addChild(subkey, newkey);
        }
        key = newkey;
        subk = subkEnd;
        if (*subk) subk++;
    }
    allocateResource(value);
    key->addValue(valuename, value);
}

x86::reg32 WinApplication::allocateResourceFixed(GenericResource *resource, x86::reg32 fixedHandle)
{
    NFS2_ASSERT((fixedHandle < 0x20000) || (fixedHandle > 0x20000 + 0x30000));
    m_resourceContext->lock();
    resource->setResourceIndex(fixedHandle);
    bool inserted = m_resources.insert(std::make_pair(fixedHandle, resource)).second;
    NFS2_ASSERT(inserted);
    m_resourceContext->unlock();
    return fixedHandle;
}

x86::reg32 WinApplication::allocateResource(GenericResource *resource)
{
    m_resourceContext->lock();
    x86::reg32 resourceIndex = ++s_resourceIndex + 0x20000;
    resource->setResourceIndex(resourceIndex);
    bool inserted = m_resources.insert(std::make_pair(resourceIndex, resource)).second;
    NFS2_ASSERT(inserted);
    m_resourceContext->unlock();
    return resourceIndex;
}

GenericResource* WinApplication::getResource(x86::reg32 resource)
{
    GenericResource* result = nullptr;
    m_resourceContext->lock();
    std::map<x86::reg32, GenericResource*>::iterator it = m_resources.find(resource);
    if (it != m_resources.end())
    {
        result = it->second;
    }
    else
    {
        result = nullptr;
    }
    m_resourceContext->unlock();
    return result;
}

void WinApplication::freeResource(x86::reg32 resource)
{
    if (resource != 0xffffffff)
    {
        m_resourceContext->lock();
        std::map<x86::reg32, GenericResource*>::iterator it = m_resources.find(resource);
        NFS2_ASSERT(it != m_resources.end());
        it->second->setResourceIndex(0);
        delete it->second;
        m_resources.erase(it);
        m_resourceContext->unlock();
    }
}

void WinApplication::registerMethod(x86::reg32 pointer, Method method)
{
    NFS2_ASSERT(pointer != 0);
    NFS2_ASSERT(m_methodNames.find(pointer) == m_methodNames.end());
    m_methodNames[pointer] = std::move(method.name);
    if (m_methodNames.size() * 2 > m_methodSlots.size())
        growMethodTable();
    insertMethod(pointer, method.method);
}

MethodPtr WinApplication::replaceMethod(x86::reg32 pointer, MethodPtr method)
{
    const MethodPtr previous = findMethod(pointer);
    insertMethod(pointer, method);
    return previous;
}

const char* WinApplication::getMethodName(x86::reg32 address) const
{
    std::unordered_map<x86::reg32, std::string>::const_iterator it = m_methodNames.find(address);
    return it != m_methodNames.end() ? it->second.c_str() : nullptr;
}

MethodPtr WinApplication::findMethod(x86::reg32 address) const
{
    const x86::reg32 mask = x86::reg32(m_methodSlots.size()) - 1;
    for (x86::reg32 i = methodSlot(address); ; i = (i + 1) & mask)
    {
        const MethodSlot& slot = m_methodSlots[i];
        if (slot.address == address)
            return slot.method;
        if (slot.address == 0)
            missingMethod(address);
    }
}

// Stores at the first slot holding the address or empty.
void WinApplication::insertMethod(x86::reg32 address, MethodPtr method)
{
    const x86::reg32 mask = x86::reg32(m_methodSlots.size()) - 1;
    x86::reg32 i = methodSlot(address);
    while (m_methodSlots[i].address != 0 && m_methodSlots[i].address != address)
        i = (i + 1) & mask;
    m_methodSlots[i] = MethodSlot{address, method};
}

void WinApplication::growMethodTable()
{
    std::vector<MethodSlot> slots(m_methodSlots.size() * 2, MethodSlot{0, &callAddressZero});
    slots.swap(m_methodSlots);
    --m_methodShift;
    for (std::vector<MethodSlot>::const_iterator it = slots.begin(); it != slots.end(); ++it)
    {
        if (it->address)
            insertMethod(it->address, it->method);
    }
}

int WinApplication::runThread(x86::CPU& cpu, x86::reg32 entryPoint, x86::reg32 parameter, bool threadLock)
{
    if (!m_cpu.terminate)
    {
        win32::MemMap threadStorage(4096);
        win32::MemMap threadStack(1024 * 1024);
        cpu.init(threadStorage.getBlockStart(), entryPoint);
        getMemory<Mutex*>(cpu.efs + 8) = threadLock ? m_executionContext : nullptr;
        LockContext ctx(*this, cpu);
        cpu.esp = threadStack.getBlockStart() + threadStack.getBlockSize() - 8;
        getMemory<x86::reg32>(cpu.esp + 4) = parameter;
        getMemory<x86::reg32>(cpu.esp) = cpu.ip;
        dynamic_call(entryPoint, cpu);
        return int(cpu.eax);
    }
    else
    {
        return 0;
    }
}

void WinApplication::lockContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + 8);
    NFS2_ASSERT(executionContext);
    if (executionContext)
    {
        executionContext->lock();
    }
}

void WinApplication::unlockContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + 8);
    NFS2_ASSERT(executionContext);
    if (executionContext)
    {
        executionContext->unlock();
    }
}

void WinApplication::unmarkContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + 8);
    if (executionContext)
    {
        executionContext->unlock();
        getMemory<Mutex*>(cpu.efs + 8) = nullptr;
    }
    else
    {
        NFS2_ASSERT(false);
    }
}

void WinApplication::terminate()
{
    m_cpu.terminate = true;
}

}
