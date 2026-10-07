// client_console.cpp -- client (console version: receive stats + decode verify, no render)
// usage: client_console.exe <teacherIP> [multicastGroupIP]
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include "net/client_receiver.h"
#include "decode/ffmpeg_decoder.h"

static std::atomic<bool> g_running{ true };
static void onSignal(int) { g_running.store(false); }

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);   // UTF-8 source -> console correctly displays Chinese
    if (argc < 2) {
        printf("用法: client_console.exe <教师端IP> [组播组IP]\n");
        printf("  [组播组IP] 可省略(单播); 填 239.255.1.1 等组播地址则加入组播组收流\n");
        return 1;
    }
    const char* teacherIP = argv[1];
    const char* group     = argc > 2 ? argv[2] : "";
    bool multicast = group[0] != '\0';

    FFmpegDecoder decoder;
    std::atomic<uint64_t> decodedOk{ 0 }, decodedFail{ 0 };
    std::atomic<int> decW{ 0 }, decH{ 0 };
    std::atomic<bool> decodePrimed{ false };   // first frame decoded: receive thread keeps requesting keyframes based on this

    ClientReceiver rx;
    if (!rx.start(teacherIP, group, 5004, 5005, [&](const ReceivedFrame& f) {
        if (decoder.decode(f.data->data(), (int)f.data->size(), f.captureMs)) {
            ++decodedOk;
            if (!decodePrimed.load()) decodePrimed.store(true);
            decW = decoder.width(); decH = decoder.height();
        } else {
            ++decodedFail;
        }
    })) {
        printf("[错误] 客户端启动失败\n");
        return 1;
    }
    rx.setDecodePrimed(&decodePrimed);

    printf("客户端启动: %s, 教师端 %s (Ctrl+C 退出)\n",
           multicast ? "加入组播组收流" : "单播监听", teacherIP);
    auto t0 = std::chrono::steady_clock::now();
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - t0).count();
        printf("\r[%llds] 收包=%llu 完整帧=%llu FEC恢复=%llu 丢帧=%llu 放弃=%llu 解码成功=%llu 失败=%llu %dx%d   ",
               (long long)sec, (unsigned long long)rx.packets(),
               (unsigned long long)rx.frames(),
               (unsigned long long)rx.recoveredFrames(),
               (unsigned long long)rx.lostFrames(),
               (unsigned long long)rx.abandonedFrames(),
               (unsigned long long)decodedOk.load(), (unsigned long long)decodedFail.load(),
               decW.load(), decH.load());
        fflush(stdout);
    }
    rx.stop();
    printf("\n客户端已停止\n");
    return 0;
}
