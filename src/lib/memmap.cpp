#include <lib/memmap.h>
#include <lib/mutex.h>
#include <platform/log.h>
#include <psp2/kernel/sysmem.h>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace win32
{

static const x86::reg32 s_simulatedMemory = 64*1024*1024;
// One byte per 4 KB block of the simulated memory, page-aligned.
static const x86::reg32 s_blockMapSize = (s_simulatedMemory / 4096 + 4095) & ~4095u;
// End of the highest image section (voodoo2a .rsrc at 0xa97200, 0x400 bytes), page-aligned.
static const x86::reg32 s_sectionLimit = 0xa98000;
// Layout: sections, block map, simulated memory.
static const x86::reg32 s_arenaSize = s_sectionLimit + s_blockMapSize + s_simulatedMemory;
static_assert(s_arenaSize % 4096 == 0, "memblock sizes are whole pages");

x86::reg8* MemMap::s_blocks = nullptr;
x86::reg32 MemMap::s_blockCount = 0;
x86::reg32 MemMap::s_blockSize = 0;
x86::reg32 MemMap::s_sectionSize = 0;
x86::reg32 MemMap::s_addressOffset = 0;
Mutex*     MemMap::s_lock = nullptr;
std::vector<MemMap*> MemMap::s_memMaps;

static SceUID s_arenaBlock = -1;

// The guest address space is one zeroed memblock; pages are always accessible.
// Runs before main, so failure is reported by MemMap::init once logging is up.
static x86::reg8* allocArena()
{
    s_arenaBlock = sceKernelAllocMemBlock("nfs3_guest", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, s_arenaSize, nullptr);
    if (s_arenaBlock < 0)
        return nullptr;
    void* base = nullptr;
    sceKernelGetMemBlockBase(s_arenaBlock, &base);
    memset(base, 0, s_arenaSize);
    return static_cast<x86::reg8*>(base);
}

static void freeArena()
{
    if (s_arenaBlock >= 0)
    {
        sceKernelFreeMemBlock(s_arenaBlock);
        s_arenaBlock = -1;
    }
}

x86::reg8* const g_guestBase = allocArena();

MemMap::MemMap(x86::reg32 size)
    :   m_block(x86::reg32(-1))
    ,   m_blockCount(0)
{
    m_blockCount = ((size+4095) & ~4095u) / 4096;
    NFS2_ASSERT(m_blockCount >= 1);
    s_lock->lock();
    for (x86::reg32 b = 0; b < s_blockCount - m_blockCount; /* nothing */)
    {
        bool success = true;
        for (x86::reg32 s = b; s < b + m_blockCount; ++s)
        {
            if (s_blocks[s])
            {
                success = false;
                b = s + 2;
                break;
            }
        }
        if (success)
        {
            m_block = b;
            break;
        }
    }
    if (m_block == x86::reg32(-1))
        pf::fatal("guest memory exhausted (requested %u bytes)", size);
    for (x86::reg32 s = m_block; s < m_block + m_blockCount; ++s)
    {
        s_blocks[s] = 1;
    }
    s_memMaps.push_back(this);
    s_lock->unlock();
}

MemMap::~MemMap()
{
    s_lock->lock();
    for (std::vector<MemMap*>::iterator it = s_memMaps.begin(); it != s_memMaps.end(); ++it)
    {
        if (*it == this)
        {
            s_memMaps.erase(it);
            break;
        }
    }
    for (x86::reg32 s = m_block; s < m_block + m_blockCount; ++s)
    {
        s_blocks[s] = 0;
    }
    s_lock->unlock();
}

MemMap* MemMap::findBlock(x86::reg32 memIndex)
{
    NFS2_ASSERT(memIndex >= s_addressOffset);
    NFS2_ASSERT(memIndex < s_addressOffset + s_simulatedMemory);
    MemMap* result = nullptr;
    s_lock->lock();
    for (std::vector<MemMap*>::iterator it = s_memMaps.begin(); it != s_memMaps.end(); ++it)
    {
        if ((*it)->getBlockStart() <= memIndex && (*it)->getBlockStart() + (*it)->getBlockSize() > memIndex)
        {
            NFS2_ASSERT(!result);
            result = *it;
            break;
        }
    }
    s_lock->unlock();
    return result;
}

void MemMap::init(x86::reg32 baseAddress, const std::vector<Section>& sections)
{
    if (!g_guestBase)
        pf::fatal("guest memory allocation of %u bytes failed: 0x%08x", s_arenaSize, s_arenaBlock);
    pf::logInfo("guest memory: %u bytes at %p", s_arenaSize, static_cast<void*>(g_guestBase));
    s_blockCount = s_simulatedMemory / 4096;
    s_blockSize = s_blockMapSize;
    x86::reg32 maxEnd = 0;
    for (std::vector<Section>::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        x86::reg32 end = it->baseAddress + it->size;
        if (end > maxEnd)
            maxEnd = end;
    }
    s_sectionSize = (maxEnd + 4095) & ~4095u;
    if (s_sectionSize > s_sectionLimit)
        pf::fatal("image sections end at 0x%08x, past the guest arena limit 0x%08x", s_sectionSize, s_sectionLimit);
    s_blocks = g_guestBase + s_sectionSize;
    s_addressOffset = s_sectionSize + s_blockSize;
    for (std::vector<Section>::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        NFS2_ASSERT(it->baseAddress >= baseAddress);
        NFS2_ASSERT(it->baseAddress + it->size <= s_sectionSize);
        if (it->data)
        {
            memcpy(g_guestBase + it->baseAddress, it->data, it->size);
        }
    }
    s_lock = new Mutex("MemMap");
}

void MemMap::fini()
{
    std::vector<MemMap*> memMapsCopy(s_memMaps);
    for (std::vector<MemMap*>::iterator it = memMapsCopy.begin(); it != memMapsCopy.end(); ++it)
    {
        delete *it;
    }
    NFS2_ASSERT(s_memMaps.empty());
    s_blocks = nullptr;
    s_blockCount = 0;
    s_blockSize = 0;
    s_addressOffset = 0;
    delete s_lock;
    s_lock = nullptr;
    freeArena();
}

void MemMap::fillDebugGraph(x86::reg16* graph)
{
    s_lock->lock();
    const x86::reg32 blocksPerPixel = (s_blockCount+639)/640;
    for (x86::reg32 b = 0; b < s_blockCount; b += blocksPerPixel)
    {
        x86::reg16 c = 0;
        for (x86::reg32 b1 = b; b1 < b+blocksPerPixel; ++b1)
        {
            if (s_blocks[b1])
                c++;
        }
        x86::reg16 r = (0x1f*c/blocksPerPixel) << (5+6);
        x86::reg16 g = (0x3f*(blocksPerPixel-c)/blocksPerPixel) << 5;
        graph[b/blocksPerPixel + 640*0] = r|g;
        graph[b/blocksPerPixel + 640*1] = r|g;
        graph[b/blocksPerPixel + 640*2] = r|g;
    }
    s_lock->unlock();
}

void MemMap::fillDebugGraph(x86::reg32* graph)
{
    s_lock->lock();
    const x86::reg32 blocksPerPixel = (s_blockCount+639)/640;
    for (x86::reg32 b = 0; b < s_blockCount; b += blocksPerPixel)
    {
        x86::reg16 c = 0;
        for (x86::reg32 b1 = b; b1 < b+blocksPerPixel; ++b1)
        {
            if (s_blocks[b1])
                c++;
        }
        x86::reg32 r = (0xff*c/blocksPerPixel) << 24;
        x86::reg32 g = (0xff*(blocksPerPixel-c)/blocksPerPixel) << 16;
        graph[b/blocksPerPixel + 640*0] = r|g;
        graph[b/blocksPerPixel + 640*1] = r|g;
        graph[b/blocksPerPixel + 640*2] = r|g;
    }
    s_lock->unlock();
}

}
