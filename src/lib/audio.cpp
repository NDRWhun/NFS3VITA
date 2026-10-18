#include <lib/audio.h>
#include <platform/log.h>
#include <psp2/audioout.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace win32
{

namespace
{

// Hundredths of a decibel to a Q15 gain; -10000 and below is silence.
x86::sreg32 attenuationToGain(x86::sreg32 hundredthsDb)
{
    if (hundredthsDb <= -10000)
        return 0;
    if (hundredthsDb >= 0)
        return AudioDevice::s_unityGain;
    return x86::sreg32(std::pow(10.0f, float(hundredthsDb) / 2000.0f) * float(AudioDevice::s_unityGain) + 0.5f);
}

inline x86::sreg32 sampleValue(x86::reg8 sample)    { return (x86::sreg32(sample) - 128) * 256; }
inline x86::sreg32 sampleValue(x86::sreg16 sample)  { return sample; }

// Mixes up to `frames` output frames from at most `available` source frames starting at `index` in a
// ring of `ringFrames`, interpolating linearly between source frames. Advances index and phase and
// returns the number of source frames consumed.
template <typename Sample, x86::reg32 Channels>
x86::reg32 mixFrames(const Sample* ring, x86::reg32 ringFrames, x86::reg32& index, x86::reg32 available,
                     x86::reg32& phase, x86::reg32 step, x86::sreg32 gainLeft, x86::sreg32 gainRight,
                     x86::sreg32* out, x86::reg32 frames)
{
    x86::reg32 consumed = 0;
    for (x86::reg32 i = 0; i < frames && consumed < available; ++i, out += AudioDevice::s_channelCount)
    {
        const Sample* current = ring + index * Channels;
        const Sample* next = current;
        if (consumed + 1 < available)
            next = index + 1 == ringFrames ? ring : current + Channels;
        const x86::sreg32 fraction = x86::sreg32(phase >> 1);
        x86::sreg32 left = sampleValue(current[0]);
        left += ((sampleValue(next[0]) - left) * fraction) >> 15;
        x86::sreg32 right = left;
        if constexpr (Channels == 2)
        {
            right = sampleValue(current[1]);
            right += ((sampleValue(next[1]) - right) * fraction) >> 15;
        }
        out[0] += (left * gainLeft) >> 15;
        out[1] += (right * gainRight) >> 15;

        phase += step;
        x86::reg32 advance = phase >> 16;
        phase &= 0xffff;
        if (advance > available - consumed)
        {
            advance = available - consumed;
            phase = 0;
        }
        consumed += advance;
        index += advance;
        while (index >= ringFrames)
            index -= ringFrames;
    }
    return consumed;
}

// Advances index and phase like mixFrames without reading samples.
x86::reg32 skipFrames(x86::reg32 ringFrames, x86::reg32& index, x86::reg32 available,
                      x86::reg32& phase, x86::reg32 step, x86::reg32 frames)
{
    const x86::reg64 end = x86::reg64(phase) + x86::reg64(step) * frames;
    x86::reg32 consumed = x86::reg32(end >> 16);
    phase = x86::reg32(end & 0xffff);
    if (consumed >= available)
    {
        consumed = available;
        phase = 0;
    }
    index = (index + consumed) % ringFrames;
    return consumed;
}

}

AudioDevice::AudioDevice()
    :   m_port(-1)
    ,   m_mutex("nfs3_audio", false)
    ,   m_running(false)
    ,   m_thread(nullptr)
{
    m_masterGain[0] = s_unityGain;
    m_masterGain[1] = s_unityGain;
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

void AudioDevice::play(AudioBuffer* buffer, bool looping)
{
    pf::LockGuard guard(m_mutex);
    buffer->m_looping = looping;
    if (!buffer->m_playing)
    {
        buffer->m_playing = true;
        m_playingBuffers.push_back(buffer);
    }
}

void AudioDevice::stop(AudioBuffer* buffer)
{
    pf::LockGuard guard(m_mutex);
    buffer->m_playing = false;
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
            for (std::vector<AudioBuffer*>::iterator it = m_playingBuffers.begin(); it != m_playingBuffers.end();)
            {
                if ((*it)->mixInto(m_accumulator, s_grain, m_masterGain))
                    ++it;
                else
                    it = m_playingBuffers.erase(it);
            }
        }
        for (x86::reg32 i = 0; i < samples; ++i)
        {
            x86::sreg32 v = m_accumulator[i];
            m_output[i] = x86::sreg16(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        sceAudioOutOutput(m_port, m_output);
    }
}


AudioBuffer::AudioBuffer(AudioDevice* device, WinApplication* app, x86::reg32 bufferSize, const AudioFormat& format, bool primary)
    :   m_device(device)
    ,   m_bufferSize(bufferSize)
    ,   m_memmap(new MemMap(bufferSize))
    ,   m_data(&app->getMemory<x86::reg8>(m_memmap->getBlockStart()))
    ,   m_format(format)
    ,   m_position(0)
    ,   m_staleFrames(0)
    ,   m_phase(0)
    ,   m_step(0)
    ,   m_frequency(0)
    ,   m_volume(0)
    ,   m_pan(0)
    ,   m_playing(false)
    ,   m_looping(false)
    ,   m_primary(primary)
{
    m_gain[0] = AudioDevice::s_unityGain;
    m_gain[1] = AudioDevice::s_unityGain;
    // A new buffer is zeroed, as on Windows.
    memset(&app->getMemory<x86::reg8>(m_memmap->getBlockStart()), 0, m_bufferSize);
    updateStep();
}

AudioBuffer::~AudioBuffer()
{
    m_device->stop(this);
    delete m_memmap;
}

void AudioBuffer::play(bool looping)
{
    m_device->play(this, looping);
}

void AudioBuffer::stop()
{
    m_device->stop(this);
}

bool AudioBuffer::playing() const
{
    pf::LockGuard guard(m_device->mutex());
    return m_playing;
}

x86::reg32 AudioBuffer::lock() const
{
    return m_memmap->getBlockStart();
}

void AudioBuffer::unlock()
{
    pf::LockGuard guard(m_device->mutex());
    m_staleFrames = 0;
}

x86::reg32 AudioBuffer::position() const
{
    pf::LockGuard guard(m_device->mutex());
    return m_position;
}

void AudioBuffer::setPosition(x86::reg32 offset)
{
    pf::LockGuard guard(m_device->mutex());
    m_position = offset - offset % m_format.frameBytes();
    m_phase = 0;
}

void AudioBuffer::setFormat(const AudioFormat& format)
{
    pf::LockGuard guard(m_device->mutex());
    m_format = format;
    m_position -= m_position % format.frameBytes();
    m_phase = 0;
    updateStep();
}

void AudioBuffer::setVolume(x86::sreg32 volume)
{
    pf::LockGuard guard(m_device->mutex());
    m_volume = volume;
    updateGain();
}

void AudioBuffer::setPan(x86::sreg32 pan)
{
    pf::LockGuard guard(m_device->mutex());
    m_pan = pan;
    updateGain();
}

void AudioBuffer::setFrequency(x86::reg32 frequency)
{
    pf::LockGuard guard(m_device->mutex());
    m_frequency = frequency;
    updateStep();
}

// Pan attenuates the opposite channel. The primary buffer's volume and pan apply to the whole mix.
void AudioBuffer::updateGain()
{
    x86::sreg32* gain = m_primary ? m_device->m_masterGain : m_gain;
    gain[0] = attenuationToGain(m_volume - std::max<x86::sreg32>(m_pan, 0));
    gain[1] = attenuationToGain(m_volume + std::min<x86::sreg32>(m_pan, 0));
}

void AudioBuffer::updateStep()
{
    m_step = x86::reg32((x86::reg64(frequency()) << 16) / AudioDevice::s_sampleRate);
}

bool AudioBuffer::mixInto(x86::sreg32* accumulator, x86::reg32 frames, const x86::sreg32* masterGain)
{
    const x86::reg32 frameBytes = m_format.frameBytes();
    const x86::reg32 ringFrames = m_bufferSize / frameBytes;
    x86::reg32 index = m_position / frameBytes;
    // Frames left before a non-looping buffer ends.
    const x86::reg32 available = m_looping ? ~x86::reg32(0) : ringFrames - index;
    // The primary buffer is only written as a write-primary stream; a lap without new data plays silent.
    const bool stale = m_primary && m_staleFrames >= ringFrames;
    const x86::sreg32 left = stale ? 0 : (m_gain[0] * masterGain[0]) >> 15;
    const x86::sreg32 right = stale ? 0 : (m_gain[1] * masterGain[1]) >> 15;
    x86::reg32 consumed;
    if (!left && !right)
        consumed = skipFrames(ringFrames, index, available, m_phase, m_step, frames);
    else if (m_format.bitsPerSample == 16)
    {
        const x86::sreg16* ring = reinterpret_cast<const x86::sreg16*>(m_data);
        if (m_format.channels == 2)
            consumed = mixFrames<x86::sreg16, 2>(ring, ringFrames, index, available, m_phase, m_step, left, right, accumulator, frames);
        else
            consumed = mixFrames<x86::sreg16, 1>(ring, ringFrames, index, available, m_phase, m_step, left, right, accumulator, frames);
    }
    else
    {
        if (m_format.channels == 2)
            consumed = mixFrames<x86::reg8, 2>(m_data, ringFrames, index, available, m_phase, m_step, left, right, accumulator, frames);
        else
            consumed = mixFrames<x86::reg8, 1>(m_data, ringFrames, index, available, m_phase, m_step, left, right, accumulator, frames);
    }
    m_staleFrames = std::min(m_staleFrames + consumed, ringFrames);
    m_position = index * frameBytes;
    if (consumed < available)
        return true;
    m_playing = false;
    m_phase = 0;
    return false;
}


}
