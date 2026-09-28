#ifndef DSOUND_IDIRECTSOUNDBUFFER_H_
#define DSOUND_IDIRECTSOUNDBUFFER_H_

#include "winapi/com.h"

namespace win32
{

class AudioBuffer;
class AudioDevice;
struct AudioFormat;

namespace dsound
{

enum DirectSoundBufferFlags
{
    DSBCAPS_PRIMARYBUFFER         = 0x00000001,
    DSBCAPS_STATIC                = 0x00000002,
    DSBCAPS_LOCHARDWARE           = 0x00000004,
    DSBCAPS_LOCSOFTWARE           = 0x00000008,
    DSBCAPS_CTRL3D                = 0x00000010,
    DSBCAPS_CTRLFREQUENCY         = 0x00000020,
    DSBCAPS_CTRLPAN               = 0x00000040,
    DSBCAPS_CTRLVOLUME            = 0x00000080,
    DSBCAPS_CTRLDEFAULT           = 0x000000E0,  /* Pan + volume + frequency. */
    DSBCAPS_CTRLPOSITIONNOTIFY    = 0x00000100,
    DSBCAPS_CTRLFX                = 0x00000200,
    DSBCAPS_CTRLALL               = 0x000001F0,  /* All control capabilities */
    DSBCAPS_STICKYFOCUS           = 0x00004000,
    DSBCAPS_GLOBALFOCUS           = 0x00008000,
    DSBCAPS_GETCURRENTPOSITION2   = 0x00010000,  /* More accurate play cursor under emulation*/
    DSBCAPS_MUTE3DATMAXDISTANCE   = 0x00020000,
    DSBCAPS_LOCDEFER              = 0x00040000
};

class IDirectSound;
struct DSBUFFERDESC;
struct DSBUFFERDESC1;
struct WAVEFORMATEX;
typedef IDirectSound* LPDIRECTSOUND;
typedef const DSBUFFERDESC* LPCDSBUFFERDESC;
typedef const WAVEFORMATEX* LPCWAVEFORMATEX;

class IDirectSoundBuffer : public com::IUnknown
{
public:
    IDirectSoundBuffer(WinApplication* app, AudioDevice* device, DWORD bufferBytes, const AudioFormat& format, bool primary);

    // Validates a CreateSoundBuffer description and returns the sample format and size of the buffer.
    static HRESULT readDescription(WinApplication* app, LPCDSBUFFERDESC lpcDSBufferDesc, AudioFormat* format, DWORD* bufferBytes);
    static x86::reg32 init(WinApplication* app, x86::reg32 memblockIndex, x86::reg8* memBlock);

private:
    struct DSBCAPS;
    struct DSBCAPS1;
    typedef DSBCAPS* LPDSBCAPS;
    typedef WAVEFORMATEX* LPWAVEFORMATEX;

    AudioBuffer* buffer() const;

    BOOL QueryInterface(WinApplication* app, x86::CPU& cpu,
                        REFGUID refiid, x86::reg32* pvvObject);
    HRESULT GetCaps(WinApplication* app, x86::CPU& cpu, LPDSBCAPS lpDSBufferCaps);
    HRESULT GetCurrentPosition(WinApplication* app, x86::CPU& cpu, LPDWORD lpdwCurrentPlayCursor, LPDWORD lpdwCurrentWriteCursor);
    HRESULT GetFormat(WinApplication* app, x86::CPU& cpu, LPWAVEFORMATEX lpwfxFormat, DWORD dwSizeAllocated, LPDWORD lpdwSizeWritten);
    HRESULT GetVolume(WinApplication* app, x86::CPU& cpu, LPLONG lplVolume);
    HRESULT GetPan(WinApplication* app, x86::CPU& cpu, LPLONG lplpan);
    HRESULT GetFrequency(WinApplication* app, x86::CPU& cpu, LPDWORD lpdwFrequency);
    HRESULT GetStatus(WinApplication* app, x86::CPU& cpu, LPDWORD lpdwStatus);
    HRESULT Initialize(WinApplication* app, x86::CPU& cpu, LPDIRECTSOUND lpDirectSound, LPCDSBUFFERDESC lpcDSBufferDesc);
    HRESULT Lock(WinApplication* app, x86::CPU& cpu, DWORD dwOffset, DWORD dwBytes,
                 Packed<void>* ppvAudioPtr1, LPDWORD pdwAudioBytes1, Packed<void>* ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD dwFlags);
    HRESULT Play(WinApplication* app, x86::CPU& cpu, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags);
    HRESULT SetCurrentPosition(WinApplication* app, x86::CPU& cpu, DWORD dwNewPosition);
    HRESULT SetFormat(WinApplication* app, x86::CPU& cpu, LPCWAVEFORMATEX lpcfxFormat);
    HRESULT SetVolume(WinApplication* app, x86::CPU& cpu, LONG lVolume);
    HRESULT SetPan(WinApplication* app, x86::CPU& cpu, LONG lPan);
    HRESULT SetFrequency(WinApplication* app, x86::CPU& cpu, DWORD dwFrequency);
    HRESULT Stop(WinApplication* app, x86::CPU& cpu);
    HRESULT Unlock(WinApplication* app, x86::CPU& cpu, Packed<void> pvAudioPtr1, DWORD dwAudioBytes1, Packed<void> pvAudioPtr2, DWORD dwAudioBytes2);
    HRESULT Restore(WinApplication* app, x86::CPU& cpu);

private:
    static x86::reg32 s_iDirectSoundBufferVtable;
};

}}

#endif /*!DSOUND_IDIRECTSOUNDBUFFER_H_*/
