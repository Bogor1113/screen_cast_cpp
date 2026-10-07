#pragma once
// core/streamer.h -- teacher streaming engine: capture/encode/differential-send integrated
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "capture/dxgi_capture.h"
#include "encode/ffmpeg_encoder.h"
#include "net/udp_socket.h"
#include "packet/video_packet.h"

// the sender thread's bounded dequeue buffer: push is non-blocking, when the network is congested
// (queue full, 8MB) the whole frame is rejected -> selective whole-frame drop;
// popBatch is consumed exclusively by the sender thread, moving out at most 128 packets at once,
// large-frame bursts are gently throttled by the sender thread.
struct SendPacketQueue {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> pkts;
    size_t bytes = 0;
    bool closed = false;
    static constexpr size_t kMaxBytes = 8 * 1024 * 1024;

    // push: inside the lock, if bytes + pkt.size() > kMaxBytes return false (don't enqueue); otherwise enqueue and notify
    bool push(std::vector<uint8_t>&& pkt) {
        std::lock_guard<std::mutex> lk(mtx);
        if (bytes + pkt.size() > kMaxBytes) return false;   // queue full: drop the whole frame, don't block capture/encode threads
        pkts.push_back(std::move(pkt));
        bytes += pkts.back().size();
        cv.notify_one();
        return true;
    }

    // popBatch: wait on cv; returns the currently queued packets (moved out as a whole, at most maxPkts)
    // the byte count only subtracts what was actually moved this time, never zeroes it: for a large burst
    // of more than 128 packets (e.g. a 140+ fragment IDR keyframe) the queue still has leftover packets, so
    // zeroing would make push wrongly think the queue is empty and the 8MB cap would be breached by the
    // "leftover + new packet" sum, growing memory without bound under extreme congestion.
    std::vector<std::vector<uint8_t>> popBatch(size_t maxPkts = 128) {
        std::vector<std::vector<uint8_t>> out;
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [this] { return !pkts.empty() || closed; });
        size_t poppedBytes = 0;
        while (!pkts.empty() && out.size() < maxPkts) {
            out.push_back(std::move(pkts.front()));
            pkts.pop_front();
            poppedBytes += out.back().size();
        }
        bytes = bytes > poppedBytes ? bytes - poppedBytes : 0;
        return out;
    }

    void closeNotify() {
        std::lock_guard<std::mutex> lk(mtx);
        closed = true;
        cv.notify_all();
    }

    // reset before restart: after stop() closed=true and packets may remain; start() must clear them,
    // otherwise the new senderLoop exits immediately because closed is set, sending nothing
    void resetForStart() {
        std::lock_guard<std::mutex> lk(mtx);
        closed = false;
        pkts.clear();
        bytes = 0;
    }
};

// quality presets: ORIGINAL=source resolution / HD1080P=cap 1920x1080 / SD720P=cap 1280x720 / FLUENT=cap 960x540
// (a preset only downscales; if the source is smaller than the target it keeps the source size; weak-network
// classrooms choosing SD/FLUENT can significantly cut bandwidth and stutter)
enum class Quality { ORIGINAL = 0, HD1080P = 1, SD720P = 2, FLUENT = 3 };

// statistics snapshot (plain values, copied by snapshot())
struct StreamStats {
    uint64_t framesCaptured = 0;   // number of captured frames
    uint64_t framesSent = 0;       // number of encoded and sent frames
    uint64_t framesSkipped = 0;    // number of frames skipped by differential mode
    uint64_t packetsSent = 0;
    uint64_t bytesSent = 0;
    uint64_t sendDropFrames = 0;   // number of whole-frame drops when the send queue is full (network congestion, client will request a keyframe rebuild)
    int encWidth = 0, encHeight = 0;   // encoding resolution
    int srcWidth = 0, srcHeight = 0;   // source resolution
    int targetKbps = 0;                // current effective target bitrate (preset/manual/adaptive)
    bool gdiMode = false;              // capture mode: true=GDI fallback (remote/virtual display), false=DXGI
};

class Streamer {
public:
    // preview callback (called from the capture thread; args: BGRA raw image + changed region)
    using PreviewCallback = std::function<void(const uint8_t* bgra, int w, int h, int stride,
                                               const RECT* dirty, int dirtyCount)>;

    Streamer();
    ~Streamer();

    bool start(const std::string& targetIP, uint16_t videoPort, uint16_t ctrlPort,
               const std::string& localIp = "");   // localIp: multicast egress interface (auto-detected if empty)
    void stop();

    void setQuality(Quality q);            // callable from any thread, takes effect on the capture thread (rebuilds the encoder)
    void setBitrate(int kbps);             // callable from any thread, overrides the preset default bitrate (0=preset default)
    void setAdaptiveBitrate(bool on);      // adaptive bitrate: AIMD control from client-reported loss (floor 400kbps ~ ceiling)
    bool adaptiveEnabled() const { return adaptive_.load(); }
    int  currentBitrateKbps() const;       // current effective target bitrate (preset default / manual override / adaptive target)
    void setDifferentialEnabled(bool on);  // differential-screen toggle
    void setPreviewCallback(PreviewCallback cb);
    // exclude the teacher's own window area (screen coords): that region in the sent picture is filled with a base color, preventing feedback flicker
    void setSelfExcludeRect(const RECT& rc, bool enabled);

    bool isRunning() const { return running_.load(); }
    StreamStats snapshot() const;
    // online client IP list (clients report HELLO heartbeat every 2s, considered offline if silent for 10s)
    std::vector<std::string> clientIps() const;

private:
    void captureLoop();
    void controlLoop();
    void senderLoop();           // dedicated send thread: consumes packets from sendQueue_ and sendTo's them, never blocks the capture thread
    bool initEncoder(int srcW, int srcH);
    void onFrame(const CapturedFrame& f);
    int  presetKbps(Quality q) const;      // default bitrate for a quality preset
    int  ceilingKbps() const;              // adaptive ceiling: manual override if set, else the preset default
    int  effectiveKbps() const;            // bitrate the encoder should use right now (manual/preset/adaptive)
    void adaptiveStep(uint64_t now);       // AIMD control step (control thread, ~1Hz)

    DXGICapture capture_;
    FFmpegEncoder enc_;
    UDPSocket videoSock_, ctrlSock_;
    std::string targetIP_;
    uint16_t videoPort_ = 5004, ctrlPort_ = 5005;

    std::thread captureThread_, ctrlThread_, senderThread_;
    std::atomic<bool> running_{ false };
    SendPacketQueue sendQueue_;                // bounded buffer between capture/encode -> send thread
    std::atomic<uint64_t> sendDropFrames_{ 0 };

    std::atomic<int> quality_{ (int)Quality::ORIGINAL };
    std::atomic<int> bitrateKbps_{ 0 };     // 0=preset default bitrate; >0 override (manual lower bitrate on weak networks)
    std::atomic<bool> adaptive_{ false };   // adaptive bitrate (AIMD from client-reported loss)
    std::atomic<int>  adaptiveCur_{ 0 };    // adaptive controller's current target (0 = ceiling, not yet adjusted)
    std::atomic<bool> differential_{ true };
    std::atomic<bool> keyframeReq_{ false };
    int  activeQuality_ = -1;
    int  lastInitKbps_ = -1;        // bitrate the current encoder was built with (capture thread), -1 = not built yet
    int  curSrcW_ = 0, curSrcH_ = 0;   // used on the capture thread
    bool firstFrame_ = true;
    uint64_t lastSentMs_ = 0;
    uint64_t lastKeyMs_ = 0;        // when the last forced keyframe occurred (rate-limited: 1 IDR per second)
    uint16_t frameId_ = 0;
    uint64_t lastAdaptiveDecMs_ = 0;   // when the last AIMD decrease happened (control thread): 3s cooldown
    uint64_t lastAdaptiveIncMs_ = 0;   // when the last AIMD increase happened (control thread): slow probe-up, 10s cooldown
    uint64_t lastAdaptiveChangeMs_ = 0;   // when ANY adaptive rate change happened: increases require 10s stability since any change (anti-oscillation)
    uint32_t highLossStreak_ = 0;     // consecutive 2s report windows with loss >= 3% (sustained-congestion gate: a transient spike must not rebuild the encoder)
    uint64_t lastHighReportMs_ = 0;   // timestamp of the last report that advanced the streak (only fresh reports count)

    PreviewCallback previewCb_;
    std::mutex cbMtx_;

    // stats (written by capture thread, read by snapshot)
    std::atomic<uint64_t> framesCaptured_{ 0 }, framesSent_{ 0 }, framesSkipped_{ 0 };
    std::atomic<uint64_t> packetsSent_{ 0 }, bytesSent_{ 0 };
    std::atomic<int> encW_{ 0 }, encH_{ 0 }, statSrcW_{ 0 }, statSrcH_{ 0 };

    // online-client tracking (written by controlLoop thread, read by clientIps under lock)
    mutable std::mutex peerMtx_;
    std::map<std::string, uint64_t> peers_;   // client IP -> last heartbeat time (ms)
    // client loss reports for adaptive bitrate (client IP -> {loss permille 0..1000, report time ms}), guarded by peerMtx_
    // NOTE: adaptiveStep aggregates these at the P90 percentile, NOT the single worst client -- see adaptiveStep.
    std::map<std::string, std::pair<uint32_t, uint64_t>> lossReports_;

    PacketBufferPool pool_;   // recycles the packet fragment buffers between the capture (acquire) and send (release) threads
};
