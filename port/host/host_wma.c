/* xWMA decoding (milestone 7): the PC game keeps most of its short sounds (guns, zombies) as XAudio2's
 * xWMA - WMA version 2 in packets of nBlockAlign bytes. Android has no WMA decoder, so FFmpeg's
 * (libavcodec's wmav2 only, LGPL 2.1, built static: build/third_party/ffmpeg-wma) decodes a whole sound
 * to 16-bit PCM when the guest first plays it (snd_driver_quest.cpp caches the result).
 *
 * xWMA carries no codec extradata, which FFmpeg's WMA decoder wants; as FFmpeg's own xWMA reader
 * (libavformat/xwma.c) does, it gets six bytes with flags 31, and the bit rate corrected for the mono
 * rates whose xWMA headers give a wrong one. */
#include "host.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <string.h>

static int16_t to16(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

/* the guest: in_bytes of xWMA packets (block_align each) to interleaved 16-bit PCM in out (out_bytes
   long); the bytes written, 0 on failure */
uint32_t host_wma_decode(uint32_t guest_in, uint32_t in_bytes, uint32_t channels, uint32_t rate, uint32_t block_align,
                         uint32_t avg_bytes_per_sec, uint32_t guest_out, uint32_t out_bytes)
{
    const uint8_t *in = GUEST_PTR(const uint8_t *, guest_in);
    int16_t *out = GUEST_PTR(int16_t *, guest_out);
    if (!channels || channels > 2 || !block_align || !in_bytes) return 0;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_WMAV2);
    if (!codec) {
        host_error("wma: no decoder");
        return 0;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint32_t written = 0;
    if (!ctx || !packet || !frame) goto done;

    av_channel_layout_default(&ctx->ch_layout, (int)channels);
    ctx->sample_rate = (int)rate;
    ctx->block_align = (int)block_align;
    {
        int br = (int)avg_bytes_per_sec * 8;
        if (channels == 1) {
            if ((rate == 22050 || rate == 32000) && (br == 48000 || br == 192000)) br = 20000;
            else if (rate == 44100 && (br == 96000 || br == 192000)) br = 48000;
        }
        ctx->bit_rate = br;
    }
    ctx->extradata = av_mallocz(6 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx->extradata) goto done;
    ctx->extradata[4] = 31;
    ctx->extradata_size = 6;
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        host_error("wma: can't open the decoder (%u ch, %u Hz, block %u)", channels, rate, block_align);
        goto done;
    }

    for (uint32_t at = 0; at + block_align <= in_bytes; at += block_align) {
        if (av_new_packet(packet, (int)block_align) < 0) break;
        memcpy(packet->data, in + at, block_align);
        int r = avcodec_send_packet(ctx, packet);
        av_packet_unref(packet);
        if (r < 0) break;
        while (avcodec_receive_frame(ctx, frame) == 0) {
            int n = frame->nb_samples, ch = frame->ch_layout.nb_channels;
            for (int i = 0; i < n && written + 2u * (uint32_t)ch <= out_bytes; i++) {
                for (int c = 0; c < ch; c++) {
                    float v = frame->format == AV_SAMPLE_FMT_FLTP ? ((const float *)frame->extended_data[c])[i]
                                                                   : ((const float *)frame->extended_data[0])[i * ch + c];
                    out[written / 2] = to16(v);
                    written += 2;
                }
            }
            av_frame_unref(frame);
        }
    }

done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    return written;
}
