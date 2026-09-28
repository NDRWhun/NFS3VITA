#include <winapi/dsound/idirectsoundbuffer.h>
#include <winapi/dsound/idirectsound3dlistener.h>
#include <winapi/dsound/idirectsound.h>
#include <winapi/wrapper.h>
#include <lib/audio.h>
#include <lib/thread.h>
#include <algorithm>


namespace win32 { namespace dsound
{

x86::reg32 IDirectSoundBuffer::s_iDirectSoundBufferVtable = 0;

struct WAVEFORMATEX
{
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
};

// Guest size of WAVEFORMATEX (byte packed, no tail padding).
static const DWORD s_waveFormatSize = 18;

struct IDirectSoundBuffer::DSBCAPS
{
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwUnlockTransferRate;
    DWORD dwPlayCpuOverhead;
};

enum DirectSoundBufferStatus
{
    DSBSTATUS_PLAYING         = 0x00000001,
    DSBSTATUS_BUFFERLOST      = 0x00000002,
    DSBSTATUS_LOOPING         = 0x00000004
};

enum DirectSoundConstants
{
    WAVE_FORMAT_PCM           = 1,
    DSBPLAY_LOOPING           = 0x00000001,
    DSBLOCK_FROMWRITECURSOR   = 0x00000001,
    DSBLOCK_ENTIREBUFFER      = 0x00000002,
    DSBSIZE_MIN               = 4,
    DSBSIZE_MAX               = 0x0FFFFFFF,
    DSBFREQUENCY_ORIGINAL     = 0,
    DSBFREQUENCY_MIN          = 100,
    DSBFREQUENCY_MAX          = 100000,
    DSBVOLUME_MIN             = -10000,
    DSBVOLUME_MAX             = 0,
    DSBPAN_LEFT               = -10000,
    DSBPAN_RIGHT              = 10000
};

DEFINE_GUID(IID_IDirectSoundBuffer,	0x279AFA85,0x4981,0x11CE,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60);
DEFINE_GUID(IID_IDirectSound3DListener, 0x279AFA84,0x4981,0x11CE,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60);

// Accepts PCM with 1-2 channels of 8 or 16 bits.
static HRESULT readWaveFormat(LPCWAVEFORMATEX lpwfxFormat, AudioFormat* format)
{
    const x86::reg32 channels = lpwfxFormat->nChannels;
    const x86::reg32 bits = lpwfxFormat->wBitsPerSample;
    const x86::reg32 rate = lpwfxFormat->nSamplesPerSec;
    if (lpwfxFormat->wFormatTag != WAVE_FORMAT_PCM
        || (channels != 1 && channels != 2)
        || (bits != 8 && bits != 16)
        || rate < DSBFREQUENCY_MIN || rate > DSBFREQUENCY_MAX
        || lpwfxFormat->nBlockAlign != channels * bits / 8)
        return DSERR_BADFORMAT;
    format->channels = channels;
    format->bitsPerSample = bits;
    format->sampleRate = rate;
    return DS_OK;
}

HRESULT IDirectSoundBuffer::readDescription(WinApplication* app, LPCDSBUFFERDESC lpcDSBufferDesc, AudioFormat* format, DWORD* bufferBytes)
{
    if (!lpcDSBufferDesc || lpcDSBufferDesc->dwSize != sizeof(DSBUFFERDESC))
        return DSERR_INVALIDPARAM;
    if (lpcDSBufferDesc->dwFlags & DSBCAPS_PRIMARYBUFFER)
    {
        *format = AudioDevice::s_outputFormat;
        *bufferBytes = AudioBuffer::s_primaryBufferBytes;
        return DS_OK;
    }
    if (!lpcDSBufferDesc->lpwfxFormat)
        return DSERR_INVALIDPARAM;
    const HRESULT result = readWaveFormat(&app->getMemory<WAVEFORMATEX>(lpcDSBufferDesc->lpwfxFormat), format);
    if (result != DS_OK)
        return result;
    const DWORD bytes = lpcDSBufferDesc->dwBufferBytes;
    if (bytes < DSBSIZE_MIN || bytes > DSBSIZE_MAX)
        return DSERR_INVALIDPARAM;
    // Rounded up to whole frames.
    const DWORD frameBytes = format->frameBytes();
    *bufferBytes = (bytes + frameBytes - 1) / frameBytes * frameBytes;
    return DS_OK;
}

IDirectSoundBuffer::IDirectSoundBuffer(WinApplication* app, AudioDevice* device, DWORD bufferBytes, const AudioFormat& format, bool primary)
    :   IUnknown(IID_IDirectSoundBuffer)
{
    m_vtable = s_iDirectSoundBufferVtable;
    m_resource = new AudioBuffer(device, app, bufferBytes, format, primary);
    app->allocateResource(m_resource);
}

AudioBuffer* IDirectSoundBuffer::buffer() const
{
    return static_cast<AudioBuffer*>(m_resource);
}

BOOL IDirectSoundBuffer::QueryInterface(WinApplication* app, x86::CPU& cpu,
                                        REFGUID refiid, x86::reg32* pvvObject)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(refiid);
    if (*refiid == IID_IDirectSoundBuffer)
    {
        *pvvObject = ComGetIndex(app, this);
        m_refCount ++;
        return 0;
    }
    else if (*refiid == IID_IDirectSound3DListener)
    {
        *pvvObject = com::ComAlloc(app);
        new (&app->getMemory<void>(*pvvObject)) IDirectSound3DListener(this);
        return 0;
    }
    else
    {
        *pvvObject = 0;
        //m_refCount ++;
        return 0x80004002;
    }
}

HRESULT IDirectSoundBuffer::GetCaps(WinApplication* app, x86::CPU& cpu, LPDSBCAPS lpDSBufferCaps)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!lpDSBufferCaps || lpDSBufferCaps->dwSize != sizeof(DSBCAPS))
        return DSERR_INVALIDPARAM;
    lpDSBufferCaps->dwFlags = DSBCAPS_GETCURRENTPOSITION2|DSBCAPS_CTRLALL;
    if (buffer()->primary())
        lpDSBufferCaps->dwFlags |= DSBCAPS_LOCHARDWARE;
    else
        lpDSBufferCaps->dwFlags |= DSBCAPS_LOCSOFTWARE;
    lpDSBufferCaps->dwBufferBytes = buffer()->bufferSize();
    lpDSBufferCaps->dwUnlockTransferRate = 0;
    lpDSBufferCaps->dwPlayCpuOverhead = 0;
    return DS_OK;
}

// Both cursors are the next byte the mixer reads; everything before it has been mixed.
HRESULT IDirectSoundBuffer::GetCurrentPosition(WinApplication* app, x86::CPU& cpu, LPDWORD lpdwCurrentPlayCursor, LPDWORD lpdwCurrentWriteCursor)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    const x86::reg32 position = buffer()->position();
    if (lpdwCurrentPlayCursor)
        *lpdwCurrentPlayCursor = position;
    if (lpdwCurrentWriteCursor)
        *lpdwCurrentWriteCursor = position;
    return DS_OK;
}

HRESULT IDirectSoundBuffer::GetFormat(WinApplication* app, x86::CPU& cpu, LPWAVEFORMATEX lpwfxFormat, DWORD dwSizeAllocated, LPDWORD lpdwSizeWritten)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!lpwfxFormat)
    {
        if (!lpdwSizeWritten)
            return DSERR_INVALIDPARAM;
        *lpdwSizeWritten = s_waveFormatSize;
        return DS_OK;
    }
    if (dwSizeAllocated < s_waveFormatSize)
    {
        if (lpdwSizeWritten)
            *lpdwSizeWritten = 0;
        return DSERR_INVALIDPARAM;
    }
    const AudioFormat& format = buffer()->format();
    lpwfxFormat->wFormatTag = WAVE_FORMAT_PCM;
    lpwfxFormat->nChannels = WORD(format.channels);
    lpwfxFormat->nSamplesPerSec = format.sampleRate;
    lpwfxFormat->nAvgBytesPerSec = format.sampleRate * format.frameBytes();
    lpwfxFormat->nBlockAlign = WORD(format.frameBytes());
    lpwfxFormat->wBitsPerSample = WORD(format.bitsPerSample);
    lpwfxFormat->cbSize = 0;
    if (lpdwSizeWritten)
        *lpdwSizeWritten = s_waveFormatSize;
    return DS_OK;
}

HRESULT IDirectSoundBuffer::GetVolume(WinApplication* app, x86::CPU& cpu, LPLONG lplVolume)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!lplVolume)
        return DSERR_INVALIDPARAM;
    *lplVolume = buffer()->volume();
    return DS_OK;
}

HRESULT IDirectSoundBuffer::GetPan(WinApplication* app, x86::CPU& cpu, LPLONG lplpan)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!lplpan)
        return DSERR_INVALIDPARAM;
    *lplpan = buffer()->pan();
    return DS_OK;
}

HRESULT IDirectSoundBuffer::GetFrequency(WinApplication* app, x86::CPU& cpu, LPDWORD lpdwFrequency)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!lpdwFrequency)
        return DSERR_INVALIDPARAM;
    if (buffer()->primary())
        return DSERR_CONTROLUNAVAIL;
    *lpdwFrequency = buffer()->frequency();
    return DS_OK;
}

HRESULT IDirectSoundBuffer::GetStatus(WinApplication* app, x86::CPU& cpu, LPDWORD lpdwStatus)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!lpdwStatus)
        return DSERR_INVALIDPARAM;
    DWORD status = 0;
    if (buffer()->playing())
        status = buffer()->looping() ? DSBSTATUS_PLAYING|DSBSTATUS_LOOPING : DSBSTATUS_PLAYING;
    *lpdwStatus = status;
    return DS_OK;
}

// Buffers are initialised by IDirectSound::CreateSoundBuffer.
HRESULT IDirectSoundBuffer::Initialize(WinApplication* app, x86::CPU& cpu, LPDIRECTSOUND lpDirectSound, LPCDSBUFFERDESC lpcDSBufferDesc)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(lpDirectSound);
    NFS2_USE(lpcDSBufferDesc);
    return DSERR_ALREADYINITIALIZED;
}

HRESULT IDirectSoundBuffer::Lock(WinApplication* app, x86::CPU& cpu, DWORD dwOffset, DWORD dwBytes,
                                 Packed<void>* ppvAudioPtr1, LPDWORD pdwAudioBytes1, Packed<void>* ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD dwFlags)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    AudioBuffer* audio = buffer();
    const x86::reg32 bufferSize = audio->bufferSize();
    if (dwFlags & DSBLOCK_FROMWRITECURSOR)
        dwOffset = audio->position();
    if (dwFlags & DSBLOCK_ENTIREBUFFER)
        dwBytes = bufferSize;
    if (!ppvAudioPtr1 || !pdwAudioBytes1 || dwOffset >= bufferSize || dwBytes > bufferSize)
        return DSERR_INVALIDPARAM;
    const x86::reg32 memory = audio->lock();
    const x86::reg32 firstBytes = std::min(dwBytes, bufferSize - dwOffset);
    *ppvAudioPtr1 = Packed<void>(memory + dwOffset);
    *pdwAudioBytes1 = firstBytes;
    if (ppvAudioPtr2)
        *ppvAudioPtr2 = Packed<void>(dwBytes > firstBytes ? memory : 0);
    if (pdwAudioBytes2)
        *pdwAudioBytes2 = ppvAudioPtr2 ? dwBytes - firstBytes : 0;
    return DS_OK;
}

HRESULT IDirectSoundBuffer::Play(WinApplication* app, x86::CPU& cpu, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(dwReserved1);
    NFS2_USE(dwReserved2);
    buffer()->play((dwFlags & DSBPLAY_LOOPING) != 0);
    return DS_OK;
}

HRESULT IDirectSoundBuffer::SetCurrentPosition(WinApplication* app, x86::CPU& cpu, DWORD dwNewPosition)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (buffer()->primary())
        return DSERR_INVALIDCALL;
    if (dwNewPosition >= buffer()->bufferSize())
        return DSERR_INVALIDPARAM;
    buffer()->setPosition(dwNewPosition);
    return DS_OK;
}

HRESULT IDirectSoundBuffer::SetFormat(WinApplication* app, x86::CPU& cpu, LPCWAVEFORMATEX lpcfxFormat)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (!buffer()->primary())
        return DSERR_INVALIDCALL;
    if (!lpcfxFormat)
        return DSERR_INVALIDPARAM;
    AudioFormat format;
    const HRESULT result = readWaveFormat(lpcfxFormat, &format);
    if (result != DS_OK)
        return result;
    buffer()->setFormat(format);
    return DS_OK;
}

HRESULT IDirectSoundBuffer::SetVolume(WinApplication* app, x86::CPU& cpu, LONG lVolume)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (lVolume < DSBVOLUME_MIN || lVolume > DSBVOLUME_MAX)
        return DSERR_INVALIDPARAM;
    buffer()->setVolume(lVolume);
    return DS_OK;
}

HRESULT IDirectSoundBuffer::SetPan(WinApplication* app, x86::CPU& cpu, LONG lPan)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (lPan < DSBPAN_LEFT || lPan > DSBPAN_RIGHT)
        return DSERR_INVALIDPARAM;
    buffer()->setPan(lPan);
    return DS_OK;
}

HRESULT IDirectSoundBuffer::SetFrequency(WinApplication* app, x86::CPU& cpu, DWORD dwFrequency)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    if (buffer()->primary())
        return DSERR_CONTROLUNAVAIL;
    if (dwFrequency != DSBFREQUENCY_ORIGINAL && (dwFrequency < DSBFREQUENCY_MIN || dwFrequency > DSBFREQUENCY_MAX))
        return DSERR_INVALIDPARAM;
    buffer()->setFrequency(dwFrequency);
    return DS_OK;
}

HRESULT IDirectSoundBuffer::Stop(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    buffer()->stop();
    return DS_OK;
}

HRESULT IDirectSoundBuffer::Unlock(WinApplication* app, x86::CPU& cpu, Packed<void> pvAudioPtr1, DWORD dwAudioBytes1, Packed<void> pvAudioPtr2, DWORD dwAudioBytes2)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(pvAudioPtr1);
    NFS2_USE(dwAudioBytes1);
    NFS2_USE(pvAudioPtr2);
    NFS2_USE(dwAudioBytes2);
    buffer()->unlock();
    return DS_OK;
}

// Buffer memory is never lost.
HRESULT IDirectSoundBuffer::Restore(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return DS_OK;
}

x86::reg32 IDirectSoundBuffer::init(WinApplication *app, x86::reg32 memblockIndex, x86::reg8* memBlock)
{
    x86::reg32* vtable = reinterpret_cast<x86::reg32*>(memBlock);
    app->registerMethod(0x400068, {"IDirectSoundBuffer::QueryInterface", &Wrapper<decltype(&IDirectSoundBuffer::QueryInterface), &IDirectSoundBuffer::QueryInterface>::stdcall});
    app->registerMethod(0x400069, {"IDirectSoundBuffer::GetCaps", &Wrapper<decltype(&IDirectSoundBuffer::GetCaps), &IDirectSoundBuffer::GetCaps>::stdcall});
    app->registerMethod(0x40006a, {"IDirectSoundBuffer::GetCurrentPosition", &Wrapper<decltype(&IDirectSoundBuffer::GetCurrentPosition), &IDirectSoundBuffer::GetCurrentPosition>::stdcall});
    app->registerMethod(0x40006b, {"IDirectSoundBuffer::GetFormat", &Wrapper<decltype(&IDirectSoundBuffer::GetFormat), &IDirectSoundBuffer::GetFormat>::stdcall});
    app->registerMethod(0x40006c, {"IDirectSoundBuffer::GetVolume", &Wrapper<decltype(&IDirectSoundBuffer::GetVolume), &IDirectSoundBuffer::GetVolume>::stdcall});
    app->registerMethod(0x40006d, {"IDirectSoundBuffer::GetPan", &Wrapper<decltype(&IDirectSoundBuffer::GetPan), &IDirectSoundBuffer::GetPan>::stdcall});
    app->registerMethod(0x40006e, {"IDirectSoundBuffer::GetFrequency", &Wrapper<decltype(&IDirectSoundBuffer::GetFrequency), &IDirectSoundBuffer::GetFrequency>::stdcall});
    app->registerMethod(0x40006f, {"IDirectSoundBuffer::GetStatus", &Wrapper<decltype(&IDirectSoundBuffer::GetStatus), &IDirectSoundBuffer::GetStatus>::stdcall});
    app->registerMethod(0x400070, {"IDirectSoundBuffer::Initialize", &Wrapper<decltype(&IDirectSoundBuffer::Initialize), &IDirectSoundBuffer::Initialize>::stdcall});
    app->registerMethod(0x400071, {"IDirectSoundBuffer::Lock", &Wrapper<decltype(&IDirectSoundBuffer::Lock), &IDirectSoundBuffer::Lock>::stdcall});
    app->registerMethod(0x400072, {"IDirectSoundBuffer::Play", &Wrapper<decltype(&IDirectSoundBuffer::Play), &IDirectSoundBuffer::Play>::stdcall});
    app->registerMethod(0x400073, {"IDirectSoundBuffer::SetCurrentPosition", &Wrapper<decltype(&IDirectSoundBuffer::SetCurrentPosition), &IDirectSoundBuffer::SetCurrentPosition>::stdcall});
    app->registerMethod(0x400074, {"IDirectSoundBuffer::SetFormat", &Wrapper<decltype(&IDirectSoundBuffer::SetFormat), &IDirectSoundBuffer::SetFormat>::stdcall});
    app->registerMethod(0x400075, {"IDirectSoundBuffer::SetVolume", &Wrapper<decltype(&IDirectSoundBuffer::SetVolume), &IDirectSoundBuffer::SetVolume>::stdcall});
    app->registerMethod(0x400076, {"IDirectSoundBuffer::SetPan", &Wrapper<decltype(&IDirectSoundBuffer::SetPan), &IDirectSoundBuffer::SetPan>::stdcall});
    app->registerMethod(0x400077, {"IDirectSoundBuffer::SetFrequency", &Wrapper<decltype(&IDirectSoundBuffer::SetFrequency), &IDirectSoundBuffer::SetFrequency>::stdcall});
    app->registerMethod(0x400078, {"IDirectSoundBuffer::Stop", &Wrapper<decltype(&IDirectSoundBuffer::Stop), &IDirectSoundBuffer::Stop>::stdcall});
    app->registerMethod(0x400079, {"IDirectSoundBuffer::Unlock", &Wrapper<decltype(&IDirectSoundBuffer::Unlock), &IDirectSoundBuffer::Unlock>::stdcall});
    app->registerMethod(0x40007a, {"IDirectSoundBuffer::Restore", &Wrapper<decltype(&IDirectSoundBuffer::Restore), &IDirectSoundBuffer::Restore>::stdcall});

    s_iDirectSoundBufferVtable = memblockIndex;
    vtable[0] = 0x400068;    // IDirectSoundBuffer_QueryInterface
    vtable[1] = 0x400001;    // IUnknown_AddRef
    vtable[2] = 0x400002;    // IUnknown_Release
    vtable[3] = 0x400069;    // IDirectSoundBuffer_GetCaps
    vtable[4] = 0x40006a;    // IDirectSoundBuffer_GetCurrentPosition
    vtable[5] = 0x40006b;    // IDirectSoundBuffer_GetFormat
    vtable[6] = 0x40006c;    // IDirectSoundBuffer_GetVolume
    vtable[7] = 0x40006d;    // IDirectSoundBuffer_GetPan
    vtable[8] = 0x40006e;    // IDirectSoundBuffer_GetFrequency
    vtable[9] = 0x40006f;    // IDirectSoundBuffer_GetStatus
    vtable[10] = 0x400070;   // IDirectSoundBuffer_Initialize
    vtable[11] = 0x400071;   // IDirectSoundBuffer_Lock
    vtable[12] = 0x400072;   // IDirectSoundBuffer_Play
    vtable[13] = 0x400073;   // IDirectSoundBuffer_SetCurrentPosition
    vtable[14] = 0x400074;   // IDirectSoundBuffer_SetFormat
    vtable[15] = 0x400075;   // IDirectSoundBuffer_SetVolume
    vtable[16] = 0x400076;   // IDirectSoundBuffer_SetPan
    vtable[17] = 0x400077;   // IDirectSoundBuffer_SetFrequency
    vtable[18] = 0x400078;   // IDirectSoundBuffer_Stop
    vtable[19] = 0x400079;   // IDirectSoundBuffer_Unlock
    vtable[20] = 0x40007a;   // IDirectSoundBuffer_Restore
    return 21 * sizeof(x86::reg32);
}

}}
