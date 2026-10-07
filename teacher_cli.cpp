// teacher_cli.cpp -- teacher console version (no GUI, reuses the streaming engine, for scripts/headless env)
// usage: teacher_cli.exe <targetIP> [bitrateKbps] [d|n] [h|s|f] [--adaptive]
//       a multicast address as targetIP (e.g. 239.255.1.1) = multicast mode (40 clients); a unicast IP = 1:1 streaming
//       d = differential on (default) n = differential off;  h = HD (default) s = SD f = fluent
//       --adaptive = adaptive bitrate (AIMD from client-reported loss, 400kbps ~ ceiling)
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <thread>
#include "core/streamer.h"

static std::atomic<bool> g_running{ true };
static void onSignal(int) { g_running.store(false); }

static bool isMulticastAddr(const char* ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    return sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a >= 224 && a <= 239;
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);   // UTF-8 源码 -> 控制台正确显示中文
    if (argc < 2) {
        printf("用法: teacher_cli.exe <目标IP> [码率Kbps] [d|n] [h|s|f]\n");
        printf("  <目标IP>: 单播 IP(1:1 推流) 或组播地址如 239.255.1.1(组播, 40 客户端)\n");
        printf("  d = 差分投屏开(默认), n = 差分关;  h = 高清(默认), s = 标清, f = 流畅\n");
        return 1;
    }
    const char* ip = argv[1];
    int  bitrate = argc > 2 ? atoi(argv[2]) : 0;   // 0 = use the quality-preset default bitrate; >0 overrides
    bool diffOn  = (argc > 3 && argv[3][0] == 'n') ? false : true;
    // quality preset: h=HD (default, cap 1080p) s=SD (cap 720p, weak network) f=fluent (cap 540p, most bandwidth savings)
    Quality q = Quality::HD1080P;
    if (argc > 4) {
        switch (argv[4][0]) {
        case 's': q = Quality::SD720P;  break;
        case 'f': q = Quality::FLUENT;  break;
        default:  q = Quality::HD1080P; break;
        }
    }
    bool adaptive = false;   // --adaptive: adaptive bitrate (AIMD from client-reported loss)
    for (int i = 2; i < argc; ++i)
        if (_stricmp(argv[i], "--adaptive") == 0) adaptive = true;

    Streamer s;
    s.setQuality(q);
    if (bitrate > 0) s.setBitrate(bitrate);
    s.setAdaptiveBitrate(adaptive);
    s.setDifferentialEnabled(diffOn);
    if (!s.start(ip, 5004, 5005)) {
        printf("[错误] 推流启动失败\n");
        return 1;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    printf("[教师端] 已启动 -> %s (%s, %s, 差分%s, 码率控制%s), Ctrl+C 停止\n", ip,
           isMulticastAddr(ip) ? "组播模式" : "单播模式",
           bitrate > 0 ? "自定义码率" : "档位默认码率", diffOn ? "开" : "关",
           adaptive ? "自适应" : "固定");

    while (g_running.load()) {
        StreamStats st = s.snapshot();
        if (st.encWidth > 0) {
            printf("\r采集 %llu | 发送 %llu | 差分跳过 %llu | %llu 包 | 目标 %d kbps | %s | %dx%d -> %dx%d   ",
                   (unsigned long long)st.framesCaptured, (unsigned long long)st.framesSent,
                   (unsigned long long)st.framesSkipped, (unsigned long long)st.packetsSent,
                   st.targetKbps,
                   st.gdiMode ? "GDI捕获" : "DXGI捕获",
                   st.srcWidth, st.srcHeight, st.encWidth, st.encHeight);
            // online client list (heartbeat tracking, 10s timeout offline)
            std::vector<std::string> ips = s.clientIps();
            printf("| 客户端 %zu: ", ips.size());
            for (size_t i = 0; i < ips.size(); ++i)
                printf("%s%s", i ? "," : "", ips[i].c_str());
            printf("   ");
            fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    s.stop();
    printf("\n[教师端] 已停止\n");
    return 0;
}
