// encode/ffmpeg_encoder.cpp -- FFmpeg ultra-low-latency H.264 encoding (NVENC first, libx264 fallback, supports scaling)
#include "encode/ffmpeg_encoder.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // GetEnvironmentVariableA: SCREENCAST_X264_PRESET override

bool FFmpegEncoder::init(int srcW, int srcH, int dstW, int dstH, int fps, int bitrateKbps, PacketCallback cb) {
    cb_ = std::move(cb);
    srcH_ = srcH;

    // try each available encoder until one opens: h264_nvenc -> libx264 (CPU fallback)
    // NOTE: even if h264_nvenc is found, an old driver version may fail to open it, so try one by one
    const char* kCandidates[] = { "h264_nvenc", "libx264" };
    for (const char* name : kCandidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) continue;

        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) continue;

        // ---- common low-latency params: no B frames, short GOP ----
        ctx_->width  = dstW;
        ctx_->height = dstH;
        ctx_->time_base = { 1, fps };
        ctx_->framerate = { fps, 1 };
        ctx_->pix_fmt   = AV_PIX_FMT_NV12;
        ctx_->gop_size  = fps * 2;                 // one natural keyframe every 2s; loss is covered by client requests
        ctx_->max_b_frames = 0;                    // B frames off = removes the biggest source of encoding latency
        // constrained-quality mode: bit_rate/rc_max_rate/rc_buffer_size act as peak caps (widened buffer lets
        // quality fluctuate: uses the cap on complex scenes, well below it on simple scenes, no longer padding to fill the bitrate)
        ctx_->bit_rate = (int64_t)bitrateKbps * 1000;
        ctx_->rc_max_rate = ctx_->bit_rate;
        ctx_->rc_buffer_size = (int)(ctx_->bit_rate * 2);
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

        // ---- encoder private options ----
        if (std::strcmp(name, "h264_nvenc") == 0) {
            ctx_->thread_count = 1;                                // CPU threads are irrelevant for NVENC
            av_opt_set(ctx_->priv_data, "preset", "p4", 0);        // p4: quality/latency balance
            av_opt_set(ctx_->priv_data, "tune", "ull", 0);         // ultra-low-latency tuning
            av_opt_set(ctx_->priv_data, "rc", "vbr", 0);           // variable bitrate (simple scenes auto-lower bandwidth)
            // cq 18 (was 19, and 21 before that): one QP step is roughly +10% bitrate for visibly
            // crisper text edges. NVENC is a fixed-function unit, so this costs ZERO CPU -- the only
            // price is network bytes, and the VBV cap below (bit_rate/rc_max_rate) still bounds the
            // peaks, which is what actually protects a WiFi classroom.
            av_opt_set(ctx_->priv_data, "cq", "18", 0);
            av_opt_set(ctx_->priv_data, "zerolatency", "1", 0);    // zero-latency mode
        } else {
            // SLICED threads: libx264 splits every frame into slices that are encoded in parallel, so
            // the frame rate scales with the core count while latency stays at a single frame (no
            // frame-level lookahead buffering). The old thread_count = 1 pinned encoding to ONE core,
            // which is half of why a high-resolution source could not hold a high frame rate.
            ctx_->thread_count = 0;                                // 0 = auto (one per logical core)
            av_opt_set(ctx_->priv_data, "sliced-threads", "1", 0);

            // CLARITY: tune=zerolatency used to be set here, and it silently forces force-cavlc=1 --
            // CABAC off, which costs 10-15% compression efficiency at the SAME quality target. It also
            // keeps the 8x8 transform off. Together with the old ultrafast preset that put x264 at its
            // weakest possible configuration: at CRF 20 the picture carried visibly more coding noise
            // than it needed to. The latency properties zerolatency existed for are set explicitly in
            // x264-params below (no b-frames, no lookahead, no sync frames), so nothing regresses,
            // while CABAC + the 8x8 transform + aq-mode 2 + psy-rd put the same bits to better use.
            // superfast = one step above ultrafast: still dia ME / trellis 0, so per-frame cost only
            // rises modestly, but the entropy coder and transform size are the real quality.
            char presetBuf[32] = "superfast";
            {
                char e[32] = { 0 };
                DWORD n = GetEnvironmentVariableA("SCREENCAST_X264_PRESET", e, sizeof(e));
                if (n > 0 && e[0]) {   // escape hatch for a weak teacher PC: set ultrafast, no rebuild
                    strncpy(presetBuf, e, sizeof(presetBuf) - 1);
                    presetBuf[sizeof(presetBuf) - 1] = 0;
                }
            }
            av_opt_set(ctx_->priv_data, "preset", presetBuf, 0);
            av_opt_set(ctx_->priv_data, "crf", "20", 0);           // constrained quality: target CRF 20
            // keyint tracks the declared frame rate (one natural keyframe every 2s); scene-cut
            // detection stays off so a slide transition does not fire an extra IDR burst.
            // rc-lookahead=0 + sync-lookahead=0 + bframes=0 are what zerolatency actually provided for
            // latency; no-mbtree is required because mbtree needs lookahead frames to exist.
            char x264p[160];
            snprintf(x264p, sizeof(x264p),
                     "keyint=%d:min-keyint=%d:scenecut=0:bframes=0:rc-lookahead=0:sync-lookahead=0"
                     ":no-mbtree=1:sliced-threads=1:cabac=1:aq-mode=2:psy-rd=1.0",
                     fps * 2, fps * 2);
            av_opt_set(ctx_->priv_data, "x264-params", x264p, 0);
            fprintf(stderr, "[encode] x264 preset=%s crf=20 cabac=on aq=2 zerolatency=off\n", presetBuf);
        }

        // intentionally do NOT set AV_CODEC_FLAG_GLOBAL_HEADER: the keyframe carries SPS/PPS, the client can decode upon receiving a keyframe
        if (avcodec_open2(ctx_, codec, nullptr) == 0) {
            fprintf(stderr, "[encode] 使用编码器: %s, 编码 %dx%d\n", name, dstW, dstH);
            break;
        }
        fprintf(stderr, "[encode] 编码器 %s 打开失败, 尝试下一个...\n", name);
        avcodec_free_context(&ctx_);
        ctx_ = nullptr;
    }
    if (!ctx_) return false;

    // input frame + color conversion + scaling (BGRA source -> NV12 target size)
    frame_ = av_frame_alloc();
    frame_->format = AV_PIX_FMT_NV12;
    frame_->width  = dstW;
    frame_->height = dstH;
    if (av_frame_get_buffer(frame_, 32) < 0) { shutdown(); return false; }

    // adaptive scaling algorithm: downscaling (scale < 1.0) always uses LANCZOS -- text/icon downscale is the
    // highest-frequency path (e.g. 2K/5K -> 1080p or 1080p -> 720p, scale 0.5~0.9); BILINEAR blurs text;
    // upscaling (scale >= 1.0) uses BILINEAR: upscaling adds no new detail, LANCZOS only adds ringing artifacts
    // and CPU cost. The old version used 0.66 as the boundary, so downscales of 0.67~0.99 (the most common
    // preset drop) went through BILINEAR.
    int swsFlags;
    double scale = (double)dstW / srcW;
    double scaleH = (double)dstH / srcH;
    if (scaleH < scale) scale = scaleH;
    swsFlags = (scale < 1.0) ? SWS_LANCZOS : SWS_BILINEAR;

    // REUSE the existing sws context when the src/dst dimensions are unchanged: a bitrate-only rebuild
    // (adaptive bitrate AIMD) keeps the resolution, and recreating a 1080p LANCZOS sws context costs
    // ~100-200ms -- during which the stream stalls and the student screen flashes on every rate change.
    // Reusing it drops the rebuild to a few ms, so rate changes no longer interrupt the picture.
    if (!sws_ || swsSrcW_ != srcW || swsSrcH_ != srcH || swsDstW_ != dstW || swsDstH_ != dstH) {
        // source/target size changed (or first init): create a fresh conversion context
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        sws_ = sws_getContext(srcW, srcH, AV_PIX_FMT_BGRA,
                              dstW, dstH, AV_PIX_FMT_NV12,
                              swsFlags, nullptr, nullptr, nullptr);
        if (!sws_) { shutdown(); return false; }
        swsSrcW_ = srcW; swsSrcH_ = srcH; swsDstW_ = dstW; swsDstH_ = dstH;
    }
    // else: same source/target size -> reuse the existing conversion context (~100-200ms saved on bitrate-only rebuilds)

    pkt_ = av_packet_alloc();
    return true;
}

bool FFmpegEncoder::encode(const uint8_t* bgra, int stride, uint64_t captureMs) {
    if (!ctx_) return false;
    // consume the keyframe request (atomic flag, avoids directly mutating AVFrame across threads causing data races)
    // forced-IDR double safety: pict_type=I (encoder-level forced keyframe) + KEY flag (for packetization/client recognition),
    // reset immediately after sending, so subsequent frames are normal P frames (fixes the bandwidth waste of all-frame-IDR from a sticky key flag)
    if (requestKey_.exchange(false)) {
        frame_->pict_type = AV_PICTURE_TYPE_I;
        frame_->flags |= AV_FRAME_FLAG_KEY;
    }

    sws_scale(sws_, &bgra, &stride, 0, srcH_, frame_->data, frame_->linesize);
    frame_->pts = pts_++;
    int ret = avcodec_send_frame(ctx_, frame_);
    frame_->pict_type = AV_PICTURE_TYPE_NONE;              // reset, next frame returns to P frames
    frame_->flags &= ~(int)AV_FRAME_FLAG_KEY;
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "[encode] avcodec_send_frame failed: %s\n", err);
        requestKey_.store(true);   // frame not consumed, restore the IDR request, retry on the next frame (prevents lost request)
        return false;
    }
    while (avcodec_receive_packet(ctx_, pkt_) == 0) {
        bool key = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
        if (cb_) cb_(pkt_->data, pkt_->size, captureMs, key);
        av_packet_unref(pkt_);
    }
    return true;
}

void FFmpegEncoder::forceKeyframe() {
    requestKey_.store(true);   // consumed on the next encode(), thread-safe
}

void FFmpegEncoder::shutdown() {
    if (ctx_)   avcodec_free_context(&ctx_);
    if (frame_) av_frame_free(&frame_);
    if (pkt_)   av_packet_free(&pkt_);
    // NOTE: keep sws_ alive across shutdown/init: adaptive-bitrate rebuilds reuse the same-size conversion
    // context (~100-200ms saved), only the destructor frees it. Resolution changes recreate it in init().
}
