/* The sound driver on the Quest (milestone 7): the engine's sound system (sound/snd.cpp) drives a
 * driver through a few calls - SD_Init, SD_StartAlias, SD_UpdateVoice, SD_StopVoice, SD_PauseVoice /
 * SD_UnpauseVoice, SD_PreUpdate, SD_BO1SamplesPlayed, SD_Shutdown. On Windows that driver is XAudio2
 * (sound/snd_driver_xaudio2.cpp, left out of this build); this one does what it does with a software
 * mixer of its own, its logic kept as the original's where the engine can tell.
 *
 * Each engine voice is a mixer voice: 16-bit PCM or MS-ADPCM (the two formats the PC game's sounds come
 * in), one buffer for a sound in memory, a queue of two for a stream (the stream's windows, handed back
 * to the stream as each finishes playing, as the original's OnBufferEnd does). The engine's panning
 * (the speaker map, for the "Stereo" configuration) gives each voice's gains to the left and right
 * outputs; its pitch, the playback rate. A mixer thread keeps about 50 ms queued in the host's audio
 * output (host_audio.c, AAudio, 48 kHz stereo float).
 *
 * Not yet: the game's effects (the reverb bus, the futz filter, the master EQ and limiter) and xWMA. */
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "win32_internal.h"
#include "sound/snd_driver_xaudio2.h"
#include "sound/snd_stream.h"
#include "sound/snd_dvar.h"
#include "sound/snd_utils.h"
#include "sound/snd_globals.h"
#include "sound/snd_db.h"
#include "sound/snd_public_async.h"
#include "sound/snd_bank.h"
#include "qcommon/common.h"

extern "C" int host_audio_open(unsigned int rate);
extern "C" unsigned int host_audio_write(const float *frames, unsigned int count);
extern "C" unsigned int host_audio_queued(void);
extern "C" unsigned int host_wma_decode(const void *in, unsigned int in_bytes, unsigned int channels, unsigned int rate,
                                        unsigned int block_align, unsigned int avg_bytes_per_sec, void *out,
                                        unsigned int out_bytes);
#include <unordered_map>

namespace {

const unsigned OUTPUT_RATE = 48000;
const unsigned MIX_FRAMES = 240;       /* 5 ms */
const unsigned QUEUE_TARGET = 2400;    /* 50 ms */
const unsigned ADPCM_BLOCK_FRAMES = 512;

struct MixBuffer {
    const uint8_t *data;
    unsigned bytes;
    unsigned context; /* streams: bufferIndex | streamVoice << 8 */
};

struct MixVoice {
    bool used, playing, stream, looping;
    bool adpcm;
    unsigned channels, rate, block_align;
    float pitch;
    float gain[2][2];   /* [output][input channel] */
    MixBuffer queue[3];
    unsigned queued;
    double pos;          /* frame position in queue[0] */
    unsigned long long played;
    /* the ADPCM block of queue[0] decoded last */
    int16_t block[ADPCM_BLOCK_FRAMES * 2];
    int block_index;
};

MixVoice g_voices[SND_MAX_VOICES];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
bool g_running;

/* a stream's two windows, as the original's StreamVoice */
struct StreamState {
    const uint8_t *data[2];
    unsigned queued_count;
} g_streams[SND_MAX_STREAM_VOICES];

/* ---- decoding ---- */

const int ADAPT[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
const int COEF1[7] = {256, 512, 0, 192, 240, 460, 392};
const int COEF2[7] = {0, -256, 0, 64, 0, -208, -232};

int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* MS-ADPCM: one block (block_align bytes, channels interleaved) into frames of interleaved samples;
   how many frames it held */
unsigned decode_adpcm_block(const uint8_t *in, unsigned bytes, unsigned channels, int16_t *out)
{
    if (bytes < 7 * channels) return 0;
    int pred[2], delta[2], s1[2], s2[2];
    const uint8_t *p = in;
    for (unsigned c = 0; c < channels; c++) pred[c] = *p++ % 7;
    for (unsigned c = 0; c < channels; c++, p += 2) delta[c] = (int16_t)(p[0] | p[1] << 8);
    for (unsigned c = 0; c < channels; c++, p += 2) s1[c] = (int16_t)(p[0] | p[1] << 8);
    for (unsigned c = 0; c < channels; c++, p += 2) s2[c] = (int16_t)(p[0] | p[1] << 8);
    unsigned frames = 0;
    for (unsigned c = 0; c < channels; c++) out[c] = (int16_t)s2[c];
    for (unsigned c = 0; c < channels; c++) out[channels + c] = (int16_t)s1[c];
    frames = 2;
    unsigned nibbles = (unsigned)(in + bytes - p) * 2, n = 0;
    while (n < nibbles && frames < ADPCM_BLOCK_FRAMES) {
        for (unsigned c = 0; c < channels && n < nibbles; c++, n++) {
            int nib = (n & 1) ? (p[n >> 1] & 15) : (p[n >> 1] >> 4);
            int signed_nib = nib >= 8 ? nib - 16 : nib;
            int predicted = (s1[c] * COEF1[pred[c]] + s2[c] * COEF2[pred[c]]) >> 8;
            int sample = clamp16(predicted + signed_nib * delta[c]);
            s2[c] = s1[c];
            s1[c] = sample;
            delta[c] = (delta[c] * ADAPT[nib]) >> 8;
            if (delta[c] < 16) delta[c] = 16;
            out[frames * channels + c] = (int16_t)sample;
        }
        frames++;
    }
    return frames;
}

unsigned buffer_frames(const MixVoice &v, const MixBuffer &b)
{
    if (!v.adpcm) return b.bytes / (2 * v.channels);
    unsigned whole = b.bytes / v.block_align, rest = b.bytes % v.block_align;
    unsigned frames = whole * ADPCM_BLOCK_FRAMES;
    if (rest >= 7 * v.channels) frames += (rest - 7 * v.channels) * 2 / v.channels + 2;
    return frames;
}

/* one input channel's sample at a frame of queue[0] */
float sample_at(MixVoice &v, unsigned frame, unsigned channel)
{
    const MixBuffer &b = v.queue[0];
    if (!v.adpcm) return (int16_t)(b.data[(frame * v.channels + channel) * 2] | b.data[(frame * v.channels + channel) * 2 + 1] << 8) *
                         (1.0f / 32768.0f);
    int index = (int)(frame / ADPCM_BLOCK_FRAMES);
    if (index != v.block_index) {
        unsigned at = (unsigned)index * v.block_align, len = b.bytes - at < v.block_align ? b.bytes - at : v.block_align;
        decode_adpcm_block(b.data + at, len, v.channels, v.block);
        v.block_index = index;
    }
    return v.block[(frame % ADPCM_BLOCK_FRAMES) * v.channels + channel] * (1.0f / 32768.0f);
}

/* the front buffer is done: a stream's window goes back to the stream (OnBufferEnd) */
void pop_buffer(MixVoice &v)
{
    if (v.stream) {
        unsigned ctx = v.queue[0].context;
        iSND_ReleaseStreamBuffer(ctx >> 8, ctx & 255);
    }
    for (unsigned i = 1; i < v.queued; i++) v.queue[i - 1] = v.queue[i];
    v.queued--;
    v.pos = 0.0;
    v.block_index = -1;
}

/* mixes every playing voice into out (frames of stereo) */
void mix(float *out, unsigned frames)
{
    memset(out, 0, sizeof(float) * 2 * frames);
    for (MixVoice &v : g_voices) {
        if (!v.used || !v.playing || !v.queued) continue;
        double step = (double)v.rate * (double)v.pitch / OUTPUT_RATE;
        if (step <= 0.0) continue;
        for (unsigned f = 0; f < frames && v.queued; f++) {
            unsigned total = buffer_frames(v, v.queue[0]);
            if (!total) {
                pop_buffer(v);
                continue;
            }
            unsigned i0 = (unsigned)v.pos;
            if (i0 >= total) {
                if (v.looping && !v.stream) {
                    v.pos -= total;
                    i0 = (unsigned)v.pos;
                } else {
                    pop_buffer(v);
                    if (!v.queued) break;
                    total = buffer_frames(v, v.queue[0]);
                    i0 = 0;
                }
            }
            unsigned i1 = i0 + 1 < total ? i0 + 1 : (v.looping && !v.stream ? 0 : i0);
            float t = (float)(v.pos - i0);
            for (unsigned c = 0; c < v.channels && c < 2; c++) {
                float a = sample_at(v, i0, c), b = sample_at(v, i1, c);
                float s = a + (b - a) * t;
                out[f * 2] += s * v.gain[0][c];
                out[f * 2 + 1] += s * v.gain[1][c];
            }
            v.pos += step;
            v.played++;
        }
    }
    /* a soft limit, standing in for the game's own limiter (the master bus effect) */
    for (unsigned i = 0; i < frames * 2; i++) {
        /* linear to 0.8, then eased towards 1 */
        float x = out[i], ax = fabsf(x);
        if (ax > 0.8f) out[i] = (x > 0 ? 1.0f : -1.0f) * (0.8f + 0.2f * tanhf((ax - 0.8f) / 0.2f));
    }
}

void *mixer_thread(void *)
{
    static float buffer[MIX_FRAMES * 2];
    while (g_running) {
        while (host_audio_queued() < QUEUE_TARGET) {
            pthread_mutex_lock(&g_lock);
            mix(buffer, MIX_FRAMES);
            pthread_mutex_unlock(&g_lock);
            if (!host_audio_write(buffer, MIX_FRAMES)) break;
        }
        usleep(3000);
    }
    return NULL;
}

/* ---- xWMA: decoded once (host_wma.c, FFmpeg), then played as PCM ---- */

struct Decoded {
    int16_t *pcm;
    unsigned bytes;
};
std::unordered_map<const void *, Decoded> g_wma_cache;

/* the sound as 16-bit PCM: its block size and rate as the original driver declares them to XAudio2 */
const Decoded *wma_pcm(const snd_asset *snd)
{
    auto found = g_wma_cache.find(snd->data);
    if (found != g_wma_cache.end()) return found->second.pcm ? &found->second : NULL;
    unsigned block_align = snd->channel_count != 1 ? 4096 : 2230;
    unsigned in_bytes = snd->seek_table_count ? snd->seek_table_count * block_align : snd->data_size;
    if (in_bytes > snd->data_size) in_bytes = snd->data_size;
    unsigned out_bytes = snd->seek_table && snd->seek_table_count ? snd->seek_table[snd->seek_table_count - 1]
                                                                   : snd->frame_count * 2 * snd->channel_count;
    out_bytes += 8192 * snd->channel_count; /* a frame's slack past the table's last packet */
    Decoded d = {(int16_t *)malloc(out_bytes), 0};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (d.pcm) d.bytes = host_wma_decode(snd->data, in_bytes, snd->channel_count, snd->frame_rate, block_align,
                                         6000 * snd->channel_count, d.pcm, out_bytes);
    if (!d.bytes) {
        free(d.pcm);
        d.pcm = NULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* hitch hunting: each first play's decode, and how long it held up the caller */
    bo1_log("sound: xWMA %u ch %u Hz, %u bytes -> %u bytes of PCM in %.1f ms", snd->channel_count, snd->frame_rate, in_bytes,
            d.bytes, (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    auto inserted = g_wma_cache.emplace(snd->data, d).first;
    return inserted->second.pcm ? &inserted->second : NULL;
}

/* ---- the engine's voices on mixer voices ---- */

/* the original's iSND_CreateVoice: a sound's format, and (in memory) its one buffer */
int create_voice(int voiceIndex, const snd_asset *snd, int isLooping)
{
    bool streaming = SND_IsStream(voiceIndex);
    const Decoded *wma = NULL;
    if (snd->format == SND_ASSET_FORMAT_WMA && !streaming) {
        wma = wma_pcm(snd); /* outside the lock: decoding takes a moment */
        if (!wma) return 0;
    } else if (snd->format != SND_ASSET_FORMAT_PCMS16 && snd->format != SND_ASSET_FORMAT_MSADPCM) {
        static int warned;
        if (warned++ < 20)
            Com_PrintError(9, "sound: format %d of alias %s isn't played on the Quest yet\n", (int)snd->format,
                           g_snd.voice[voiceIndex].alias ? g_snd.voice[voiceIndex].alias->name : "?");
        return 0;
    }
    if (!snd->channel_count || snd->channel_count > 2 || !snd->frame_rate) return 0;
    pthread_mutex_lock(&g_lock);
    MixVoice &v = g_voices[voiceIndex];
    memset(&v, 0, sizeof v);
    v.used = true;
    v.stream = streaming;
    v.looping = isLooping != 0;
    v.adpcm = snd->format == SND_ASSET_FORMAT_MSADPCM;
    v.channels = snd->channel_count;
    v.rate = snd->frame_rate;
    v.block_align = 262 * snd->channel_count;
    v.pitch = 1.0f;
    v.block_index = -1;
    if (wma) {
        v.queue[0] = {(const uint8_t *)wma->pcm, wma->bytes, 0};
        v.queued = 1;
    } else if (!streaming) {
        v.queue[0] = {(const uint8_t *)snd->data, snd->data_size, 0};
        v.queued = 1;
    } else {
        g_streams[iSND_GetStreamChannel(voiceIndex)].queued_count = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return 1;
}

void destroy_voice(int voiceIndex)
{
    pthread_mutex_lock(&g_lock);
    g_voices[voiceIndex].used = false;
    g_voices[voiceIndex].playing = false;
    g_voices[voiceIndex].queued = 0;
    pthread_mutex_unlock(&g_lock);
}

/* the original's SDXA2_UpdateVoiceSends, the dry send: the speaker map's levels to the two outputs */
void update_gains(int voiceIndex)
{
    snd_voice_t *voice = &g_snd.voice[voiceIndex];
    unsigned inputs = voice->pan.input_channel_count, outputs = voice->pan.output_channel_count;
    float g[2][2] = {};
    for (unsigned out = 0; out < 2 && out < outputs; out++)
        for (unsigned in = 0; in < inputs && in < 2; in++) g[out][in] = Snd_SpeakerMapGetVolume(&voice->pan, in, out) * voice->dryLevel;
    if (outputs == 1)
        for (unsigned in = 0; in < 2; in++) g[1][in] = g[0][in];
    pthread_mutex_lock(&g_lock);
    memcpy(g_voices[voiceIndex].gain, g, sizeof g);
    pthread_mutex_unlock(&g_lock);
}

int start_alias_ram(SndStartAliasInfo *startAliasInfo, int voiceIndex)
{
    bool isLooping = (startAliasInfo->alias->flags & 1) != 0;
    snd_asset *snd = &startAliasInfo->alias->soundFile->u.loadSnd->sound;
    if (!create_voice(voiceIndex, snd, isLooping)) return -1;
    unsigned rate = snd->frame_rate;
    unsigned total_msec = isLooping ? 0 : startAliasInfo->startDelay + (unsigned)((double)snd->frame_count * 1000.0 / (double)rate);
    SND_SetSoundFileVoiceInfo(voiceIndex, snd->channel_count, rate, total_msec, 0, SFLS_LOADED);
    SND_SetVoiceStartInfo(voiceIndex, startAliasInfo);
    return startAliasInfo->playbackId;
}

int start_alias_stream(SndStartAliasInfo *startAliasInfo, unsigned int voiceIndex)
{
    const snd_alias_t *alias = startAliasInfo->alias;
    int streamVoice = iSND_GetStreamChannel(voiceIndex);
    char filename[260];
    SND_AliasGetFileName(alias, filename, 256);
    if (!alias->soundFile->exists) {
        Com_PrintError(1, "Tried to play streamed sound '%s' from alias '%s', but it was not found at load time.\n", filename,
                       alias->name);
        return SND_SetPlaybackIdNotPlayed(voiceIndex);
    }
    unsigned primeSize = 0;
    char *primeData = 0;
    if ((alias->flags & 0xC000) >> 14 == 3 && alias->soundFile->u.streamSnd->primeSnd) {
        primeData = alias->soundFile->u.streamSnd->primeSnd->buffer;
        primeSize = alias->soundFile->u.streamSnd->primeSnd->size;
    }
    Snd_StreamOpen(streamVoice, filename, (alias->flags & 1) != 0, primeSize, primeData);
    SND_SetVoiceStartInfo(voiceIndex, startAliasInfo);
    SND_SetSoundFileVoiceInfo(voiceIndex, 0, 0, 0, 0, SFLS_LOADING);
    SD_UpdateVoice(voiceIndex);
    return g_snd.voice[voiceIndex].alias ? startAliasInfo->playbackId : -1;
}

void channel_error(int voiceIndex)
{
    const snd_alias_t *alias = g_snd.voice[voiceIndex].alias;
    SND_LengthNotify(voiceIndex, 0);
    SND_StopVoice(voiceIndex);
    if (alias) alias->soundFile->exists = 0;
}

/* the original's SD_UpdateStreamVoice: the voice made once the stream's header is in, then its windows
   queued as they're read, two at a time */
void update_stream_voice(int voiceIndex)
{
    int streamVoice = iSND_GetStreamChannel(voiceIndex);
    snd_stream_status status = Snd_StreamStatus(streamVoice);
    snd_voice_t *voice = &g_snd.voice[voiceIndex];
    int isLooping = (voice->alias->flags & 1) != 0;
    if (!status || status == SND_STREAM_STARVING || !Snd_StreamGetHeader(streamVoice)) return;
    if (status == SND_STREAM_ERROR) {
        channel_error(voiceIndex);
        return;
    }
    if (!g_voices[voiceIndex].used) {
        if (status != SND_STREAM_OK) return;
        const snd_asset *header = Snd_StreamGetHeader(streamVoice);
        if (!create_voice(voiceIndex, header, isLooping)) {
            channel_error(voiceIndex);
            return;
        }
        unsigned lengthMS = (unsigned)(1000LL * header->frame_count / header->frame_rate);
        SND_SetSoundFileVoiceInfo(voiceIndex, header->channel_count, header->frame_rate, lengthMS, 0, SFLS_LOADED);
        SND_UpdateVoice(&g_snd.voice[voiceIndex], 0.0);
    }
    while (Snd_StreamGetFreeWindows(streamVoice)) {
        pthread_mutex_lock(&g_lock);
        unsigned queued = g_voices[voiceIndex].queued;
        pthread_mutex_unlock(&g_lock);
        if (queued >= 2) break;
        unsigned size = 0, position = 0;
        char *data = 0;
        status = Snd_StreamAcquireWindow(streamVoice, &size, &position, &data);
        switch (status) {
        case SND_STREAM_OK: {
            StreamState &ss = g_streams[streamVoice];
            unsigned bufferIndex = ss.queued_count % 2;
            ss.queued_count++;
            ss.data[bufferIndex] = (const uint8_t *)data;
            pthread_mutex_lock(&g_lock);
            MixVoice &v = g_voices[voiceIndex];
            if (v.queued < 3) v.queue[v.queued++] = {(const uint8_t *)data, size, bufferIndex | (unsigned)streamVoice << 8};
            pthread_mutex_unlock(&g_lock);
            break;
        }
        case SND_STREAM_STARVING:
            return;
        case SND_STREAM_EOF:
            if (!queued) SND_StopVoice(voiceIndex);
            return;
        case SND_STREAM_ERROR:
            channel_error(voiceIndex);
            return;
        default:
            return;
        }
    }
}

} // namespace

/* ---- the driver ---- */

bool __cdecl SD_Xaudio2CanInit() { return true; }

bool SND_IsVoiceFree(int voiceIndex) { return !g_snd.voiceAliasHash[voiceIndex]; }

unsigned int __cdecl iSND_GetStreamChannel(unsigned int index) { return index; }

void __cdecl iSND_ReleaseStreamBuffer(unsigned int streamVoice, unsigned int bufferIndex)
{
    if (streamVoice >= SND_MAX_STREAM_VOICES || bufferIndex >= 2) return;
    const uint8_t *window = (const uint8_t *)__atomic_exchange_n(&g_streams[streamVoice].data[bufferIndex], (const uint8_t *)0,
                                                                  __ATOMIC_ACQ_REL);
    if (window) Snd_StreamReleaseWindow(streamVoice, (char *)window);
}

char __cdecl SD_Init()
{
    if (host_audio_open(OUTPUT_RATE)) {
        Com_PrintError(9, "sound: no audio output\n");
        return 0;
    }
    memset(g_voices, 0, sizeof g_voices);
    memset(g_streams, 0, sizeof g_streams);
    /* stereo: the speaker configuration the engine pans for */
    Snd_SetConfigStringsBySpeakerCount(2);
    for (unsigned index = 0; index < SND_GetSpeakerConfigCount(); ++index)
        if (Snd_GetSpeakerConfig(index)->speakerCount == 2) {
            Dvar_SetInt((dvar_s *)snd_speakerConfiguration, index);
            break;
        }
    if (!g_running) {
        g_running = true;
        pthread_t thread;
        pthread_create(&thread, NULL, mixer_thread, NULL);
    }
    Com_Printf(9, "sound: the Quest's mixer, %u Hz stereo\n", OUTPUT_RATE);
    return 1;
}

void SD_Shutdown()
{
    for (int i = 0; i < SND_MAX_VOICES; i++) SND_StopVoice(i);
}

void SD_PreUpdate() {}

void SD_TruncateAudioDeviceNames(Font_s *, float, int) {}

void __cdecl SD_StopVoice(int voiceIndex)
{
    destroy_voice(voiceIndex);
    if (SND_IsStream(voiceIndex)) {
        int streamVoice = iSND_GetStreamChannel(voiceIndex);
        for (unsigned i = 0; i < 2; ++i) iSND_ReleaseStreamBuffer(streamVoice, i);
        if (Snd_StreamStatus(streamVoice)) Snd_StreamClose(streamVoice);
    }
}

void __cdecl SD_PauseVoice(int voiceIndex)
{
    g_snd.voice[voiceIndex].paused = 1;
    if (g_snd.voice[voiceIndex].soundFileInfo.loadingState != SFLS_LOADING) {
        pthread_mutex_lock(&g_lock);
        g_voices[voiceIndex].playing = false;
        pthread_mutex_unlock(&g_lock);
    }
}

void __cdecl SD_UnpauseVoice(int voiceIndex)
{
    if (g_snd.voice[voiceIndex].soundFileInfo.loadingState != SFLS_LOADING) {
        pthread_mutex_lock(&g_lock);
        if (g_voices[voiceIndex].used) g_voices[voiceIndex].playing = true;
        pthread_mutex_unlock(&g_lock);
    }
}

__int64 SD_BO1SamplesPlayed(int voiceIndex)
{
    if (g_snd.voice[voiceIndex].soundFileInfo.loadingState == SFLS_LOADING || !g_voices[voiceIndex].used) return -1;
    return (__int64)g_voices[voiceIndex].played;
}

/* the original's SD_UpdateVoice: streams fed; levels and pitch; started once it has something to play
   (and isn't held back by a start delay or a pause); stopped once it has nothing left */
void __cdecl SD_UpdateVoice(unsigned int voiceIndex)
{
    if (SND_IsStream(voiceIndex)) {
        update_stream_voice(voiceIndex);
        if (!g_snd.voiceAliasHash[voiceIndex]) return;
    }
    if (g_snd.voice[voiceIndex].soundFileInfo.loadingState == SFLS_LOADING) return;
    snd_voice_t *voice = &g_snd.voice[voiceIndex];
    if (!g_voices[voiceIndex].used) return;
    update_gains(voiceIndex);
    pthread_mutex_lock(&g_lock);
    MixVoice &v = g_voices[voiceIndex];
    if (!voice->startDelay && !v.played && v.queued && !voice->paused) v.playing = true;
    unsigned queued = v.queued;
    if (queued) v.pitch = SND_GetPitch(voice);
    pthread_mutex_unlock(&g_lock);
    if (!queued) SND_StopVoice(voiceIndex);
}

int __cdecl SD_StartAlias(SndStartAliasInfo *startAliasInfo, unsigned int voice)
{
    if (SND_IsStream(voice)) return start_alias_stream(startAliasInfo, voice);
    return start_alias_ram(startAliasInfo, voice);
}
