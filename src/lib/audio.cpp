#include <lib/audio.h>
#include <platform/log.h>
#include <psp2/audioout.h>
#include <algorithm>
#include <cstring>

namespace win32
{

static const x86::reg32 s_defaultBufferBytes = 4096 * AudioDevice::s_channelCount * sizeof(x86::sreg16);

AudioDevice::AudioDevice()
    :   m_port(-1)
    ,   m_mutex("nfs3_audio", false)
    ,   m_running(false)
    ,   m_thread(nullptr)
{
    m_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, s_grain, s_sampleRate, SCE_AUDIO_OUT_MODE_STEREO);
    if (m_port < 0)
    {
        pf::logError("sceAudioOutOpenPort failed: 0x%08x", m_port);
        return;
    }
    int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(m_port, SceAudioOutChannelFlag(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volume);
    m_running = true;
    m_thread = new pf::Thread("nfs3_audio", &threadEntry, this, pf::Thread::PriorityHigh, 64 * 1024, pf::Thread::CpuAll);
    m_thread->start();
}

AudioDevice::~AudioDevice()
{
    if (m_thread)
    {
        m_running = false;
        m_thread->join();
        delete m_thread;
    }
    if (m_port >= 0)
    {
        sceAudioOutOutput(m_port, nullptr);
        sceAudioOutReleasePort(m_port);
    }
}

void AudioDevice::play(AudioBuffer* buffer)
{
    pf::LockGuard guard(m_mutex);
    if (std::find(m_playingBuffers.begin(), m_playingBuffers.end(), buffer) == m_playingBuffers.end())
        m_playingBuffers.push_back(buffer);
}

void AudioDevice::stop(AudioBuffer* buffer)
{
    pf::LockGuard guard(m_mutex);
    m_playingBuffers.erase(std::remove(m_playingBuffers.begin(), m_playingBuffers.end(), buffer), m_playingBuffers.end());
}

int AudioDevice::threadEntry(void* self)
{
    static_cast<AudioDevice*>(self)->run();
    return 0;
}

void AudioDevice::run()
{
    const x86::reg32 samples = s_grain * s_channelCount;
    while (m_running)
    {
        memset(m_accumulator, 0, sizeof(m_accumulator));
        {
            pf::LockGuard guard(m_mutex);
            for (std::vector<AudioBuffer*>::iterator it = m_playingBuffers.begin(); it != m_playingBuffers.end(); ++it)
                (*it)->mixInto(m_accumulator, s_grain);
        }
        for (x86::reg32 i = 0; i < samples; ++i)
        {
            x86::sreg32 v = m_accumulator[i];
            m_output[i] = x86::sreg16(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        sceAudioOutOutput(m_port, m_output);
    }
}


AudioBuffer::AudioBuffer(AudioDevice* device, WinApplication* app, x86::reg32 bufferSize)
    :   m_device(device)
    ,   m_bufferSize(bufferSize ? bufferSize : s_defaultBufferBytes)
    ,   m_memmap(new MemMap(m_bufferSize))
    ,   m_bufferMap(new MemMap(m_bufferSize))
    ,   m_buffer(&app->getMemory<x86::sreg16>(m_memmap->getBlockStart()))
    ,   m_playStart(0)
    ,   m_playStop(0)
    ,   m_primary(bufferSize == 0)
{
}

x86::reg32 AudioBuffer::bufferSize() const
{
    return m_bufferSize;
}

AudioBuffer::~AudioBuffer()
{
    m_device->stop(this);
    delete m_bufferMap;
    delete m_memmap;
}

void AudioBuffer::play()
{
    m_device->play(this);
}

void AudioBuffer::stop()
{
    m_device->stop(this);
}

void AudioBuffer::mixInto(x86::sreg32* accumulator, x86::reg32 frames)
{
    x86::reg32 start = m_playStart;
    x86::reg32 stop = start + frames * AudioDevice::s_channelCount * sizeof(x86::sreg16);
    if (stop > m_playStop)
        stop = m_playStop;
    if (stop <= start)
        return;
    x86::reg32 sampleCount = (stop - start) / sizeof(x86::sreg16);
    x86::reg32 bufferSamples = m_bufferSize / sizeof(x86::sreg16);
    x86::reg32 index = (start % m_bufferSize) / sizeof(x86::sreg16);
    for (x86::reg32 i = 0; i < sampleCount; ++i)
    {
        accumulator[i] += m_buffer[index];
        if (++index == bufferSamples)
            index = 0;
    }
    m_playStart = stop;
}

x86::reg32 AudioBuffer::lock()
{
    return m_memmap->getBlockStart();
}

void AudioBuffer::unlock(x86::reg32 bufferWritten)
{
    pf::LockGuard guard(m_device->mutex());
    m_playStop += bufferWritten;
}


}
