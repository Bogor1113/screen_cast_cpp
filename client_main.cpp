// client_main.cpp -- client (phase one: unicast 1:1 receiver)
// usage: client.exe <teacherIP>
// multicast: replace bindUnicast with bindAndJoinGroup("239.255.1.1", kVideoPort)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include "net/udp_socket.h"
#include "packet/fragment_reassembler.h"

static std::atomic<bool> g_running{ true };

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("用法: client.exe <教师端IP>\n");
        return 1;
    }
    const char* teacherIP = argv[1];
    const uint16_t kVideoPort   = 5004;
    const uint16_t kControlPort = 5005;

    UDPSocket videoSock, ctrlSock;
    videoSock.createSocket();
    videoSock.bindUnicast(kVideoPort);          // unicast version
    videoSock.setRecvBuffer(8 * 1024 * 1024);   // 8MB receive buffer, burst-resistant

    ctrlSock.createSocket();                    // only sends, never receives, no bind needed

    FragmentReassembler reassembler;
    auto t0 = std::chrono::steady_clock::now();

    // control thread: periodically request a keyframe (phase-one simplified version; phase four uses loss-detection trigger)
    std::thread ctrl([&] {
        char req[] = "KEYFRAME";
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            ctrlSock.sendTo(teacherIP, kControlPort, req, (int)strlen(req));
        }
    });

    char buf[1500];
    uint64_t pkts = 0, frames = 0;
    printf("客户端启动: 监听 %d, 教师端 %s\n", kVideoPort, teacherIP);
    while (g_running) {
        int n = videoSock.recvFrom(buf, sizeof(buf));
        if (n <= 0) continue;
        ++pkts;

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
        const std::vector<uint8_t>* frame = reassembler.push((const uint8_t*)buf, n, now);
        reassembler.cleanup(now, 100);

        if (frame) {
            ++frames;
            printf("收到完整帧 #%llu, %zu 字节\n",
                   (unsigned long long)frames, frame->size());
            // TODO: 1) FFmpeg decode (H.264 -> NV12/BGRA)
            //       2) D3D11 upload + render (debug phase can use SetDIBitsToDevice)
            //       3) pace rendering by ptsMs, measure end-to-end latency
        }
        if (pkts % 1000 == 0)
            printf("统计: 收包=%llu 完成帧=%llu\n", (unsigned long long)pkts,
                   (unsigned long long)frames);
    }

    g_running = false;
    ctrl.join();
    return 0;
}
