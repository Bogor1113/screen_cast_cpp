// net/client_receiver.cpp -- client receive thread implementation
#include "net/client_receiver.h"
#include <cstdio>
#include <cstring>
#include <exception>

static int64_t nowMs() { return (int64_t)GetTickCount64(); }

bool ClientReceiver::start(const std::string& teacherIP, const std::string& group,
                           uint16_t videoPort, uint16_t ctrlPort, FrameCallback cb) {
    if (running_.load()) return false;
    teacherIP_ = teacherIP; group_ = group;
    videoPort_ = videoPort; ctrlPort_ = ctrlPort;
    { std::lock_guard<std::mutex> lk(cbMtx_); cb_ = std::move(cb); }

    if (!videoSock_.createSocket()) return false;
    if (!group_.empty()) {
        // pin the group membership to the outbound NIC: joining on INADDR_ANY (all interfaces) makes
        // a multi-NIC client (or a single-machine loopback where the stream reaches us via both the
        // NIC and the loopback) receive every packet twice -> frameId reuse -> decoder corruption.
        std::string iface = UDPSocket::detectLocalIPv4();
        videoSock_.bindAndJoinGroup(group_, videoPort_, iface);   // multicast: join the group on one NIC
    } else
        videoSock_.bindUnicast(videoPort_);                // unicast: just bind the port
    videoSock_.setRecvBuffer(8 * 1024 * 1024);   // 8MB receive buffer, withstands bursts
    videoSock_.setRecvTimeout(100);              // 100ms timeout: lets stop() exit promptly

    if (!ctrlSock_.createSocket()) { videoSock_.close(); return false; }   // don't leak the video socket

    running_.store(true);
    thread_ = std::thread(&ClientReceiver::loop, this);
    return true;
}

void ClientReceiver::stop() {
    if (!running_.exchange(false)) return;
    videoSock_.close();          // closesocket makes a blocked recvFrom return immediately
    ctrlSock_.close();
    if (thread_.joinable()) thread_.join();
}

void ClientReceiver::loop() {
    FragmentReassembler reassembler;
    char buf[1500];
    int64_t lastKeyReq = 0;      // when the last KEYFRAME was sent
    int64_t lastHello = 0;       // when the last HELLO heartbeat was sent
    int64_t lastFrameMs = 0;     // when the last complete frame was received (0 = none received yet)
    uint64_t lastLost = 0;       // last observed lost-frame count (for triggering requests on loss)
    uint64_t lastRecovered = 0;  // last synced FEC recovered-frame count
    uint64_t lastAbandoned = 0;  // last synced abandoned-frame count (real loss signal)
    uint64_t lastReportLost = 0, lastReportFrames = 0;   // loss-report deltas over the last window
    uint32_t smoothedLoss = 0;                           // EMA-smoothed loss permille (noise filter)

    // wrap the loop in a catch-all: reassembly/queue operations allocate (unordered_map/vector), and a
    // bad_alloc here would otherwise propagate to the thread boundary and abort the whole process with no
    // crash dump (std::terminate, not a SEH access violation). Logging and stopping the thread is far safer.
    try {
    while (running_.load()) {
        int64_t now = nowMs();
        // reconnect reset: the GUI detected an outage and asked to clear the sequence tracking,
        // so the teacher's restarted (or new) frame numbering doesn't fake a huge loss gap
        if (resetSeq_.exchange(0)) {
            haveLastFrame_ = false;
            lastFrameId_ = 0;
            lastLost = lostFrames_.load();   // re-sync the loss-trigger baseline too
            reassembler.reset();             // clear pending frames + completed history (teacher may have restarted)
        }
        // intelligent KEYFRAME request (replaces the old unconditional every-2s):
        // 1) not yet "decoded into picture" (just started/just joined the group/first keyframe lost): keep requesting until the
        //    decoder actually produces a first frame -- this is the key to fixing "client stays black": only receiving P frames
        //    cannot initialize the decoder
        // 2) lost frames detected 3) no complete frame for >3s (stalled stream)
        // otherwise no request -> control traffic is almost zero in steady state, the teacher is only bothered by new/join/recovery scenarios
        bool notPrimed = primed_ ? !primed_->load() : (lastFrameMs == 0);
        bool needKey = notPrimed || (now - lastFrameMs > 3000);
        if (lostFrames_.load() > lastLost) {   // growing lost count = incomplete data, needs keyframe rebuild
            needKey = true;
            lastLost = lostFrames_.load();
        }
        if (needKey && now - lastKeyReq > 300) {   // merged request, at most 1 per 300ms
            lastKeyReq = now;
            ctrlSock_.sendTo(teacherIP_, ctrlPort_, "KEYFRAME", 8);
        }
        // heartbeat + loss report every 2s: HELLO keeps us in the teacher's online list, REPORT feeds the
        // teacher's adaptive-bitrate controller with our loss rate over the last 2s window (permille 0..1000)
        if (now - lastHello > 2000) {
            lastHello = now;
            ctrlSock_.sendTo(teacherIP_, ctrlPort_, "HELLO", 5);
            uint64_t nLost = lostFrames_.load(), nFrames = frames_.load();
            uint64_t dLost = nLost - lastReportLost, dFrames = nFrames - lastReportFrames;
            lastReportLost = nLost; lastReportFrames = nFrames;
            // require >=2 frames in the window to count real loss: on a static desktop differential mode sends
            // only ~1 keep-alive frame/2s, so a single lost frame would falsely read as 100% loss -> spurious
            // bitrate cut. Loss is only meaningful when frames are actually flowing (active desktop, ~60/window).
            uint32_t raw = (dFrames >= 2) ? (uint32_t)(dLost * 1000 / dFrames) : 0;
            if (raw > 1000) raw = 1000;
            smoothedLoss = smoothedLoss ? (smoothedLoss * 3 + raw) / 4 : raw;   // EMA: transient spikes don't punch through
            uint32_t permille = smoothedLoss;
            // a fully-stalled stream (no complete frame for >3s) is 100% loss for this client, report it as such
            if (lastFrameMs > 0 && now - lastFrameMs > 3000) permille = 1000;
            char rep[24];
            snprintf(rep, sizeof(rep), "REPORT:%u", permille);
            ctrlSock_.sendTo(teacherIP_, ctrlPort_, rep, (int)strlen(rep));
        }

        int n = videoSock_.recvFrom(buf, sizeof(buf));
        if (n <= 0) continue;
        packets_++;

        // cursor packet (same port as video): detect and divert here, keeps it out of reassembly (the reassembler
        // ignores non-video magics anyway, but diverting early avoids counting it in lost/frame sequences)
        if (n >= (int)sizeof(CursorPacket)) {
            CursorPacket cp;
            memcpy(&cp, buf, sizeof(cp));
            if (cp.magic == kCursorMagic) {
                // source resolution travels with the cursor stream (unchanged except on a quality switch),
                // letting the client show a true 1:1 even when the teacher downscales the stream
                if (cp.srcW > 0 && cp.srcH > 0) { srcW_.store(cp.srcW); srcH_.store(cp.srcH); }
                if (cp.nx >= 0.0f && cp.nx <= 1.0f && cp.ny >= 0.0f && cp.ny <= 1.0f) {
                    cursorNX_.store(cp.nx);
                    cursorNY_.store(cp.ny);
                    cursorVisible_.store(cp.visible ? 1 : 0);
                    haveCursor_.store(1);
                }
                continue;
            }
        }

        const std::vector<uint8_t>* frame = reassembler.push((const uint8_t*)buf, n, now);
        reassembler.cleanup(now, 150);
        // sync FEC recovery count (reassembly layer is private to the receive thread, read directly, accumulate the delta into atomic stats)
        uint64_t rec = reassembler.recoveredFrames();
        if (rec > lastRecovered) { recoveredFrames_ += rec - lastRecovered; lastRecovered = rec; }
        // sync abandoned-frame count too: these are the frames a lost fragment killed (hidden by the
        // completed-frame-gap loss counter), the true packet-loss indicator
        uint64_t abn = reassembler.abandonedFrames();
        if (abn > lastAbandoned) { abandonedFrames_ += abn - lastAbandoned; lastAbandoned = abn; }
        if (!frame) continue;

        frames_++;
        lastFrameMs = now;
        lastFrameArriveMs_.store((uint64_t)now);   // for the main thread's stream-outage detection (>5s no frame = connection lost)
        // loss detection: accumulate lost frames by the gap between "assembly-complete" frame numbers.
        // Only use completed frame numbers, not any first-packet frame number: on out-of-order UDP arrival
        // (receiving a later frame's first packet first) this avoids falsely reporting loss -> a KEYFRAME request
        // storm -> the teacher forced to resend the whole IDR every 1s -> whole class stutters
        uint16_t cid = reassembler.lastFrameId();
        if (haveLastFrame_ && cid != lastFrameId_) {
            uint16_t gap = (uint16_t)(cid - lastFrameId_ - 1);
            // gap cap prevents false reports from old frames / connection resets
            if (gap > 0 && gap <= 512) lostFrames_ += gap;
        }
        lastFrameId_ = cid;
        haveLastFrame_ = true;

        ReceivedFrame rf;
        rf.data = frame;
        // frame metadata is saved by the reassembly layer from the packet header (reassembled data is pure encoded stream, no header)
        rf.captureMs = reassembler.lastPtsMs();
        rf.keyframe  = reassembler.lastKeyframe();
        std::lock_guard<std::mutex> lk(cbMtx_);
        if (cb_) cb_(rf);
    }
    } catch (const std::exception& e) {
        fprintf(stderr, "[receiver] exception, receive thread stopping: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "[receiver] unknown exception, receive thread stopping\n");
    }
}