// decode/ffmpeg_decoder.cpp -- client H.264 decoding implementation
#include "decode/ffmpeg_decoder.h"
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
}

// --- hardware decoding (D3D11VA), OFF BY DEFAULT -------------------------------------------------
// The reasoning is sound in principle: the H.264 entropy decode + motion compensation + inverse
// transform is the most expensive part of the client, and every GPU since ~2011 can do it (D3D11VA).
//
// MEASURED, and the reason it is not the default: this client has to hand the picture to its OWN
// renderer, so a hardware-decoded frame must first be pulled back into system memory
// (av_hwframe_transfer_data = a synchronised VRAM->RAM copy every frame). On the development machine
// (Intel UHD + RTX 5060, 1080p @ 53fps) that readback costs more than the software decode it replaces:
//
//     software decode ............ 19-21% of one core, 819 handles, 68 threads, 167MB private
//     D3D11VA + readback ......... 37.1% of one core, 948 handles, 85 threads, 203MB private
//
// So shipping it as the default would be a straight regression. The real win needs a ZERO-COPY path --
// the renderer sampling the decoded D3D11 texture directly instead of a CPU BGRA copy -- which is a
// larger change to the render pipeline. Until that exists the hardware path is opt-in, and the code is
// kept (working, tested, with its own fallbacks) so it can be enabled the moment zero-copy lands.
//
// Set SCREENCAST_ENABLE_HW_DECODE=1 to enable it. The fallback is inherent, not bolted on:
// av_hwdevice_ctx_create() failing leaves hwDev_ null, and if the offered pixel-format list has no
// AV_PIX_FMT_D3D11 libavcodec simply stays on software. A driver that advertises D3D11VA but cannot
// deliver a picture is caught by the transfer-failure counter below.
static bool hardwareDecodeEnabled() {
    static const bool enabled = [] {
        char b[8] = { 0 };
        DWORD n = GetEnvironmentVariableA("SCREENCAST_ENABLE_HW_DECODE", b, sizeof(b));
        return n > 0 && b[0] == '1';
    }();
    return enabled;
}

static enum AVPixelFormat pickHwFormat(AVCodecContext* c, const AVPixelFormat* fmts) {
    (void)c;
    for (const AVPixelFormat* p = fmts; p && *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    return fmts ? fmts[0] : AV_PIX_FMT_NONE;
}

// --- hardening constants -------------------------------------------------------------------------
// A corrupt/absurd SPS can advertise a gigantic picture size. Allocating it would either throw
// bad_alloc inside the decode thread (bad_alloc in a thread = std::terminate = process death) or
// succeed and eat all RAM. 8192x8192 BGRA is already 256MB, far beyond any teaching projector.
static constexpr int    kMaxDim = 8192;
// libswscale's SIMD back-ends may write a few bytes past the last pixel of the final row when the
// width is not a multiple of their block size; sizing the destination to exactly w*h*4 put that write
// into the allocator's bookkeeping (heap corruption -> a crash somewhere completely unrelated, later).
// 64 bytes of slack is enough for every x86/ARM swscale kernel.
static constexpr size_t kSwsSlack = 64;

// whether the frame data contains an SPS NAL (type 7, Annex-B start code) -- used as the decoder sync point
static bool hasSpsNal(const uint8_t* data, int size) {
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    while (p + 3 <= end) {
        int sc = 0;
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) sc = 3;                                  // 00 00 01
        else if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) sc = 4; // 00 00 00 01
        if (!sc) { ++p; continue; }
        const uint8_t* nal = p + sc;
        if (nal >= end) break;              // no type byte after the start code (prevent out-of-bounds read)
        if ((nal[0] & 0x1F) == 7) return true;   // SPS
        // jump to the next start code (supports 3/4 byte)
        const uint8_t* q = p + sc;
        while (q + 3 <= end) {
            if (q[0] == 0 && q[1] == 0 && q[2] == 1) break;
            if (q + 4 <= end && q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) break;
            ++q;
        }
        p = q;
    }
    return false;
}

FFmpegDecoder::~FFmpegDecoder() {
    if (ctx_)   avcodec_free_context(&ctx_);
    if (pkt_)   av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (swFrame_) av_frame_free(&swFrame_);
    if (sws_)   sws_freeContext(sws_);
    releaseHw();
}

void FFmpegDecoder::releaseHw() {
    if (hwDev_) av_buffer_unref(&hwDev_);
    hwActive_ = false;
    hwTransferFails_ = 0;
    // hwFailed_ is deliberately NOT cleared: once a machine's driver has proved it cannot deliver
    // D3D11VA pictures, every later rebuild stays on software instead of stuttering once more.
}

void FFmpegDecoder::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (ctx_) { avcodec_flush_buffers(ctx_); avcodec_free_context(&ctx_); }
    if (pkt_)   av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (swFrame_) av_frame_free(&swFrame_);
    if (sws_)   sws_freeContext(sws_);
    sws_ = nullptr; pkt_ = nullptr; frame_ = nullptr; swFrame_ = nullptr;
    width_ = 0; height_ = 0; ptsMs_ = 0;
    bgra_.clear();
    extradata_.clear();
    failStreak_ = 0;
    releaseHw();
}

bool FFmpegDecoder::ready() const { std::lock_guard<std::mutex> lk(mtx_); return ctx_ != nullptr; }
const uint8_t* FFmpegDecoder::bgra() const { std::lock_guard<std::mutex> lk(mtx_); return bgra_.empty() ? nullptr : bgra_.data(); }
bool FFmpegDecoder::copyBgra(std::vector<uint8_t>& out, int& w, int& h) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (bgra_.empty() || width_ <= 0 || height_ <= 0) return false;
    out = bgra_;         // copy under the lock: the buffer can't be freed mid-copy by close()
    w = width_; h = height_;
    return true;
}
bool FFmpegDecoder::takeBgra(std::vector<uint8_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (bgra_.empty() || width_ <= 0 || height_ <= 0) return false;
    // swap, not copy: the caller receives the pixels and this decoder keeps the caller's old buffer as
    // its scratch. Feeds the client's frame pool, so a steady stream performs ZERO full-frame memcpy
    // and ZERO heap traffic (the decode thread hands the same few buffers round-robin forever).
    bgra_.swap(out);
    w = width_; h = height_;
    return true;
}
int  FFmpegDecoder::width()  const { std::lock_guard<std::mutex> lk(mtx_); return width_; }
int  FFmpegDecoder::height() const { std::lock_guard<std::mutex> lk(mtx_); return height_; }
uint64_t FFmpegDecoder::ptsMs() const { std::lock_guard<std::mutex> lk(mtx_); return ptsMs_; }

bool FFmpegDecoder::open(const uint8_t* data, int size) {
    // extract SPS/PPS from the first Annex-B frame as extradata, letting the decoder self-sync
    extradata_.clear();
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    while (p + 4 <= end) {
        // find a start code 00 00 01 / 00 00 00 01
        int sc = 0;
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) sc = 3;
        else if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) sc = 4;
        if (!sc) { ++p; continue; }
        const uint8_t* nal = p + sc;
        if (nal >= end) break;
        uint8_t type = nal[0] & 0x1F;
        if (type == 7 || type == 8) {   // SPS(7) / PPS(8)
            const uint8_t* next = nal;
            while (next + 4 <= end && !(next[0] == 0 && next[1] == 0 && (next[2] == 1 || (next[2] == 0 && next[3] == 1))))
                ++next;
            extradata_.insert(extradata_.end(), p, next);
            p = next;
        } else {
            break;
        }
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;

    // ---- hardware decode attempt (D3D11VA), opt-in; see the measurement above ----
    if (hardwareDecodeEnabled() && !hwFailed_ && !hwDev_) {
        if (av_hwdevice_ctx_create(&hwDev_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
            hwDev_ = nullptr;   // no GPU / remote session / driver without the decoder -> software
        hwActive_ = (hwDev_ != nullptr);
    }
    if (hwDev_) {
        ctx_->hw_device_ctx = av_buffer_ref(hwDev_);   // may legally be null if the ref failed
        if (ctx_->hw_device_ctx) ctx_->get_format = pickHwFormat;
        else hwActive_ = false;
    }

    if (!extradata_.empty()) {
        ctx_->extradata = (uint8_t*)av_mallocz(extradata_.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(ctx_->extradata, extradata_.data(), extradata_.size());
        ctx_->extradata_size = (int)extradata_.size();
    }
    ctx_->thread_count = 1;          // low latency first, avoids multithread buffering
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        releaseHw();
        return false;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) {
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        avcodec_free_context(&ctx_);
        releaseHw();
        return false;
    }
    return true;
}

bool FFmpegDecoder::decode(const uint8_t* data, int size, uint64_t ptsMs) {
    std::lock_guard<std::mutex> lk(mtx_);   // serialize against close() from the reconnect path
    // lazy init: only build the decoder from a sync frame (keyframe) containing SPS
    // if initialized with a P frame (no SPS/PPS), the decoder could never output a picture -- must wait for the next keyframe to retry
    if (!ctx_) {
        if (!hasSpsNal(data, size)) return false;   // not a sync frame, skip, wait for a keyframe
        if (!open(data, size)) return false;
    }
    ptsMs_ = ptsMs;

    // copy the input into the packet's own buffer: the caller's buffer (the decode thread's popped
    // queue item) is overwritten on the next pop, so referencing it directly could leave FFmpeg reading
    // freed memory if send_packet doesn't fully consume it synchronously (e.g. EAGAIN on B-frame reorder).
    if (av_new_packet(pkt_, size) < 0) return false;
    memcpy(pkt_->data, data, size);
    pkt_->pts = (int64_t)ptsMs;      // pass through the timestamp (for render pacing / latency stats)

    int sr = avcodec_send_packet(ctx_, pkt_);
    if (sr == AVERROR(EAGAIN)) {
        // The decoder's output queue is still full. The old code returned right here, which left the
        // context permanently "full": every later packet got EAGAIN too, no frame was ever produced
        // again and the picture stayed frozen/black with no way back. Drain the pending output first
        // and retry the same packet once instead.
        while (avcodec_receive_frame(ctx_, frame_) == 0) av_frame_unref(frame_);
        sr = avcodec_send_packet(ctx_, pkt_);
    }
    av_packet_unref(pkt_);
    if (sr < 0 && sr != AVERROR(EAGAIN)) { ++failStreak_; return false; }
    failStreak_ = 0;

    while (avcodec_receive_frame(ctx_, frame_) == 0) {
        // A hardware picture lives in a D3D11 texture: bring it into system memory (NV12) first, then
        // run the same conversion as the software path. `pic` is the frame we actually read pixels from.
        AVFrame* pic = frame_;
        if (frame_->format == AV_PIX_FMT_D3D11 || frame_->hw_frames_ctx) {
            if (!swFrame_) swFrame_ = av_frame_alloc();
            if (!swFrame_) { av_frame_unref(frame_); return false; }
            av_frame_unref(swFrame_);
            if (av_hwframe_transfer_data(swFrame_, frame_, 0) < 0) {
                av_frame_unref(frame_);
                // A driver that advertises D3D11VA but cannot hand back a picture must not leave the
                // student with a black window: after a handful of failures switch to software for good
                // and let the client's existing self-heal rebuild the context (failStreak_ is pushed
                // past its threshold, so the decode thread closes and re-opens it on software).
                if (++hwTransferFails_ >= 5) {
                    hwFailed_ = true;
                    hwActive_ = false;
                    failStreak_ = 1000;
                }
                return false;
            }
            pic = swFrame_;
            hwTransferFails_ = 0;
        }
        // convert to BGRA by the decoder's actual output format (key: libx264 outputs yuv420p, not NV12;
        // fixing on NV12 would misalign the chroma planes -> whitish/green picture; here choose dynamically by frame->format).
        // Note this also covers the hardware path, whose transferred frames come back as NV12.
        int w = pic->width, h = pic->height;
        // sanity gate: a corrupt SPS otherwise turns into a huge allocation inside this thread, and an
        // uncaught bad_alloc in a std::thread calls std::terminate() -- the process dies with no dump.
        if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim) {
            ++failStreak_;
            av_frame_unref(frame_);
            return false;
        }
        AVPixelFormat srcFmt = (AVPixelFormat)pic->format;
        // grow-only sizing: the buffer is recycled between the decoder, the frame pool and the widget,
        // so realloc happens at most once when the resolution first appears (or changes) and never again
        const size_t need = (size_t)w * h * 4 + kSwsSlack;
        if (bgra_.size() < need) bgra_.resize(need);
        if (!sws_ || swsSrcW_ != w || swsSrcH_ != h || swsSrcFmt_ != (int)srcFmt) {
            if (sws_) sws_freeContext(sws_);
            // CLARITY NOTE (measured, kept behind a switch): the decode output is 4:2:0 -- one chroma
            // sample is shared between 2x2 pixel blocks. SWS_FULL_CHR_H_INT does the full chroma
            // interpolation, which removes the colour fringing around red/blue text and thin UI
            // outlines, i.e. it is the theoretically correct setting for lecture content.
            // BUT it measured ~3x the conversion cost per frame (client 0.36%/frame -> ~1.1%/frame at
            // 1080p), and per-frame CPU is exactly what the 1366x768 classroom machines cannot spare
            // (see round 5). So the smooth default stays plain BILINEAR; machines that report colour
            // fringing AND have CPU headroom set SCREENCAST_FULL_CHROMA=1 to opt in. The proper zero
            // cost fix is sampling chroma in the D3D shader (the NV12 render path), not in sws.
            static const int kFastFlags = SWS_BILINEAR | SWS_ACCURATE_RND;
            int flags = kFastFlags;
            {
                char e[8] = { 0 };
                if (GetEnvironmentVariableA("SCREENCAST_FULL_CHROMA", e, sizeof(e)) > 0 && e[0] == '1')
                    flags = kFastFlags | SWS_FULL_CHR_H_INT;
            }
            sws_ = sws_getContext(w, h, srcFmt, w, h, AV_PIX_FMT_BGRA, flags, nullptr, nullptr, nullptr);
            if (!sws_)   // some driver/format combinations reject full-chroma: fall back, never drop frames
                sws_ = sws_getContext(w, h, srcFmt, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            swsSrcW_ = w; swsSrcH_ = h; swsSrcFmt_ = (int)srcFmt;
        }
        if (!sws_) { av_frame_unref(frame_); return false; }   // conversion context creation failed (unsupported format/OOM), skip this frame to prevent a crash
        uint8_t* dst = bgra_.data();
        int dstStride = w * 4;
        sws_scale(sws_, pic->data, pic->linesize, 0, h, &dst, &dstStride);
        width_ = w; height_ = h;
        av_frame_unref(frame_);
        return true;
    }
    return false;   // no frame produced (e.g. P frame before keyframe)
}
