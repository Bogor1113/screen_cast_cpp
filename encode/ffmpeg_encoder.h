#pragma once
// encode/ffmpeg_encoder.h -- FFmpeg ultra-low-latency H.264 encoder
#include <atomic>
#include <cstdint>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

class FFmpegEncoder {
public:
    using PacketCallback = std::function<void(const uint8_t* data, int size,
                                              uint64_t captureMs, bool keyframe)>;

    // srcW/srcH: input BGRA source size; dstW/dstH: encoding size (can scale, implements quality presets)
    bool init(int srcW, int srcH, int dstW, int dstH, int fps, int bitrateKbps, PacketCallback cb);
    bool encode(const uint8_t* bgra, int stride, uint64_t captureMs);  // feed one BGRA frame
    void forceKeyframe();   // thread-safe: request the next frame be an IDR keyframe
    void shutdown();

private:
    AVCodecContext* ctx_ = nullptr;
    SwsContext*     sws_ = nullptr;
    AVFrame*        frame_ = nullptr;
    AVPacket*       pkt_ = nullptr;
    PacketCallback  cb_;
    int64_t         pts_ = 0;
    int             srcH_ = 0;                          // sws_scale source height
    int swsSrcW_ = 0, swsSrcH_ = 0, swsDstW_ = 0, swsDstH_ = 0;   // sws context's src/dst size (reuse on bitrate-only rebuilds)
    std::atomic<bool> requestKey_{ false };   // control thread writes, encode thread consumes, avoids cross-thread AVFrame mutation
};
