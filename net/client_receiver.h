#pragma once
// net/client_receiver.h -- client receive thread: UDP receive + fragment reassembly (FEC recovery) + callback for complete frames
// supports unicast (default) and multicast (pass a multicast group addr): under multicast the video stream uses the group, KEYFRAME control still goes unicast to the teacher
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "net/udp_socket.h"
#include "packet/fragment_reassembler.h"

// called when a whole frame (encoded data) is received; args: frame data/size/capture timestamp/whether keyframe
struct ReceivedFrame {
    const std::vector<uint8_t>* data = nullptr;
    uint64_t captureMs = 0;
    bool keyframe = false;
};

class ClientReceiver {
public:
    using FrameCallback = std::function<void(const ReceivedFrame&)>;

    // group: multicast group addr (e.g. "239.255.1.1"); empty = unicast receive
    bool start(const std::string& teacherIP, const std::string& group,
               uint16_t videoPort, uint16_t ctrlPort, FrameCallback cb);
    void stop();   // idempotent, safe to call multiple times

    // hook into the "first decode succeeded" flag: until the first frame is truly decoded, the
    // receive thread keeps requesting keyframes (300ms rate-limited), fixing the black-screen
    // stall of "first keyframe lost, only P frames after, decoder never joins" (one root cause of client black screen)
    void setDecodePrimed(std::atomic<bool>* primed) { primed_ = primed; }

    uint64_t packets() const { return packets_.load(); }
    uint64_t frames()  const { return frames_.load(); }
    uint64_t lostFrames() const { return lostFrames_.load(); }   // lost frames accumulated by completed-frame-number gaps
    uint64_t recoveredFrames() const { return recoveredFrames_.load(); }  // frames recovered via FEC parity
    uint64_t abandonedFrames() const { return abandonedFrames_.load(); }  // frames abandoned (partial, timed out) = real loss the gap-count misses

    // timestamp when the last complete frame arrived (GetTickCount64 baseline, 0 = none received yet).
    // the client GUI uses this to detect a teacher stream outage (no frame for >5s) and shows "connection interrupted" in the title bar.
    uint64_t lastFrameMs() const { return lastFrameArriveMs_.load(); }

    // reset the frame-sequence tracking (loss-gap detection) for a clean re-join: after a long outage the
    // teacher may have restarted and its frameId counter reset, which would otherwise fake a huge loss gap
    void requestReset() { resetSeq_.store(1); }

    // teacher pointer state (receive thread writes, main thread reads): returns false = no cursor packet received yet
    bool cursorState(float& nx, float& ny, bool& visible) const {
        if (!haveCursor_.load()) return false;
        nx = cursorNX_.load(); ny = cursorNY_.load();
        visible = cursorVisible_.load() != 0;
        return true;
    }

    // teacher's captured source resolution (0 = not yet received). used for the client's true 1:1 view.
    void sourceSize(uint16_t& w, uint16_t& h) const { w = srcW_.load(); h = srcH_.load(); }

private:
    void loop();
    UDPSocket videoSock_, ctrlSock_;
    std::string teacherIP_, group_;
    uint16_t videoPort_ = 5004, ctrlPort_ = 5005;
    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<uint64_t> packets_{ 0 }, frames_{ 0 }, lostFrames_{ 0 }, recoveredFrames_{ 0 }, abandonedFrames_{ 0 };
    std::atomic<uint64_t> lastFrameArriveMs_{ 0 };   // arrival time of the last complete frame (0=none yet), used by GUI stream-outage detection
    std::atomic<uint8_t> resetSeq_{ 0 };   // GUI writes 1 on reconnect: clear sequence tracking on the loop thread
    std::atomic<bool>* primed_ = nullptr;   // caller-held "first frame decoded" flag (may be null)
    uint16_t lastFrameId_ = 0;      // loss detection: previous "assembly-complete" frame number (loop thread only)
    bool     haveLastFrame_ = false;
    FrameCallback cb_;
    std::mutex cbMtx_;
    std::atomic<float> cursorNX_{ 0.0f }, cursorNY_{ 0.0f };   // pointer normalized coords
    std::atomic<uint8_t> cursorVisible_{ 0 };                  // pointer visibility
    std::atomic<uint8_t> haveCursor_{ 0 };                     // whether a cursor packet has been received
    std::atomic<uint16_t> srcW_{ 0 }, srcH_{ 0 };              // teacher captured source resolution (for 1:1 view)
};