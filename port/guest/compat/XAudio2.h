/* Just enough of the XAudio2 names for the engine's sound headers to compile. The XAudio2 driver
   itself (snd_driver_xaudio2.cpp) is not built for the guest: milestone 3 replaces it with an
   Android audio driver, so these types only describe runtime state, never data on disk. */
#pragma once
#include <windows.h>

#define STDMETHOD_(type, name) virtual type name
#define THIS_
#define THIS void

typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX;
typedef struct adpcmcoef_tag {
    short iCoef1, iCoef2;
} ADPCMCOEFSET;

#define XAUDIO2_END_OF_STREAM 0x0040
typedef UINT32 XAUDIO2_WINDOWS_PROCESSOR_SPECIFIER;

typedef struct XAUDIO2_BUFFER {
    UINT32 Flags, AudioBytes;
    const BYTE *pAudioData;
    UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount;
    void *pContext;
} XAUDIO2_BUFFER;
typedef struct XAUDIO2_VOICE_DETAILS {
    UINT32 CreationFlags, ActiveFlags, InputChannels, InputSampleRate;
} XAUDIO2_VOICE_DETAILS;
typedef struct XAUDIO2_DEVICE_DETAILS {
    WAVEFORMATEX OutputFormat;
} XAUDIO2_DEVICE_DETAILS;
typedef struct XAUDIO2_PERFORMANCE_DATA {
    UINT32 ActiveSourceVoiceCount, TotalSourceVoiceCount, GlitchesSinceEngineStarted;
} XAUDIO2_PERFORMANCE_DATA;

#ifdef __cplusplus
struct IXAudio2;
struct IXAudio2Voice;
struct IXAudio2SourceVoice;
struct IXAudio2SubmixVoice;
struct IXAudio2MasteringVoice;

/* StreamVoice overrides these */
struct IXAudio2VoiceCallback {
    virtual void OnVoiceProcessingPassStart(UINT32 bytes_required) = 0;
    virtual void OnVoiceProcessingPassEnd() = 0;
    virtual void OnStreamEnd() = 0;
    virtual void OnBufferStart(void *context) = 0;
    virtual void OnBufferEnd(void *context) = 0;
    virtual void OnLoopEnd(void *context) = 0;
    virtual void OnVoiceError(void *context, HRESULT error) = 0;
};
#endif
