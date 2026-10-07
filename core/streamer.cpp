// core/streamer.cpp -- teacher streaming engine implementation
#include "core/streamer.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static uint64_t nowMs() { return (uint64_t)GetTickCount64(); }

// parse the first decimal octet to decide whether the target is a multicast group (224.0.0.0/4)
static bool isMulticastGroup(const std::string& ip) {
    int a = 0, i = 0;
    while (i < (int)ip.size() && ip[i] >= '0' && ip[i] <= '9') {
        a = a * 10 + (ip[i] - '0');
        if (a > 255) break;
        ++i;
    }
    return a >= 224 && a <= 239;
}

// adaptive bitrate AIMD params: loss >= 3% (30 permille) -> -15%; loss <= 0.5% (5 permille) sustained -> +10%
// floor guards against unusably-low quality; ceiling = manual override or the preset default
// WHY SO GENTLE: every bitrate change rebuilds the encoder + forces an IDR keyframe. On a congested link
// the old scheme (-20% every 1s) rebuilt the encoder every second; each rebuild stalled the stream ~100-200ms
// (sws context creation) and the client re-entered picture -> the student screen strobed/flashed on every rate
// change. The flash is now fixed at the source: encoder rebuilds reuse the sws context (~5ms) AND the rate
// controller requires SUSTAINED congestion + long cooldowns so it doesn't oscillate around the threshold.
static constexpr int kAdaptiveFloorKbps = 400;
static constexpr uint32_t kLossHighPermille = 30;   // 3% loss: congested, back off
static constexpr uint32_t kLossLowPermille  = 5;    // 0.5% loss: healthy, probe up
static constexpr uint64_t kAdaptiveDecreaseCooldownMs = 3000;   // min interval between decreases
static constexpr uint64_t kAdaptiveIncreaseCooldownMs = 10000;  // probe up only after 10s stable since ANY rate change (anti-oscillation)
static constexpr uint32_t kMinHighLossStreak = 2;   // require >=2 consecutive 1s windows with >=3% loss before the first decrease (a single transient spike must not rebuild the encoder)
static constexpr uint64_t kLossReportExpireMs = 5000;          // drop stale reports after 5s

Streamer::Streamer() = default;
Streamer::~Streamer() { stop(); }

bool Streamer::start(const std::string& ip, uint16_t vp, uint16_t cp, const std::string& localIp) {
    if (running_.load()) return false;
    targetIP_ = ip; videoPort_ = vp; ctrlPort_ = cp;
    // resolve the multicast egress interface: explicit IP wins, otherwise auto-detect the machine's
    // outbound NIC. Only meaningful for multicast targets (a unicast send ignores IP_MULTICAST_IF).
    std::string egressIp = localIp;
    if (egressIp.empty() && isMulticastGroup(ip)) {
        egressIp = UDPSocket::detectLocalIPv4();
    }
    if (!videoSock_.createSocket()) return false;
    if (!videoSock_.setupSender(ip, vp, 1, egressIp)) {
        fprintf(stderr, "[streamer] 发送端配置失败(IP 无效?)\n");
        videoSock_.close();
        return false;
    }
    videoSock_.setSendBuffer(1024 * 1024);   // 1MB send buffer: keyframe bursts (140+ fragments) should not block the capture thread
    if (!ctrlSock_.createSocket()) return false;
    ctrlSock_.bindUnicast(cp);
    ctrlSock_.setRecvBuffer(256 * 1024);     // 256KB: absorb a 30-40 client join burst of HELLO/REPORT so none are dropped (dropped -> false offline)
    ctrlSock_.setRecvTimeout(30);       // 30ms polling: cursor ~30Hz smooth follow + fast stop response
    sendQueue_.resetForStart();         // idempotent restart: clear the last stop's closed flag and leftover packets
    running_.store(true);
    senderThread_  = std::thread(&Streamer::senderLoop, this);   // start the sender thread first so the first frame already has a consumer
    captureThread_ = std::thread(&Streamer::captureLoop, this);
    ctrlThread_    = std::thread(&Streamer::controlLoop, this);
    return true;
}

// idempotent stop: regardless of whether running_ was already set false by the capture thread (capture-init failure path),
// fully reclaim all threads. Otherwise on fail -> recover -> retry start(), the old threads are still joinable and
// reassigning std::thread would call std::terminate.
void Streamer::stop() {
    (void)running_.exchange(false);
    sendQueue_.closeNotify();                        // sender exits once it can't receive new packets and is drained
    if (captureThread_.joinable()) captureThread_.join();   // stop capture first (no more push)
    capture_.stop();
    if (ctrlThread_.joinable()) ctrlThread_.join();
    if (senderThread_.joinable()) senderThread_.join();     // drain remaining packets then exit
    enc_.shutdown();
    videoSock_.close();
    ctrlSock_.close();
    activeQuality_ = -1;   // re-init on next start
    lastKeyMs_ = 0;        // reset keyframe rate-limit timer so the first request after restart takes effect immediately
    lastAdaptiveDecMs_ = 0; lastAdaptiveIncMs_ = 0;
    lastAdaptiveChangeMs_ = 0; highLossStreak_ = 0; lastHighReportMs_ = 0;   // adaptive cooldowns + streak: deterministic behavior after a restart
}

// send thread: move packets from the bounded queue in batches and send them over UDP. exits when the queue is empty and closed.
// only this sender thread does blocking sendTo; capture/encode threads never wait on the network.
void Streamer::senderLoop() {
    for (;;) {
        auto batch = sendQueue_.popBatch();
        if (batch.empty() && sendQueue_.closed) break;   // drained and closed -> exit
        int n = (int)batch.size();
        for (int i = 0; i < n; ++i) {
            int k = videoSock_.sendTo(targetIP_.c_str(), videoPort_,
                                      batch[i].data(), (int)batch[i].size());
            if (k > 0) { packetsSent_++; bytesSent_ += (uint64_t)k; }
            // gentle throttle on large-frame bursts (tens to hundreds of fragments): yield CPU every 16
            // fragments, smoothing the network burst to avoid instantly filling switch/client buffers and
            // dropping packets -> effectively reduces stutter for clients in multicast scenarios
            if (n > 32 && (i & 15) == 15) Sleep(0);
            pool_.release(std::move(batch[i]));   // recycle the fragment buffer for the capture thread
        }
    }
}

void Streamer::captureLoop() {
    if (!capture_.start(0, [this](const CapturedFrame& f) { onFrame(f); })) {
        printf("[streamer] capture init failed, stop streaming\n");
        running_.store(false);   // clear flag: allow GUI retry, let ctrlThread exit
        return;
    }
    while (running_.load()) {
        if (!capture_.pumpFrame())
            Sleep(2);   // slight backoff on no new frame: reduce CPU contention, let the GUI thread render window drags
    }
    capture_.stop();
}

void Streamer::controlLoop() {
    char buf[64];
    uint64_t lastPrune = 0;
    uint64_t lastCursor = 0;   // cursor send throttle: at most 1 per 30ms (~30Hz, matches the 30ms recv polling)
    while (running_.load()) {
        std::string srcIP;
        int n = ctrlSock_.recvFrom(buf, sizeof(buf), &srcIP, nullptr);
        if (n > 0 && !srcIP.empty()) {
            if (n >= 5 && strncmp(buf, "HELLO", 5) == 0) {
                // client heartbeat: refresh this client's last-online time
                std::lock_guard<std::mutex> lk(peerMtx_);
                peers_[srcIP] = nowMs();
            } else if (n >= 8 && strncmp(buf, "KEYFRAME", 8) == 0) {
                keyframeReq_.store(true);
                // Tell the capture layer to produce a frame NOW. Otherwise a client that joins while the
                // desktop is idle waits for the capture's 2s keep-alive (or forever, if the capture path
                // never yields a frame at all) before it can be given a decodable picture.
                capture_.requestFrameSoon();
            } else if (n >= 8 && strncmp(buf, "REPORT:", 7) == 0) {
                // adaptive bitrate: client reports its loss rate over the last 2s window as permille (0..1000)
                // buf is NOT null-terminated (recvFrom writes exactly n bytes), so parse bounded by n (max 4 digits)
                uint32_t permille = 0;
                int i = 7;
                while (i < n && i < 11 && buf[i] >= '0' && buf[i] <= '9') {
                    permille = permille * 10 + (uint32_t)(buf[i] - '0');
                    ++i;
                }
                if (i > 7 && permille <= 1000) {
                    std::lock_guard<std::mutex> lk(peerMtx_);
                    lossReports_[srcIP] = { permille, nowMs() };
                }
            }
        }
        // every 1s: prune stale clients/loss-reports and run one adaptive-bitrate control step
        uint64_t now = nowMs();
        if (now - lastPrune >= 1000) {
            lastPrune = now;
            {
                std::lock_guard<std::mutex> lk(peerMtx_);
                for (auto it = peers_.begin(); it != peers_.end();) {
                    if (now - it->second >= 10000) it = peers_.erase(it);
                    else ++it;
                }
                for (auto it = lossReports_.begin(); it != lossReports_.end();) {
                    if (now - it->second.second >= kLossReportExpireMs) it = lossReports_.erase(it);
                    else ++it;
                }
            }
            adaptiveStep(now);
        }
        // cursor position is sent over the video channel (30ms throttle): the client overlays the teacher's pointer smoothly.
        // normalized to the captured-frame size; the pointer outside the primary monitor (incl. multi-screen negative coords) is invisible.
        // fix: cp.visible is also set to 0 on out-of-bounds -- the old implementation only cleared the coords while visible stayed 1,
        // multi-screen negative coords would pin the pointer to the client's top-left corner.
        if (now - lastCursor >= 30 && statSrcW_.load() > 0 && statSrcH_.load() > 0) {
            lastCursor = now;
            POINT pt{};
            int sw = statSrcW_.load(), sh = statSrcH_.load();
            bool visible = GetCursorPos(&pt) && pt.x >= 0 && pt.y >= 0 && pt.x < sw && pt.y < sh;
            CursorPacket cp{};
            cp.magic = kCursorMagic;
            cp.visible = visible ? 1 : 0;
            cp.srcW = (uint16_t)sw;   // source resolution: lets the client show a true 1:1 even when downscaled
            cp.srcH = (uint16_t)sh;
            if (visible) {
                cp.nx = (float)pt.x / (float)sw;
                cp.ny = (float)pt.y / (float)sh;
            }
            videoSock_.sendTo(targetIP_.c_str(), videoPort_, &cp, (int)sizeof(cp));
        }
    }
}

void Streamer::setQuality(Quality q)          { quality_.store((int)q); }
void Streamer::setBitrate(int kbps) {
    bitrateKbps_.store(kbps > 0 ? kbps : 0);
    // a manual override while adaptive is on changes the ceiling: clamp the adaptive target immediately
    // so the encoder never keeps running above the newly-set limit until the next adaptive step catches up
    if (adaptive_.load()) {
        int ceiling = kbps > 0 ? kbps : presetKbps((Quality)quality_.load());
        int cur = adaptiveCur_.load();
        if (cur > ceiling) adaptiveCur_.store(ceiling);
    }
}

void Streamer::setAdaptiveBitrate(bool on) {
    adaptive_.store(on);
    if (on) adaptiveCur_.store(0);   // start from the ceiling; the controller will find the right level
    // turning off: the effective bitrate falls back to the manual override / preset default automatically
}

void Streamer::setDifferentialEnabled(bool on){ differential_.store(on); }

void Streamer::setPreviewCallback(PreviewCallback cb) {
    std::lock_guard<std::mutex> lk(cbMtx_);
    previewCb_ = std::move(cb);
}

void Streamer::setSelfExcludeRect(const RECT& rc, bool enabled) {
    capture_.setExcludeScreenRect(rc, enabled);
}

StreamStats Streamer::snapshot() const {
    StreamStats s;
    s.framesCaptured = framesCaptured_.load();
    s.framesSent     = framesSent_.load();
    s.framesSkipped  = framesSkipped_.load();
    s.packetsSent    = packetsSent_.load();
    s.bytesSent      = bytesSent_.load();
    s.sendDropFrames = sendDropFrames_.load();
    s.encWidth  = encW_.load();   s.encHeight = encH_.load();
    s.srcWidth  = statSrcW_.load(); s.srcHeight = statSrcH_.load();
    s.targetKbps = effectiveKbps();
    s.gdiMode   = capture_.gdiMode();
    return s;
}

// preset default bitrates (mirrors the table in initEncoder)
// Tuned for a gigabit LAN: one multicast stream serves the whole class, so 20 Mbps is ~2% of a
// 1000BASE-T link and a wired student is never the bottleneck. The old numbers (1080p = 6 Mbps,
// original = 8 Mbps) were sized for weak WiFi and left most of a gigabit link idle, which is what
// made fast-moving content (scrolling, video, slide transitions) look soft. Static desktops still
// cost far less than the cap because differential mode skips unchanged frames entirely.
int Streamer::presetKbps(Quality q) const {
    // These are VBV PEAK caps, not targets: the encoder runs at CRF 20 and only touches the cap on
    // complex motion. On a wired gigabit classroom link even the ORIGINAL cap is ~2.6% of the wire, so
    // the caps are set generously enough that scrolling text / video playback never quality-pulses;
    // static slides still transmit at a fraction of these numbers. WiFi classrooms are covered by the
    // adaptive-bitrate controller, which cuts the live target on measured loss regardless of the cap.
    switch (q) {
    case Quality::HD1080P: return 16000;
    case Quality::SD720P:  return 8000;
    case Quality::FLUENT:  return 4000;
    default:               return 26000;   // ORIGINAL: source resolution, highest fidelity
    }
}

int Streamer::ceilingKbps() const {
    int m = bitrateKbps_.load();
    return m > 0 ? m : presetKbps((Quality)quality_.load());
}

// bitrate the encoder should use: adaptive target if set, else manual override, else preset default
int Streamer::effectiveKbps() const {
    if (adaptive_.load()) {
        int a = adaptiveCur_.load();
        if (a > 0) return a;
    }
    return ceilingKbps();
}

int Streamer::currentBitrateKbps() const { return effectiveKbps(); }

// AIMD control: the P90 (90th-percentile) loss across all clients reporting within the expire window
// drives the bitrate -- NOT the single worst client. A multicast stream is one shared signal, so it
// cannot be tuned to a single outlier: with 30-40 viewers, one weak/stalled student would otherwise
// drag the whole class down to the 400kbps floor and hold it there (a stalled client even reports
// 1000 permille). Taking the P90 ignores the worst ~10% as outliers while still tracking the
// majority. For tiny client counts (<=2) P90 degenerates to the max, which is the safe behavior.
//   p90 >= 3% sustained (2 consecutive 1s windows) -> -15% (3s cooldown)
//   p90 <= 0.5% AND stable 10s since ANY rate change -> +10% (slow probe-up)
// no fresh reports (no clients) -> hold, never spin up on stale data
// Anti-oscillation: a rate decrease must be earned by SUSTAINED congestion (streak), and an increase
// requires 10s of stability since ANY change -- so the controller can't ping-pong around the loss threshold,
// each ping-pong being an encoder rebuild + IDR burst that flashes the student screen.
void Streamer::adaptiveStep(uint64_t now) {
    if (!adaptive_.load()) return;
    uint32_t p90 = 0;
    uint64_t freshHighTime = 0;   // newest timestamp among clients reporting >= the high-loss threshold
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(peerMtx_);
        std::vector<uint32_t> vals;
        vals.reserve(lossReports_.size());
        for (const auto& kv : lossReports_) {
            any = true;
            vals.push_back(kv.second.first);
            if (kv.second.first >= kLossHighPermille && kv.second.second > freshHighTime)
                freshHighTime = kv.second.second;
        }
        if (!vals.empty()) {
            std::sort(vals.begin(), vals.end());
            // 90th percentile (0-based): the value ~90% of clients are at or below, ignoring the worst
            // ~10% as outliers. ceil(N*0.9) - 1 gives that index; N=40 -> index 35 (ignore top 4).
            size_t idx = (vals.size() * 9 + 9) / 10;   // ceil(N*0.9)
            if (idx > 0) --idx;
            if (idx >= vals.size()) idx = vals.size() - 1;
            p90 = vals[idx];
        }
    }
    if (!any) { highLossStreak_ = 0; lastHighReportMs_ = 0; return; }   // no client feedback: hold the current level, reset streak

    int cur = adaptiveCur_.load();
    int ceiling = ceilingKbps();
    int next = cur > 0 ? cur : ceiling;   // first step starts from the ceiling
    if (p90 >= kLossHighPermille) {
        // sustained congestion gate: a single transient spike (e.g. a bursty IDR over a weak link) must NOT
        // rebuild the encoder -- each rebuild is a visible flash. Reports arrive every 2s while this control
        // step runs every 1s, so the streak only advances when a NEW report (fresh timestamp) is high -- i.e.
        // >=2 consecutive 2s report windows must show >=3% loss before the first decrease.
        if (freshHighTime != lastHighReportMs_) {
            lastHighReportMs_ = freshHighTime;
            ++highLossStreak_;
        }
        if (highLossStreak_ >= kMinHighLossStreak &&
            now - lastAdaptiveDecMs_ >= kAdaptiveDecreaseCooldownMs) {
            next = next * 85 / 100;
            if (next < kAdaptiveFloorKbps) next = kAdaptiveFloorKbps;
            lastAdaptiveDecMs_ = now;
            lastAdaptiveChangeMs_ = now;   // any change restarts the anti-oscillation stability hold
        }
    } else {
        highLossStreak_ = 0;
        lastHighReportMs_ = 0;
        if (p90 <= kLossLowPermille) {
            // probe up only after a long stable period since ANY last rate change (decrease or increase):
            // prevents the classic decrease->increase->decrease oscillation around the equilibrium point
            // (lastAdaptiveIncMs_ is subsumed by lastAdaptiveChangeMs_, kept for clarity of intent)
            if (now - lastAdaptiveChangeMs_ >= kAdaptiveIncreaseCooldownMs) {
                next = next * 11 / 10;
                if (next > ceiling) next = ceiling;
                lastAdaptiveIncMs_ = now;
                lastAdaptiveChangeMs_ = now;
            }
        }
    }
    // loss between 0.5% and 3%: hold (hysteresis band)
    if (next != cur) adaptiveCur_.store(next);
}

std::vector<std::string> Streamer::clientIps() const {
    std::vector<std::string> out;
    uint64_t now = nowMs();
    std::lock_guard<std::mutex> lk(peerMtx_);
    for (const auto& kv : peers_)
        if (now - kv.second < 10000) out.push_back(kv.first);
    return out;
}

// compute the target size and bitrate from the quality preset, and create the encoder
bool Streamer::initEncoder(int srcW, int srcH) {
    Quality q = (Quality)quality_.load();
    int dstW = srcW, dstH = srcH;
    switch (q) {
    case Quality::HD1080P: dstW = 1920; dstH = 1080; break;
    case Quality::SD720P:  dstW = 1280; dstH = 720;  break;
    case Quality::FLUENT:  dstW = 960;  dstH = 540;  break;
    default:               break;   // ORIGINAL: source resolution
    }
    int kbps = effectiveKbps();   // preset default, or manual override, or the current adaptive target
    // keep the aspect ratio, and make the size even (H.264 requires it)
    double s = std::min(1.0, std::min((double)dstW / srcW, (double)dstH / srcH));
    dstW = (int)(srcW * s / 2) * 2; if (dstW < 2) dstW = 2;
    dstH = (int)(srcH * s / 2) * 2; if (dstH < 2) dstH = 2;

if (!enc_.init(srcW, srcH, dstW, dstH, 60, kbps,
                   [this](const uint8_t* data, int size, uint64_t captureMs, bool key) {
                       framesSent_++;
                       auto pkts = packetizeFrame(data, size, ++frameId_, (uint32_t)captureMs, key, &pool_);
                       // decouple sending: capture/encode thread only packetizes + enqueues (non-blocking).
                       // on a full queue (network congestion) the whole frame is dropped, the client detects
                       // the loss and requests a keyframe rebuild (within design)
                       int dropped = 0;
                       for (auto& pkt : pkts) {
                           if (!sendQueue_.push(std::move(pkt))) {
                               dropped++;
                               pool_.release(std::move(pkt));   // rejected (queue full): recycle the buffer
                           }
                       }
                       if (dropped > 0) sendDropFrames_++;
                   })) {
        fprintf(stderr, "[streamer] 编码器初始化失败\n");
        return false;
    }
    activeQuality_ = (int)q;
    lastInitKbps_ = kbps;
    encW_.store(dstW); encH_.store(dstH);
    statSrcW_.store(srcW); statSrcH_.store(srcH);
    enc_.forceKeyframe();
    fprintf(stderr, "[streamer] 清晰度档位 %d: 编码 %dx%d, %dkbps%s\n", (int)q, dstW, dstH, kbps,
            adaptive_.load() ? " (自适应码率)" : "");
    return true;
}

void Streamer::onFrame(const CapturedFrame& f) {
    framesCaptured_++;
    if (f.width <= 0 || f.height <= 0) return;

    // preview callback (the UI side copies the thumbnail and changed regions).
    // check inside the lock: the callback is replaced under cbMtx_, so checking previewCb_
    // before taking the lock could still invoke an already-cleared std::function (crash).
    {
        std::lock_guard<std::mutex> lk(cbMtx_);
        if (previewCb_)
            previewCb_(f.data, f.width, f.height, f.stride,
                       f.dirtyRects.data(), (int)f.dirtyRects.size());
    }

    // first frame / source resolution change / quality switch / adaptive bitrate change -> (re)build the encoder.
    // a bitrate change rebuild forces a fresh IDR keyframe, so clients pick up the new rate with a clean join
    int wantKbps = effectiveKbps();
    bool needReinit = (activeQuality_ < 0) ||
                      (f.width != curSrcW_ || f.height != curSrcH_) ||
                      (activeQuality_ != quality_.load()) ||
                      (lastInitKbps_ != wantKbps);
    if (needReinit) {
        if (activeQuality_ >= 0) enc_.shutdown();
        curSrcW_ = f.width; curSrcH_ = f.height;
        if (!initEncoder(f.width, f.height)) {
            // clear the flag and return: the capture loop exits, and stop() reclaims every thread.
            // (the old code joined ctrlThread_ from the capture thread here, which raced a concurrent
            // stop() joining the same thread -> double-join / std::terminate)
            running_.store(false);
            return;
        }
        firstFrame_ = true;
    }

    // differential screen: skip encoding when almost nothing changed and a frame went out recently.
    // The threshold was 2%, which on a gigabit LAN was far too coarse: partial updates (a progress
    // bar, a chart animating, a small window repainting) fell under it and the students kept staring
    // at a stale picture until the 2s keepalive. 0.5% still removes the whole point of differential
    // mode -- not re-encoding a genuinely idle desktop -- while passing small live changes through.
    // keepalive rule: if more than 2s since the last send, force encode one frame so new clients can get a keyframe to join
    uint64_t now = nowMs();
    double dirtyRatio = f.dirtyArea / (double)((uint64_t)f.width * f.height);
    // A PENDING KEYFRAME REQUEST MUST BYPASS THE SKIP. The request means "somebody is waiting for a
    // picture they can actually decode" -- a client just joined, or one lost its stream. Skipping the
    // frame left that client staring at a black window until the 2s keepalive happened to fire, which
    // is exactly the reported "the picture takes ages to appear after connecting" (measured 2.4s).
    // It got worse when the GDI capture path learned to report "nothing changed" for an idle desktop:
    // before that, GDI always claimed a full-screen change and no frame was ever skipped.
    bool keyPending = keyframeReq_.load();
    if (!firstFrame_ && differential_.load() && !keyPending && dirtyRatio < 0.005 && now - lastSentMs_ < 2000) {
        framesSkipped_++;
        return;
    }
    firstFrame_ = false;

    // keyframe strategy (two sources, shared rate-limit timer, at most 1 IDR per 250ms):
    // 1) client request (merged per client to 1 per 300ms; 250ms here bounds the burst when a whole
    //    class joins at once to ~4 IDR/s = ~6MB -- negligible on the gigabit link the deployment targets)
    // 2) periodic self-heal: differential skips slow the encoder's GOP counter, a natural keyframe may come
    //    only every 120s, so force one if >2s since the last IDR, ensuring a new/lossy client can rebuild within <=2s
    // NOTE: keyframeReq_ is only consumed when an IDR is actually sent (during the rate-limit window the request
    // is retained and resent when it expires, so the request is not lost)
    // The limit used to be 1s: a client that joined just after an IDR went out then waited up to a full
    // second ON TOP of the network/capture path before its picture appeared -- a visible chunk of the
    // reported "connecting takes ages". 250ms keeps the burst bound while cutting that wait 4x.
    bool wantKey = keyframeReq_.load() || (now - lastKeyMs_ >= 2000);
    if (wantKey && now - lastKeyMs_ >= 250) {
        keyframeReq_.store(false);
        lastKeyMs_ = now;
        enc_.forceKeyframe();
    }
    enc_.encode(f.data, f.stride, f.ptsMs);
    lastSentMs_ = now;
}
