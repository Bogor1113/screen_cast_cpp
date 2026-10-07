// ui/client_window.cpp -- client GUI main window (Qt port of client_gui.cpp Win32 version)
#include "ui/client_window.h"
#include "ui/video_widget.h"
#include "version.h"
#include <QTimer>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QWindow>
#include <QScreen>
#include <QGuiApplication>
#include <algorithm>
#include <chrono>

constexpr uint32_t kLatUnavailable = 0xFFFFFFFF;   // clocks not synced (cross machine), latency shows "--"

// ---------------- queues (same semantics as the Win32 version) ----------------
// Only this many buffers are kept in the pool: the widget cache, the in-flight frame and one spare
// cover the whole rotation, so a steady stream never touches the heap again.
static constexpr size_t kMaxSpare = 2;

void FrameQueue::push(Frame f) {
    std::lock_guard<std::mutex> lk(mtx);
    if (frames.size() > 3) frames.erase(frames.begin());   // drop backlog, keep realtime
    frames.push_back(std::move(f));
}
bool FrameQueue::pop(std::vector<uint8_t>& out, int& fw, int& fh, uint64_t& ms) {
    std::lock_guard<std::mutex> lk(mtx);
    if (frames.empty()) return false;
    Frame& f = frames.front();
    out.swap(f.data);   // out takes this frame's pixels; f.data takes back out's old buffer
    fw = f.w; fh = f.h; ms = f.pts;
    if (!f.data.empty() && spare.size() < kMaxSpare) spare.push_back(std::move(f.data));
    frames.erase(frames.begin());
    return true;
}
void FrameQueue::recycle(std::vector<uint8_t>&& buf) {
    if (buf.capacity() == 0) return;
    // deliberately NOT cleared: a pooled buffer must stay exactly as large as a decoded frame, because
    // the decoder sizes its sws destination with `if (size() < need) resize(need)` and vector::resize
    // value-initialises the grown range -- clearing the buffer would make it memSET 7.5MB per frame,
    // which is exactly the hidden cost this pool exists to remove.
    std::lock_guard<std::mutex> lk(mtx);
    if (spare.size() < kMaxSpare) spare.push_back(std::move(buf));
}
void FrameQueue::takeSpare(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(mtx);
    if (spare.empty() || !out.empty()) return;
    std::swap(out, spare.back());      // out is empty here, so no buffer is lost
    spare.pop_back();
}
bool FrameQueue::peekMs(uint64_t* ms) {
    std::lock_guard<std::mutex> lk(mtx);
    if (frames.empty()) return false;
    *ms = frames.front().pts;
    return true;
}
int FrameQueue::sizeSafe() {
    std::lock_guard<std::mutex> lk(mtx);
    return (int)frames.size();
}
void EncodedQueue::push(Item it) {
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (items.size() > 8) items.erase(items.begin());   // cap 8 frames, drop old ones to stay realtime
        items.push_back(std::move(it));
    }
    cv.notify_one();
}
bool EncodedQueue::pop(Item& out) {
    std::lock_guard<std::mutex> lk(mtx);
    if (items.empty()) return false;
    out = std::move(items.front()); items.erase(items.begin());
    return true;
}
bool EncodedQueue::waitPop(Item& out, const std::atomic<bool>& running) {
    std::unique_lock<std::mutex> lk(mtx);
    // the 100ms ceiling is a safety net (a missed notify can never wedge the decode thread) and also
    // bounds how long stop() waits for the thread to notice `running == false`.
    cv.wait_for(lk, std::chrono::milliseconds(100),
                [&] { return !items.empty() || !running.load(); });
    if (items.empty()) return false;
    out = std::move(items.front()); items.erase(items.begin());
    return true;
}

static uint64_t steadyMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------- window ----------------
ClientWindow::ClientWindow(const QString& teacherIP, const QString& group, QWidget* parent)
    : QMainWindow(parent), teacherIP_(teacherIP), group_(group),
      multicast_(!group.isEmpty()) {
    setWindowTitle(QString::fromWCharArray(SCREENCAST_VERSION_WS));
    resize(960, 600);
    setMinimumSize(480, 320);

    // Derive the presentation interval from the panel's real refresh rate. Student PCs in the
    // classroom are 1366x768 @ 60Hz: 1000/60 = 16ms, which keeps the queue just ahead of the panel
    // (never starving it) without presenting frames the panel cannot show. On a 50Hz or 120/144Hz
    // machine the interval follows automatically instead of being hard-coded.
    {
        int hz = 60;
        if (const QScreen* sc = QGuiApplication::primaryScreen()) {
            const int r = sc->refreshRate();
            if (r >= 24 && r <= 240) hz = r;
        }
        presentIntervalMs_ = qMax(1000 / hz, 4);
    }

    video_ = new VideoWidget(this);
    setCentralWidget(video_);

    // timers: frame pacing at ~30fps, idle repaint at 100Hz (fast tick, cheap checks),
    // stats + auto-reconnect edge at 1s
    frameTimer_ = new QTimer(this);
    frameTimer_->setInterval(10);
    connect(frameTimer_, &QTimer::timeout, this, &ClientWindow::onFrameTick);
    frameTimer_->start();

    idleTimer_ = new QTimer(this);
    // 25ms is deliberate: the fastest thing on this timer is the cursor-only repaint (20Hz) and the
    // "waiting for teacher" placeholder (2.5Hz), so a 10ms tick just woke the GUI thread 100x/s to do
    // nothing. 25ms still leaves 2x headroom over the repaint rate.
    idleTimer_->setInterval(25);
    connect(idleTimer_, &QTimer::timeout, this, &ClientWindow::onIdleTick);
    idleTimer_->start();

    statsTimer_ = new QTimer(this);
    statsTimer_->setInterval(1000);
    connect(statsTimer_, &QTimer::timeout, this, &ClientWindow::onStatsTick);
    statsTimer_->start();
}

ClientWindow::~ClientWindow() {
    stopAll();
}

// receive + decode threads (identical pipeline to the Win32 client)
bool ClientWindow::start() {
    if (started_) return true;   // idempotent: start() after a successful start must not spawn a 2nd thread
    // assigning a new std::thread over a still-joinable one calls std::terminate (instant process
    // death, no dump). Clear any previous (stopped but unjoined) thread first.
    if (decodeThread_.joinable()) decodeThread_.join();

    // receive thread: recv -> reassemble(FEC) -> copy the encoded frame into the queue
    if (!rx_.start(teacherIP_.toStdString(), group_.toStdString(), 5004, 5005,
                   [this](const ReceivedFrame& f) {
                       try {
                           if (!f.data || f.data->empty() || f.data->size() > 8 * 1024 * 1024) return;
                           EncodedQueue::Item it;
                           it.data = *f.data;          // copy: reassembly buffer valid only inside the callback
                           it.captureMs = f.captureMs;
                           encQueue_.push(std::move(it));
                       } catch (...) {}
                   })) {
        return false;
    }
    rx_.setDecodePrimed(&decodePrimed_);

    // (re)arm the decode-thread flag: stopAll() clears it, so a stop -> start sequence must set it
    // again, otherwise the decode thread exits immediately and no frame is ever decoded.
    running_.store(true);

    // dedicated decode thread: pop encoded frame -> H.264 decode -> stats/latency/enqueue BGRA
    decodeThread_ = std::thread([this] {
        EncodedQueue::Item it;
        // The pixel buffer rotates decoder -> queue -> widget -> pool -> decoder with no allocation and
        // no full-frame copy (see FrameQueue). `pix` is empty at the top of every iteration because it
        // was moved into the queue.
        std::vector<uint8_t> pix;
        while (running_.load()) {
            try {
                if (decodeResetReq_.exchange(false)) {
                    // the decoder drifted (repeated send_packet failures, or the GUI saw the stream come
                    // back): rebuild it on the thread that owns it and re-request keyframes so the next
                    // IDR re-syncs from scratch instead of showing a frozen/blocky picture forever.
                    decoder_.close();
                    decodePrimed_.store(false);
                }
                if (!encQueue_.waitPop(it, running_)) continue;   // blocks (no polling) until a frame lands
                // Refill the scratch from the pool, then let the decoder convert straight into it.
                // Taken together with takeBgra() below this keeps 2 buffers rotating forever: no
                // allocation, no zero-fill, no memcpy of the picture anywhere in the pipeline.
                if (pix.empty()) queue_.takeSpare(pix);
                decoder_.prepareScratch(pix);
                if (decoder_.decode(it.data.data(), (int)it.data.size(), it.captureMs)) {
                    int w = 0, h = 0;
                    if (!decoder_.takeBgra(pix, w, h)) continue;        // swap, not copy
                    const uint8_t* d = pix.data();
                    uint64_t pms = decoder_.ptsMs();
                    if (!decodePrimed_.load()) decodePrimed_.store(true);   // first frame decoded: stop keyframe requests
                    if (decodedOk_.load() % 10 == 0) {
                        // dark-pixel hint: sample every 48px, flag if >60% nearly black
                        int dark = 0, cnt = 0;
                        for (int y = 0; y < h; y += 48)
                            for (int x = 0; x < w; x += 48) {
                                const uint8_t* q = d + ((size_t)y * w + (size_t)x) * 4;
                                if (q[0] + q[1] + q[2] < 120) dark++;
                                cnt++;
                            }
                        latestDarkPct_.store(cnt ? (dark * 100) / cnt : -1);
                    }
                    FrameQueue::Frame f;
                    f.data = std::move(pix);
                    f.w = w; f.h = h; f.pts = pms;
                    queue_.push(std::move(f));
                    ++decodedOk_;
                    uint32_t now = (uint32_t)GetTickCount64();
                    if (it.captureMs && now > (uint32_t)it.captureMs) {
                        uint32_t dl = now - (uint32_t)it.captureMs;
                        latestLatencyMs_.store(dl <= 500 ? dl : kLatUnavailable);
                    }
                } else if (decoder_.failStreak() > 120) {
                    // ~2s of consecutive rejected packets: the context is desynced past recovery,
                    // ask for a rebuild on the next loop pass (done under the decoder's own lock)
                    decodeResetReq_.store(true);
                }
            } catch (...) {}
        }
    });

    started_ = true;
    return true;
}

void ClientWindow::stopAll() {
    if (!running_.exchange(false)) {
        // already stopped: still make sure a joinable thread never survives (std::thread's destructor
        // calls std::terminate when it is still joinable)
        encQueue_.wakeAll();
        if (decodeThread_.joinable()) decodeThread_.join();
        return;
    }
    rx_.stop();
    encQueue_.wakeAll();   // unblock the decode thread immediately instead of waiting for its timeout
    if (decodeThread_.joinable()) decodeThread_.join();
}

void ClientWindow::closeEvent(QCloseEvent* ev) {
    stopAll();
    QMainWindow::closeEvent(ev);
}

void ClientWindow::keyPressEvent(QKeyEvent* ev) {
    // F11 toggle maximize, Esc restore (client starts maximized with a title bar)
    if (ev->key() == Qt::Key_F11) {
        if (isMaximized()) showNormal();
        else showMaximized();
        ev->accept();
        return;
    }
    if (ev->key() == Qt::Key_Escape) {
        if (isMaximized()) { showNormal(); ev->accept(); return; }
    }
    QMainWindow::keyPressEvent(ev);
}

// ---------------- frame pump ----------------
// While the window is minimized / hidden nobody can see the picture, so the per-frame GPU upload
// (~7.5MB texture map + memcpy at 50fps) is pure waste. The video widget keeps caching frames either
// way, so restoring the window repaints the latest one immediately.
bool ClientWindow::renderPaused() const {
    return isMinimized() || !isVisible();
}

void ClientWindow::onFrameTick() {
    if (!started_) return;
    video_->setRenderPaused(renderPaused());
    uint64_t now = steadyMs();
    int fw = 0, fh = 0;
    uint64_t ms = 0, frontMs = 0;
    if (queue_.peekMs(&frontMs)) {
        // Consume the head frame, paced so network burst-jitter gets smoothed out. The cap used to be
        // a hard 33ms (=> never more than 30fps on the student screen, no matter what the teacher
        // sent); it is now derived from the panel refresh rate (~16ms at 60Hz). The pacing is still
        // self-correcting: whenever the queue is empty the next arriving frame is shown immediately
        // (lastPresentMs_ = 0 below), so a 30fps source is unaffected.
        if (lastPresentMs_ == 0 || now >= lastPresentMs_ + (uint64_t)presentIntervalMs_) {
            if (queue_.pop(frameBuf_, fw, fh, ms)) {
                // sync the teacher pointer (receive thread writes, consumed here on the GUI thread)
                float cxn = 0.0f, cyn = 0.0f; bool cv = false;
                if (rx_.cursorState(cxn, cyn, cv)) video_->renderer()->setCursor(cxn, cyn, cv);
                // presentFrame swaps frameBuf_ with the widget's one-frame cache, so frameBuf_ comes
                // back holding the PREVIOUS cache -- hand that straight to the pool. Result: the whole
                // loop keeps running on the same 3-4 buffers, no allocation, no extra memcpy.
                video_->presentFrame(frameBuf_, fw, fh);
                queue_.recycle(std::move(frameBuf_));
                lastPresentMs_ = now;
            }
        }
    } else {
        // queue empty: show the next frame immediately on arrival (catch-up), keeping low latency
        lastPresentMs_ = 0;
    }
}

void ClientWindow::onIdleTick() {
    if (!started_) return;
    video_->setRenderPaused(renderPaused());
    if (renderPaused()) return;          // minimized: nothing to repaint

    // A resize OR a pan/zoom must be followed by exactly one present(), otherwise the renderer keeps
    // its old swap-chain size / view and the OS keeps showing the last picture until a new frame
    // happens to arrive (which on a static desktop in differential mode can be up to 2s). The widget
    // sets this flag from resizeEvent and from requestViewRepaint(); handling it BEFORE the queue check
    // keeps a resize/drag responsive even while frames are flowing.
    const bool repaintPending = video_->consumeResizeRepaint();
    if (repaintPending) video_->presentCached();

    uint64_t now = steadyMs();
    if (queue_.sizeSafe() > 0) return;   // frames flowing: the paced pump handles presentation

    // Static-desktop refresh: video frames are sparse but the cursor moves, so redraw the cached
    // frame at ~20Hz -- but only when something actually changed. Repainting unconditionally
    // re-uploaded the whole frame to the GPU 20x/s (8MB per upload at 1080p) for nothing.
    if (now - lastRepaintMs_ >= 50) {
        lastRepaintMs_ = now;
        bool cursorMoved = false;
        float cxn = 0.0f, cyn = 0.0f; bool cv = false;
        if (rx_.cursorState(cxn, cyn, cv)) {
            cursorMoved = (cxn != lastCursorX_ || cyn != lastCursorY_ || cv != lastCursorVis_);
            video_->renderer()->setCursor(cxn, cyn, cv);
            lastCursorX_ = cxn; lastCursorY_ = cyn; lastCursorVis_ = cv;
        }
        if (cursorMoved) video_->presentCached();
    }

    // waiting-for-teacher placeholder: calm static dark-blue screen, refreshed at ~2.5Hz
    // (or immediately after a resize, so it fills the new window instead of staying stretched)
    if (!decodePrimed_.load() && (repaintPending || now - lastNoSigMs_ > 400)) {
        lastNoSigMs_ = now;
        video_->presentNoSignal();
    }
}

void ClientWindow::onStatsTick() {
    if (!started_) return;
    uint64_t now = steadyMs();
    uint64_t decoded = decodedOk_.load();

    // sync the teacher's captured source resolution (arrives via cursor packets) for a true 1:1 view
    uint16_t sw = 0, sh = 0;
    rx_.sourceSize(sw, sh);
    if (sw > 0 && sh > 0) video_->renderer()->setSourceSize(sw, sh);

    double fps = 0.0;
    if (lastStatsMs_ && now > lastStatsMs_) {
        double dt = (double)(now - lastStatsMs_);
        fps = (double)(decoded - lastDecoded_) * 1000.0 / dt;
    }
    lastStatsMs_ = now; lastDecoded_ = decoded;

    uint64_t lost = rx_.lostFrames(), got = rx_.frames();
    double lossPct = (lost + got) ? (double)lost * 100.0 / (double)(lost + got) : 0.0;
    int q = queue_.sizeSafe();
    uint32_t lat = latestLatencyMs_.load();

    // stream-loss detection: received picture before but no new frame for >5s
    // (teacher exited/crashed/network down). Differential keep-alive sends a frame every 2s
    // while the teacher is alive, so 5s won't false-positive on a static desktop.
    uint64_t rxLastMs = rx_.lastFrameMs();
    bool disconnected = got > 0 && rxLastMs != 0 && (uint64_t)GetTickCount64() - rxLastMs > 5000;
    if (disconnected && !wasDisconnected_) {
        // ---- auto-reconnect (rising edge) ----
        wasDisconnected_ = true;
        decodePrimed_.store(false);   // re-request keyframes every 300ms
        rx_.requestReset();           // clear frame-seq tracking (teacher restarted frameId)
        // Rebuild the decoder on the DECODE thread instead of right here: decoder_.close() takes the
        // decoder mutex, so doing it on the GUI thread stalls the UI for the duration of whatever
        // decode is in flight. The flag is picked up on the next loop pass.
        decodeResetReq_.store(true);
    } else if (!disconnected && wasDisconnected_) {
        wasDisconnected_ = false;     // recovered: stream is flowing again
    }

    QString ip = teacherIP_;
    QString title;
    if (got == 0) {
        latestDarkPct_.store(-1);
        title = QStringLiteral("教师端 %1 | 未收到画面 - 等待教师推流 | 缩放 %2%")
                    .arg(ip).arg(video_->renderer()->effectiveZoom() * 100.0, 0, 'f', 0);
    } else if (disconnected) {
        title = QStringLiteral("教师端 %1 | 连接中断 - 自动重连中... | 最后 %2 FPS | 缩放 %3%")
                    .arg(ip)
                    .arg(fps, 0, 'f', 0).arg(video_->renderer()->effectiveZoom() * 100.0, 0, 'f', 0);
    } else if (lat == kLatUnavailable) {
        title = QStringLiteral("教师端 %1 | %2 FPS | 丢包 %3% | 延迟 -- ms | 队列 %4 | 缩放 %5%")
                    .arg(ip)
                    .arg(fps, 0, 'f', 0).arg(lossPct, 0, 'f', 1).arg(q)
                    .arg(video_->renderer()->effectiveZoom() * 100.0, 0, 'f', 0);
    } else {
        title = QStringLiteral("教师端 %1 | %2 FPS | 丢包 %3% | 延迟 %4 ms | 队列 %5 | 缩放 %6%")
                    .arg(ip)
                    .arg(fps, 0, 'f', 0).arg(lossPct, 0, 'f', 1).arg(lat).arg(q)
                    .arg(video_->renderer()->effectiveZoom() * 100.0, 0, 'f', 0);
    }
    // Surface the WARP (CPU-rasterizer) fallback in the title: on a classroom PC that lacks a usable
    // 3D driver the client still works, but it is worth knowing that rendering moved to the CPU.
    if (video_->renderer()->usingWarp() && !title.contains(QStringLiteral("CPU")))
        title += QStringLiteral(" | CPU渲染");
    // Received picture size / render surface size. This is the diagnostic that tells a "the picture is
    // soft" report apart from "the swap chain is not the size we asked for" at a glance.
    if (got > 0) {
        title += QStringLiteral(" | 画面 %1x%2 窗口 %3x%4")
                     .arg(video_->renderer()->videoW()).arg(video_->renderer()->videoH())
                     .arg(video_->renderer()->surfaceW()).arg(video_->renderer()->surfaceH());
        // Which decoder is actually running. On a low-spec classroom machine this is the first thing to
        // check when someone reports high CPU: "软解" there means D3D11VA was unavailable or was refused
        // by the driver, and the fix is a driver update (or accepting the CPU cost).
        title += decoder_.hardwareDecoding() ? QStringLiteral(" | 硬解")
                                             : QStringLiteral(" | 软解");
        // Device-loss recovery counter: stays at 0 on a healthy machine. A climbing number means the
        // graphics driver keeps resetting (TDR / sleep-resume / driver crash) and the machine needs
        // attention long before anything is visible on screen. Zero cost to display.
        title += QStringLiteral(" | 重建 %1").arg(video_->deviceRebuilds());
        // pan offset in window pixels: answers "my picture moved/wandered off" in one glance, and
        // lets a drag test assert the exact offset without relying on screen capture (which cannot
        // see this window once WDA_EXCLUDEFROMCAPTURE is on)
        title += QStringLiteral(" | 视区 %1,%2")
                     .arg(qRound(video_->renderer()->panX()))
                     .arg(qRound(video_->renderer()->panY()));
    }
    setWindowTitle(title);
}