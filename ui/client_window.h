#pragma once
// ui/client_window.h -- client GUI main window (Qt port of the Win32 client_gui.cpp):
// FFmpeg decode + D3D11 render embedded in a Qt window, with the same frame pump
// (10ms tick + panel-refresh-rate pacing, cached-frame repaint at 20Hz, calm no-signal
// placeholder), 1s stats title, auto-reconnect on >5s stream outage, and "dark screen" hint.
#include <QMainWindow>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>
#include "net/client_receiver.h"
#include "decode/ffmpeg_decoder.h"

class VideoWidget;
class QTimer;

// decoded BGRA frames, each carrying its own resolution (resolution can switch mid-stream).
//
// Buffer economics matter here: a 1728x1080 frame is ~7.5MB and the client displays 50-60 of them per
// second. The previous design copied the whole frame four times (decoder bgra_ -> copyBgra -> push ->
// presentFrame/lastBgra_) AND malloc/free'd 7.5MB once per frame. Traced as ~450MB/s of pointless
// memcpy plus constant allocator churn (visible as working-set oscillation). The queue now moves
// buffers instead of copying them and recycles the ones it has seen, so after the first couple of
// frames the pipeline runs on a small fixed set of buffers with no allocation and no extra copy.
struct FrameQueue {
    struct Frame { std::vector<uint8_t> data; int w = 0, h = 0; uint64_t pts = 0; };
    std::mutex mtx;
    std::vector<Frame> frames;
    std::vector<std::vector<uint8_t>> spare;   // recycled pixel buffers (capped, see push/recycle)

    void push(Frame f);
    // swaps the head frame's pixels into `out` (out's previous buffer is kept for the pool) and pops it
    bool pop(std::vector<uint8_t>& out, int& fw, int& fh, uint64_t& ms);
    bool peekMs(uint64_t* ms);
    int sizeSafe();
    // hand a spent pixel buffer back to the pool; the decode thread picks it up via takeSpare() so the
    // steady state allocates nothing. Buffers whose size no longer fits are dropped (kept at most 2).
    void recycle(std::vector<uint8_t>&& buf);
    // drop all queued (not yet presented) frames: used when reconnecting to a different teacher so
    // stale pictures from the previous stream are never shown
    void clear();
    // take a pooled buffer to decode into (may return an empty vector; the decoder sizes it)
    void takeSpare(std::vector<uint8_t>& out);
};

// encoded-frame queue: receive thread copies+enqueues, a dedicated decode thread decodes.
// The decode thread BLOCKS on a condition variable instead of polling a sleep(1ms) loop: the old
// design woke up ~1000 times per second doing nothing while the desktop was static, and still added
// up to 1ms of latency to every frame that did arrive.
struct EncodedQueue {
    struct Item { std::vector<uint8_t> data; uint64_t captureMs; };
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<Item> items;
    void push(Item it);
    bool pop(Item& out);    // wait until an item arrives, `running` goes false, or the safety timeout elapses
    bool waitPop(Item& out, const std::atomic<bool>& running);
    void wakeAll() { cv.notify_all(); }   // shutdown / restart
    // drop every queued (not yet decoded) item: reconnecting to a different teacher must not let
    // stale packets from the old stream flow into the decoder
    void clear() { std::lock_guard<std::mutex> lk(mtx); items.clear(); }
};

class ClientWindow : public QMainWindow {
    Q_OBJECT
public:
    // teacherIP: unicast teacher address; group: multicast group (empty = unicast receive)
    ClientWindow(const QString& teacherIP, const QString& group, QWidget* parent = nullptr);
    ~ClientWindow() override;

    // returns false when the receive thread failed to start (caller shows the error)
    bool start();

private slots:
    void onFrameTick();    // ~33ms: paced frame consumption
    void onIdleTick();     // 10ms: cached-frame repaint / no-signal placeholder
    void onStatsTick();    // 1s: title stats + auto-reconnect edge

protected:
    void closeEvent(QCloseEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;   // F11 toggle fullscreen, Esc exit fullscreen

private:
    void stopAll();
    bool renderPaused() const;   // window minimized/hidden -> skip the per-frame GPU upload

    QString teacherIP_, group_;
    bool multicast_ = false;
    bool started_ = false;

    VideoWidget* video_ = nullptr;
    QTimer* frameTimer_ = nullptr;
    QTimer* idleTimer_ = nullptr;
    QTimer* statsTimer_ = nullptr;

    FrameQueue queue_;
    EncodedQueue encQueue_;
    FFmpegDecoder decoder_;
    ClientReceiver rx_;

    std::thread decodeThread_;
    std::atomic<bool> running_{ true };
    std::atomic<uint64_t> decodedOk_{ 0 };
    std::atomic<uint32_t> latestLatencyMs_{ 0 };
    std::atomic<int> latestDarkPct_{ -1 };
    std::atomic<bool> decodePrimed_{ false };
    // set by the GUI when it decides the decoder has drifted (repeated send_packet failures) so the
    // decode thread rebuilds it on its own thread instead of racing the GUI
    std::atomic<bool> decodeResetReq_{ false };

    // the buffer the GUI presents from; kept as a member (not a per-tick local) so its allocation is
    // recycled between the frame queue, the widget cache and this window instead of churning per frame
    std::vector<uint8_t> frameBuf_;

    // pacing / repaint bookkeeping
    uint64_t lastPresentMs_ = 0;
    // minimum interval between two presented frames, derived from the panel's real refresh rate
    // (classroom student PCs are 60Hz -> 16ms). A hard-coded 16ms made a 144Hz teacher machine
    // burn CPU presenting frames the panel never showed, while a 60Hz panel must never be starved.
    int presentIntervalMs_ = 16;
    uint64_t lastRepaintMs_ = 0;
    uint64_t lastNoSigMs_ = 0;
    uint64_t lastStatsMs_ = 0;
    uint64_t lastDecoded_ = 0;
    bool wasDisconnected_ = false;
    // last teacher-pointer state we painted with: the cached-frame repaint only runs when it
    // actually changed (or after a resize), instead of re-uploading the frame 20x/s
    float lastCursorX_ = -1.0f, lastCursorY_ = -1.0f;
    bool  lastCursorVis_ = false;
};