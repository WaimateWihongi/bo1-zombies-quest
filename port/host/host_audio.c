/* Audio output (milestone 7): the guest's mixer (snd_driver_quest.cpp) writes 48 kHz stereo float
 * frames into a ring here; an AAudio stream's callback plays them, and silence when the ring runs dry.
 *
 * One writer (the guest's mixer thread) and one reader (AAudio's callback thread): the ring's indices
 * are atomics, nothing is locked. If the device goes away (a headset audio route change), the error
 * callback can't reopen the stream itself, so it flags a reopen for the next write. */
#include "host.h"

#include <aaudio/AAudio.h>
#include <stdatomic.h>
#include <string.h>

#define RING_FRAMES 16384 /* a power of two: ~340 ms at 48 kHz */
#define CHANNELS 2

static float g_ring[RING_FRAMES * CHANNELS];
static atomic_uint g_write, g_read; /* frame counters; their difference is what's queued */
static AAudioStream *g_stream;
static uint32_t g_rate;
static atomic_int g_reopen;

static aaudio_data_callback_result_t play(AAudioStream *stream, void *user, void *audio, int32_t frames)
{
    float *out = (float *)audio;
    unsigned read = atomic_load_explicit(&g_read, memory_order_relaxed);
    unsigned avail = atomic_load_explicit(&g_write, memory_order_acquire) - read;
    unsigned n = (unsigned)frames < avail ? (unsigned)frames : avail;
    for (unsigned i = 0; i < n; i++) {
        unsigned at = (read + i) & (RING_FRAMES - 1);
        out[i * 2] = g_ring[at * 2];
        out[i * 2 + 1] = g_ring[at * 2 + 1];
    }
    if (n < (unsigned)frames) memset(out + n * 2, 0, (size_t)(frames - (int32_t)n) * 2 * sizeof(float));
    atomic_store_explicit(&g_read, read + n, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void failed(AAudioStream *stream, void *user, aaudio_result_t error)
{
    host_warn("audio: stream error %d (%s); reopening", error, AAudio_convertResultToText(error));
    atomic_store(&g_reopen, 1);
}

static int open_stream(uint32_t rate)
{
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return -1;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, CHANNELS);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, play, NULL);
    AAudioStreamBuilder_setErrorCallback(b, failed, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        host_error("audio: can't open an output stream: %s", AAudio_convertResultToText(r));
        g_stream = NULL;
        return -1;
    }
    r = AAudioStream_requestStart(g_stream);
    if (r != AAUDIO_OK) {
        host_error("audio: can't start the output stream: %s", AAudio_convertResultToText(r));
        AAudioStream_close(g_stream);
        g_stream = NULL;
        return -1;
    }
    host_info("audio: output %d Hz, %d channels, burst %d frames", AAudioStream_getSampleRate(g_stream),
              AAudioStream_getChannelCount(g_stream), AAudioStream_getFramesPerBurst(g_stream));
    return 0;
}

/* the guest: opens the output at this rate; 0 on success */
int32_t host_audio_open(uint32_t rate)
{
    if (g_stream) return 0;
    g_rate = rate;
    return open_stream(rate);
}

/* the guest: frames (interleaved stereo floats at guest_frames) into the ring; how many fitted */
uint32_t host_audio_write(uint32_t guest_frames, uint32_t frames)
{
    if (atomic_exchange(&g_reopen, 0) && g_rate) {
        if (g_stream) {
            AAudioStream_close(g_stream);
            g_stream = NULL;
        }
        open_stream(g_rate);
    }
    const float *in = GUEST_PTR(const float *, guest_frames);
    unsigned write = atomic_load_explicit(&g_write, memory_order_relaxed);
    unsigned space = RING_FRAMES - (write - atomic_load_explicit(&g_read, memory_order_acquire));
    unsigned n = frames < space ? frames : space;
    for (unsigned i = 0; i < n; i++) {
        unsigned at = (write + i) & (RING_FRAMES - 1);
        g_ring[at * 2] = in[i * 2];
        g_ring[at * 2 + 1] = in[i * 2 + 1];
    }
    atomic_store_explicit(&g_write, write + n, memory_order_release);
    return n;
}

/* the guest: frames written but not yet played */
uint32_t host_audio_queued(void)
{
    return atomic_load_explicit(&g_write, memory_order_acquire) - atomic_load_explicit(&g_read, memory_order_acquire);
}
