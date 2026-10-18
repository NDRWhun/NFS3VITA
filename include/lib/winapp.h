#ifndef LIB_WINAPP_H_
#define LIB_WINAPP_H_

#include    <x86.h>
#include    <cpu.h>
#include    <fpu.h>
#include    <lib/memmap.h>
#include    <atomic>
#include    <cstring>
#include    <map>
#include    <string>
#include    <unordered_map>
#include    <vector>

namespace win32
{

class WinApplication;
class Mutex;
class RegistryValue;

typedef void (*MethodPtr)(WinApplication* application, x86::CPU& cpu);

struct Method
{
    std::string name;
    MethodPtr method;
};

// One slot of the dynamic_call table; address 0 marks an empty slot.
struct MethodSlot
{
    x86::reg32  address;
    MethodPtr   method;
};

class GenericResource
{
    friend class WinApplication;
public:
    virtual ~GenericResource();
    x86::reg32 getResourceIndex() const;

protected:
    GenericResource();

    void setResourceIndex(x86::reg32 resourceIndex);

protected:
    x86::reg32  m_resourceIndex;
};

template< typename T >
class Packed
{
public:
    explicit Packed(x86::reg32 value = 0) : m_ptr(value) {}
    operator x86::reg32() const             { return m_ptr; }

private:
    x86::reg32  m_ptr;
};

template< typename T >
struct MemoryAccessor
{
    friend class WinApplication;
private:
    MemoryAccessor(x86::reg8* address) : address(address) {}
    x86::reg8* address;

public:
    MemoryAccessor(const MemoryAccessor& other) = default;
    MemoryAccessor& operator=(const MemoryAccessor& other)
    {
        if (this != &other)
        {
            T value = other;
            memcpy(address, &value, sizeof(T));
        }
        return *this;
    }
    operator T() const { T result; memcpy(&result, address, sizeof(T)); return result; }
    MemoryAccessor& operator=(T value) { memcpy(address, &value, sizeof(T)); return *this; }
    T operator-=(T value) { T result = *this; result -= value; memcpy(address, &result, sizeof(T)); return result; }
    T operator+=(T value) { T result = *this; result += value; memcpy(address, &result, sizeof(T)); return result; }
    T operator|=(T value) { T result = *this; result |= value; memcpy(address, &result, sizeof(T)); return result; }
    T operator&=(T value) { T result = *this; result &= value; memcpy(address, &result, sizeof(T)); return result; }
    T operator^=(T value) { T result = *this; result ^= value; memcpy(address, &result, sizeof(T)); return result; }
    T operator>>=(T value) { T result = *this; result >>= value; memcpy(address, &result, sizeof(T)); return result; }
    T operator<<=(T value) { T result = *this; result <<= value; memcpy(address, &result, sizeof(T)); return result; }
    T operator++(int) { T result = *this; T newValue = result; newValue++; memcpy(address, &newValue, sizeof(T)); return result; }
    T operator--(int) { T result = *this; T newValue = result; newValue--; memcpy(address, &newValue, sizeof(T)); return result; }
    T operator++() { T result = *this; T newValue = result; newValue++; memcpy(address, &newValue, sizeof(T)); return newValue; }
    T operator--() { T result = *this; T newValue = result; newValue--; memcpy(address, &newValue, sizeof(T)); return newValue; }
    T* operator&() { return reinterpret_cast<T*>(address); }
};

template<>
struct MemoryAccessor<x86::IEEEf80>
{
    friend class WinApplication;
private:
    MemoryAccessor(x86::reg8* address) : address(address) {}
    x86::reg8* address;

public:
    MemoryAccessor(const MemoryAccessor& other) = default;
    MemoryAccessor& operator=(const MemoryAccessor& other) = delete;
    operator x86::Float() const { x86::IEEEf80Data result; memcpy(&result, address, sizeof(x86::IEEEf80Data)); return double(x86::IEEEf80(result)); }
    operator x86::IEEEf80() const { x86::IEEEf80Data result; memcpy(&result, address, sizeof(x86::IEEEf80Data)); return x86::IEEEf80(result); }
    MemoryAccessor& operator=(x86::Float value) { x86::IEEEf80 f80value(value); memcpy(address, &f80value.data, sizeof(x86::IEEEf80Data)); return *this; }
    MemoryAccessor& operator=(x86::IEEEf80 value) { memcpy(address, &value.data, sizeof(x86::IEEEf80Data)); return *this; }
};

template<>
struct MemoryAccessor<void>
{
    friend class WinApplication;
private:
    MemoryAccessor(x86::reg8* address) : address(address) {}
    x86::reg8* address;

public:
    MemoryAccessor(const MemoryAccessor& other) = default;
    MemoryAccessor& operator=(const MemoryAccessor& other) = delete;
    void* operator&() { return reinterpret_cast<void*>(address); }
};

template<>
struct MemoryAccessor<const void>
{
    friend class WinApplication;
private:
    MemoryAccessor(x86::reg8* address) : address(address) {}
    x86::reg8* address;

public:
    MemoryAccessor(const MemoryAccessor& other) = default;
    MemoryAccessor& operator=(const MemoryAccessor& other) = delete;
    const void* operator&() { return reinterpret_cast<const void*>(address); }
};

class WinApplication
{
public:
    GenericResource* getResource(x86::reg32 resource);
    // False if no resource has the handle.
    bool freeResource(x86::reg32 resource);
    x86::reg32 allocateResource(GenericResource* resource);
    x86::reg32 getAppName() const { return m_appName->getBlockStart(); }
    x86::reg32 getAppNameW() const { return m_appNameW->getBlockStart(); }
    x86::reg32 getEnv() const { return m_env->getBlockStart(); }
    char* getAppNameRaw() { return &getMemory<char>(m_appName->getBlockStart()); }

    void addRegistryKey(x86::reg32 root, const char* keyname, const char* valuename, RegistryValue* value);

    template< typename T >
    inline MemoryAccessor<T> getMemory(x86::reg32 address) { return MemoryAccessor<T>{g_guestBase + address}; }

    void registerMethod(x86::reg32 pointer, Method method);
    // Replaces the method registered at a guest address and returns the previous one. Only before guest code
    // runs: dynamic_call reads the table without a lock.
    MethodPtr replaceMethod(x86::reg32 pointer, MethodPtr method);
    // Registered name of the method at a guest address, or nullptr.
    const char* getMethodName(x86::reg32 address) const;

    // A preempting thread (a timer callback, or a guest thread with a priority above normal, as Windows schedules
    // them) gets the execution lock at the next preemption point of the thread holding it.
    int runThread(x86::CPU& cpu, x86::reg32 entryPoint, x86::reg32 parameter = 0, bool threadLock = true,
                  bool preempts = false);

    void lockContext(const x86::CPU& cpu);
    void unlockContext(const x86::CPU& cpu);

    // Preemption point for the thread holding the execution lock: if a preempting thread waits for the lock, a
    // thread that does not preempt releases it and queues again. Every dynamic_call is one.
    inline void allowPreemption(const x86::CPU& cpu)
    {
        if (__builtin_expect(m_preemptWaiters.load(std::memory_order_relaxed) != 0, 0))
            yieldContext(cpu);
    }

    void terminate();

public:
    inline void dynamic_call(uint32_t address, x86::CPU& cpu)
    {
        allowPreemption(cpu);
        const MethodSlot& slot = m_methodSlots[methodSlot(address)];
        const MethodPtr method = slot.address == address ? slot.method : findMethod(address);
        method(this, cpu);
    }

protected:
    WinApplication(const char* appName, x86::reg32 baseAddress, const std::vector<Section>& sections);
    ~WinApplication();

    x86::reg32 allocateResourceFixed(GenericResource *resource, x86::reg32 fixedHandle);

private:
    static constexpr x86::reg32 MethodHashMultiplier = 0x9e3779b1u;

    // Home slot: the top bits of a multiplicative hash.
    x86::reg32 methodSlot(x86::reg32 address) const { return (address * MethodHashMultiplier) >> m_methodShift; }
    // Probes past the home slot; logs and traps if nothing is registered at the address.
    MethodPtr findMethod(x86::reg32 address) const __attribute__((cold));
    void insertMethod(x86::reg32 address, MethodPtr method);
    void growMethodTable();
    void yieldContext(const x86::CPU& cpu) __attribute__((cold));

private:
    MemMap*                                 m_appName;
    MemMap*                                 m_appNameW;
    MemMap*                                 m_env;
    std::map<x86::reg32, GenericResource*>  m_resources;
    Mutex*                                  m_executionContext;
    std::atomic<int>                        m_preemptWaiters;   // preempting threads waiting for the lock
    int                                     m_contextDepth;     // lock count of the execution context owner
    Mutex*                                  m_resourceContext;
    // Open addressing with linear probing; power-of-two size, at most half full.
    std::vector<MethodSlot>                 m_methodSlots;
    x86::reg32                              m_methodShift;  // 32 - log2(m_methodSlots.size())
    std::unordered_map<x86::reg32, std::string> m_methodNames;

protected:
    x86::CPU                                m_cpu;
};

class LockContext
{
public:
    LockContext(WinApplication& app, const x86::CPU& cpu)
        :   m_application(app)
        ,   m_cpu(cpu)
    {
        m_application.lockContext(cpu);
    }
    ~LockContext()
    {
        m_application.unlockContext(m_cpu);
    }
private:
    WinApplication& m_application;
    const x86::CPU& m_cpu;
};

}

#endif
