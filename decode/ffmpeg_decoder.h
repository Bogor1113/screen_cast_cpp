#pragma once
// decode/ffmpeg_decoder.h -- client H.264 decoder (auto-configured, outputs BGRA)
#include <cstdint>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

class FFmpegDecoder {
public:
    ~FFmpegDecoder();
    // reset the decoder state (called on reconnect, clears internal buffers so stale data doesn't interfere).
    // thread-safe: the reconnect edge runs on the GUI thread while the decode thread may be decoding -- the
    // internal mutex serializes close() against decode() so avcodec_free_context never races send_packet.
    void close();
    // feed one H.264 frame (Annex-B, can be sent directly to avcodec_send_packet), outputs a BGRA frame
    // returns true when this frame decoded a new picture
    bool decode(const uint8_t* data, int size, uint64_t ptsMs);

    bool ready() const;
    const uint8_t* bgra()  const;
    // thread-safe copy of the last decoded BGRA frame + its size. Prefer this over bgra()/width()/height()
    // in the decode thread: the reconnect path calls close() from another thread, which clears bgra_ -- so a
    // pointer returned by bgra() (lock released) can dangle by the time the caller copies it (use-after-free).
    bool copyBgra(std::vector<uint8_t>& out, int& w, int& h) const;
    // Zero-copy variant used by the GUI client: SWAPS the decoded pixels into `out` and keeps `out`'s old
    // buffer as its own scratch, so a steady-resolution stream does no allocation and no extra frame copy
    // at all (the caller hands the buffer straight on to the frame pool). Same locking/thread-safety
    // contract as copyBgra(): safe to call while another thread may call close().
    bool takeBgra(std::vector<uint8_t>& out, int& w, int& h);
    // Hand the decoder a scratch buffer to convert into, taking back whatever it was holding. Call this
    // BEFORE decode() with a correctly-sized pooled buffer: the sws destination is then already large
    // enough and decode() never has to grow it (vector::resize value-initialises the grown range, i.e.
    // it would memSET ~7.5MB per frame). Passing an empty vector is safe, it just means the next decode
    // allocates. The caller's buffer is returned empty again by takeBgra().
    void prepareScratch(std::vector<uint8_t>& buf) { std::lock_guard<std::mutex> lk(mtx_); bgra_.swap(buf); }
    int  width()  const;
    int  height() const;
    uint64_t ptsMs() const;
    // consecutive avcodec_send_packet failures. The client uses this to rebuild the decoder instead of
    // sitting on a permanently desynced context (which shows up as "the picture froze / went black
    // and never came back"): once it crosses a threshold the decoder is closed and re-synced from the
    // next keyframe.
    int failStreak() const { std::lock_guard<std::mutex> lk(mtx_); return failStreak_; }
    void resetFailStreak() { std::lock_guard<std::mutex> lk(mtx_); failStreak_ = 0; }
    // true while decoding on the GPU (D3D11VA). Surfaced in the client title bar: it is the fastest way
    // to tell whether a "low-spec client uses too much CPU" report is about a machine that fell back to
    // software decoding. Also the switch used to exercise the software path in testing.
    bool hardwareDecoding() const { std::lock_guard<std::mutex> lk(mtx_); return hwActive_; }

private:
    bool open(const uint8_t* data, int size);   // lazy init, self-syncs from the first frame's SPS/PPS (caller holds mtx_)
    void releaseHw();                           // drop the D3D11 device + the generated pixel-format callback
    // decode() and the accessors are serialised by mtx_, and every one of them can be entered from a
    // different thread than decode() (close() runs on the reconnect path), so they are all recursive-safe
    // by virtue of never calling each other.
    mutable std::mutex mtx_;   // serializes decode()/close()/accessors (reconnect closes from another thread)
    AVCodecContext* ctx_ = nullptr;
    AVPacket*       pkt_ = nullptr;
    AVFrame*        frame_ = nullptr;
    AVFrame*        swFrame_ = nullptr;   // system-memory landing frame for hardware-decoded pictures
    AVBufferRef*    hwDev_ = nullptr;     // D3D11VA device (null = software decoding)
    bool            hwActive_ = false;    // the current context really is hardware-backed
    bool            hwFailed_ = false;    // hardware failed -> stay on software until the context is rebuilt
    int             hwTransferFails_ = 0;
    SwsContext*     sws_ = nullptr;
    int64_t         swsSrcW_ = 0, swsSrcH_ = 0;
    int             swsSrcFmt_ = -1;   // sws input pixel format (per the decoder's actual output, prevents NV12/yuv420p misalignment)
    std::vector<uint8_t> bgra_;                  // decoded output buffer
    std::vector<uint8_t> extradata_;             // SPS/PPS extraction cache
    int width_ = 0, height_ = 0;
    uint64_t ptsMs_ = 0;
    int failStreak_ = 0;                         // consecutive send_packet failures (see failStreak())
};
