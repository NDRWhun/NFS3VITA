#include <lib/glidetmu.h>
#include <platform/log.h>
#include <cstring>

namespace win32
{

static const x86::reg32 s_atlasSlotCount = 2048;
static const x86::reg32 s_atlasLodCount = 9;

GlideTMU::GlideTMU(x86::reg32 atlasSize)
    :   m_videoMemorySize(0)
    ,   m_textureSlots(new GlTextureSlot*[s_atlasLodCount])
    ,   m_textureSlotPool(new GlTextureSlot[s_atlasSlotCount])
    ,   m_firstFreeTextureSlot(m_textureSlotPool)
    ,   m_quarantine(nullptr)
    ,   m_textureMem(new GlTextureSlot*[atlasSize])
{
    for (x86::reg32 i = 0; i < s_atlasLodCount; ++i)
    {
        m_textureSlots[i] = nullptr;
    }
    for (x86::reg32 i = 0; i < s_atlasSlotCount; ++i)
    {
        m_textureSlotPool[i].next = (i < s_atlasSlotCount - 1) ? &m_textureSlotPool[i + 1] : nullptr;
        m_textureSlotPool[i].lod = 0;
        m_textureSlotPool[i].freedFrame = 0;
    }
    for (x86::reg32 i = 0;i < (atlasSize/256) * (atlasSize/256); ++i)
    {
        GlTextureSlot* slot = allocateTextureSlot();
        slot->lod = 0;
        slot->x = (256 * i) % atlasSize;
        slot->y = 256 * (256 * i / atlasSize);
        slot->next = m_textureSlots[0];
        m_textureSlots[0] = slot;
    }
    memset(m_textureMem, 0, atlasSize*sizeof(GlTextureSlot*));
}

GlideTMU::~GlideTMU()
{
    delete[] m_textureSlots;
    delete[] m_textureSlotPool;
    delete[] m_textureMem;
}

x86::reg32 GlideTMU::textureMemStart() const
{
    return 0;
}

x86::reg32 GlideTMU::textureMemEnd() const
{
    return 4096*1024;
}

GlTextureSlot** GlideTMU::getTextureInfo(x86::reg32 address)
{
    return m_textureMem + address / sizeof(GlTextureSlot*);
}

GlTextureSlot* GlideTMU::allocateTextureSlot()
{
    GlTextureSlot* result = m_firstFreeTextureSlot;
    m_firstFreeTextureSlot = result->next;
    return result;
}

void GlideTMU::freeTextureSlot(GlTextureSlot* slot)
{
    slot->next = m_firstFreeTextureSlot;
    m_firstFreeTextureSlot = slot;
}

void GlideTMU::breakdownTextureSlot(GlTextureSlot* slot)
{
    slot->lod++;
    x86::reg32 size = 256 >> slot->lod;
    slot->next = m_textureSlots[slot->lod];
    m_textureSlots[slot->lod] = slot;
    GlTextureSlot* s = allocateTextureSlot();
    s->lod = slot->lod;
    s->x = slot->x + size;
    s->y = slot->y;
    s->next = m_textureSlots[slot->lod];
    m_textureSlots[slot->lod] = s;
    s = allocateTextureSlot();
    s->lod = slot->lod;
    s->x = slot->x;
    s->y = slot->y + size;
    s->next = m_textureSlots[slot->lod];
    m_textureSlots[slot->lod] = s;
    s = allocateTextureSlot();
    s->lod = slot->lod;
    s->x = slot->x + size;
    s->y = slot->y + size;
    s->next = m_textureSlots[slot->lod];
    m_textureSlots[slot->lod] = s;
}

// Moves quarantined slots whose frame completed (or all, when forced) back to the free lists.
void GlideTMU::releaseQuarantine(x86::reg32 completedFrame, bool force)
{
    GlTextureSlot** link = &m_quarantine;
    while (*link)
    {
        GlTextureSlot* slot = *link;
        if (force || slot->freedFrame <= completedFrame)
        {
            *link = slot->next;
            slot->next = m_textureSlots[slot->lod];
            m_textureSlots[slot->lod] = slot;
        }
        else
        {
            link = &slot->next;
        }
    }
}

GlTextureSlot* GlideTMU::reserveTextureSlot(x86::reg32 lod, x86::reg32 completedFrame)
{
    releaseQuarantine(completedFrame, false);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        GlTextureSlot* result = m_textureSlots[lod];
        if (!result)
        {
            for (x86::sreg32 parentLod = lod-1; parentLod >= 0; --parentLod)
            {
                GlTextureSlot* parent = m_textureSlots[parentLod];
                if (parent)
                {
                    m_textureSlots[parentLod] = parent->next;
                    for (x86::reg32 childLod = parentLod; childLod < lod-1; ++childLod)
                    {
                        breakdownTextureSlot(parent);
                        parent = m_textureSlots[childLod + 1];
                        m_textureSlots[childLod + 1] = parent->next;
                    }
                    breakdownTextureSlot(parent);
                    break;
                }
            }
            result = m_textureSlots[lod];
        }
        if (result)
        {
            m_textureSlots[lod] = result->next;
            return result;
        }
        // Atlas exhausted: reuse quarantined slots and accept a possible one-frame glitch.
        pf::logError("texture atlas full, recycling quarantined slots");
        releaseQuarantine(completedFrame, true);
    }
    pf::fatal("texture atlas exhausted");
}

void GlideTMU::returnTextureSlot(GlTextureSlot* slot, x86::reg32 frameIndex)
{
    slot->freedFrame = frameIndex;
    slot->next = m_quarantine;
    m_quarantine = slot;
    /* todo: consolidate */
}

}
