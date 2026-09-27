#ifndef LIB_AUDIO_H_
#define LIB_AUDIO_H_
#include <lib/winapp.h>
#include <lib/memmap.h>
#include <platform/sync.h>
#include <vector>

namespace win32
{

class AudioBuffer;

// SceAudioOut port fed by a mixer thread; 22050 Hz, 16-bit stereo like the game's buffers.
class AudioDevice : public GenericResource
{
public:
    static const x86::reg32 s_channelCount = 2;
    static const x86::reg32 s_sampleRate = 22050;
    static const x86::reg32 s_grain = 1024; // frames per output block

    AudioDevice();
    ~AudioDevice();

    void play(AudioBuffer* buffer);
    void stop(AudioBuffer* buffer);
    pf::Mutex& mutex() { return m_mutex; }

private:
    static int threadEntry(void* self);
    void run();

private:
    int                         m_port;
    pf::Mutex                   m_mutex;
    std::vector<AudioBuffer*>   m_playingBuffers;
    volatile bool               m_running;
    pf::Thread*                 m_thread;
    x86::sreg32                 m_accumulator[s_grain * s_channelCount];
    x86::sreg16                 m_output[s_grain * s_channelCount];
};

class AudioBuffer : public GenericResource
{
    friend class AudioDevice;
public:
    AudioBuffer(AudioDevice* device, WinApplication* app, x86::reg32 bufferSize);
    ~AudioBuffer();

    x86::reg32 bufferSize() const;
    void play();
    void stop();
    x86::reg32 lock();
    void unlock(x86::reg32 bufferWritten);
    x86::reg32 playStart() const        { return m_playStart; }
    x86::reg32 playStop() const         { return m_playStop; }
    x86::reg32 getBackBuffer() const    { return m_bufferMap->getBlockStart(); }
    bool primary() const                { return m_primary; }

private:
    // Adds up to `frames` stereo frames of pending data into the accumulator.
    void mixInto(x86::sreg32* accumulator, x86::reg32 frames);

private:
    AudioDevice*    m_device;
    x86::reg32      m_bufferSize;
    MemMap*         m_memmap;
    MemMap*         m_bufferMap;
    x86::sreg16*    m_buffer;
    x86::reg32      m_playStart;
    x86::reg32      m_playStop;
    bool            m_primary;
};

}

#endif /*!LIB_AUDIO_H_*/
