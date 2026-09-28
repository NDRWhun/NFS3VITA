#ifndef LIB_AUDIO_H_
#define LIB_AUDIO_H_
#include <lib/winapp.h>
#include <lib/memmap.h>
#include <platform/sync.h>
#include <vector>

namespace win32
{

class AudioBuffer;

// PCM sample layout: 1 or 2 channels, 8-bit unsigned or 16-bit signed samples.
struct AudioFormat
{
    x86::reg32 channels;
    x86::reg32 bitsPerSample;
    x86::reg32 sampleRate;

    x86::reg32 frameBytes() const { return channels * bitsPerSample / 8; }
};

// SceAudioOut port fed by a mixer thread; 22050 Hz, 16-bit stereo output.
class AudioDevice : public GenericResource
{
    friend class AudioBuffer;
public:
    static constexpr x86::reg32 s_channelCount = 2;
    static constexpr x86::reg32 s_sampleRate = 22050;
    static constexpr x86::reg32 s_grain = 1024;             // frames per output block
    static constexpr x86::sreg32 s_unityGain = 1 << 15;     // Q15 gains
    static constexpr AudioFormat s_outputFormat = { s_channelCount, 16, s_sampleRate };

    AudioDevice();
    ~AudioDevice();

    void play(AudioBuffer* buffer, bool looping);
    void stop(AudioBuffer* buffer);
    pf::Mutex& mutex() { return m_mutex; }

private:
    static int threadEntry(void* self);
    void run();

private:
    int                         m_port;
    pf::Mutex                   m_mutex;
    std::vector<AudioBuffer*>   m_playingBuffers;
    x86::sreg32                 m_masterGain[s_channelCount];   // primary buffer volume and pan
    volatile bool               m_running;
    pf::Thread*                 m_thread;
    x86::sreg32                 m_accumulator[s_grain * s_channelCount];
    x86::sreg16                 m_output[s_grain * s_channelCount];
};

// DirectSound-style ring buffer in guest memory. While playing, the mixer reads it from the play
// cursor at the buffer's frequency, resampled to the output rate: a looping buffer wraps until it is
// stopped, any other buffer stops at its end and rewinds. A primary buffer not written for a whole
// lap plays silence. Volume and pan are in hundredths of a decibel.
class AudioBuffer : public GenericResource
{
    friend class AudioDevice;
public:
    static constexpr x86::reg32 s_primaryBufferBytes = 4096 * AudioDevice::s_channelCount * sizeof(x86::sreg16);

    AudioBuffer(AudioDevice* device, WinApplication* app, x86::reg32 bufferSize, const AudioFormat& format, bool primary);
    ~AudioBuffer();

    x86::reg32 bufferSize() const       { return m_bufferSize; }
    bool primary() const                { return m_primary; }
    const AudioFormat& format() const   { return m_format; }
    x86::sreg32 volume() const          { return m_volume; }
    x86::sreg32 pan() const             { return m_pan; }
    x86::reg32 frequency() const        { return m_frequency ? m_frequency : m_format.sampleRate; }
    bool looping() const                { return m_looping; }

    void play(bool looping);
    void stop();
    bool playing() const;
    // Guest address of the buffer memory.
    x86::reg32 lock() const;
    // The game has written new data.
    void unlock();
    // Byte offset of the next frame the mixer reads.
    x86::reg32 position() const;
    void setPosition(x86::reg32 offset);

    void setFormat(const AudioFormat& format);
    void setVolume(x86::sreg32 volume);
    void setPan(x86::sreg32 pan);
    // 0 selects the format's sample rate.
    void setFrequency(x86::reg32 frequency);

private:
    // Adds `frames` output frames into the stereo accumulator; returns false once a non-looping
    // buffer has reached its end.
    bool mixInto(x86::sreg32* accumulator, x86::reg32 frames, const x86::sreg32* masterGain);
    void updateGain();
    void updateStep();

private:
    AudioDevice*        m_device;
    x86::reg32          m_bufferSize;
    MemMap*             m_memmap;
    const x86::reg8*    m_data;
    AudioFormat         m_format;
    x86::reg32          m_position;     // play cursor in bytes, frame aligned
    x86::reg32          m_staleFrames;  // frames mixed since the last unlock, up to one lap
    x86::reg32          m_phase;        // fraction of the current source frame, 16 bits
    x86::reg32          m_step;         // source frames per output frame, 16.16 fixed point
    x86::reg32          m_frequency;
    x86::sreg32         m_volume;
    x86::sreg32         m_pan;
    x86::sreg32         m_gain[AudioDevice::s_channelCount];
    bool                m_playing;
    bool                m_looping;
    bool                m_primary;
};

}

#endif /*!LIB_AUDIO_H_*/
