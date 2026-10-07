// teacher_main.cpp -- teacher (phase one: unicast 1:1, switch to multicast after link test passes)
// usage: teacher.exe <clientIP> [bitrateKbps]
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "capture/dxgi_capture.h"
#include "encode/ffmpeg_encoder.h"
#include "net/udp_socket.h"
#include "packet/video_packet.h"

static std::atomic<bool>     g_running{ true };
static std::atomic<uint16_t> g_frameId{ 0 };

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("用法: teacher.exe <客户端IP> [码率Kbps]\n");
        return 1;
    }
    const char* clientIP = argv[1];
    const int   bitrate  = argc > 2 ? atoi(argv[2]) : 6000;
    const uint16_t kVideoPort   = 5004;
    const uint16_t kControlPort = 5005;
    const int kFps = 30;

    // ---- network (unicast: target is the client IP; multicast: replace with "239.255.1.1") ----
    UDPSocket videoSock, ctrlSock;
    videoSock.createSocket();
    videoSock.setupSender(clientIP, kVideoPort, 1);
    ctrlSock.createSocket();
    ctrlSock.bindUnicast(kControlPort);          // receive client control requests

    // ---- lazy encoder init: create on the first frame by the actual capture resolution, avoid hard-coding resolution ----
    FFmpegEncoder enc;
    std::atomic<bool> encReady{ false };

    // ---- capture ----
    DXGICapture capture;
    bool capOk = capture.start(0, [&](const CapturedFrame& f) {
        if (!encReady.load()) {
            if (!enc.init(f.width, f.height, kFps, bitrate,
                          [&](const uint8_t* data, int size, uint64_t captureMs, bool key) {
                              auto pkts = packetizeFrame(data, size, ++g_frameId,
                                                         (uint32_t)captureMs, key);
                              for (auto& p : pkts)
                                  videoSock.sendTo(clientIP, kVideoPort, p.data(), (int)p.size());
                          })) {
                fprintf(stderr, "[main] 编码器初始化失败\n");
                g_running = false;
                return;
            }
            encReady.store(true);
            fprintf(stderr, "[main] 编码器已就绪: %dx%d@%d, %dkbps\n", f.width, f.height, kFps, bitrate);
        }
        enc.encode(f.data, f.stride, f.ptsMs);
    });
    if (!capOk) {
        printf("桌面捕获初始化失败(需在交互式桌面会话中运行)\n");
        return 3;
    }

    // ---- control thread: respond to client keyframe requests ----
    std::thread ctrl([&] {
        char buf[64];
        while (g_running) {
            int n = ctrlSock.recvFrom(buf, sizeof(buf));
            if (n >= 8 && strncmp(buf, "KEYFRAME", 8) == 0) {
                if (encReady.load()) enc.forceKeyframe();
                printf("[ctrl] 收到关键帧请求\n");
            }
        }
    });

    printf("教师端启动: 目标 %s:%d, 码率 %dkbps\n", clientIP, kVideoPort, bitrate);
    int frames = 0;
    while (g_running) {
        if (capture.pumpFrame() && ++frames % 120 == 0)
            fprintf(stderr, "[main] 已处理 %d 帧\n", frames);
    }

    g_running = false;
    ctrl.detach();
    enc.shutdown();
    return 0;
}
