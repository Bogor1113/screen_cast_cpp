// diag_decode.cpp -- diagnostic: raw avcodec call, print send_packet/receive_frame return codes on decode failure
// usage: diag_decode.exe <teacherIP> [multicastGroupIP]
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "net/client_receiver.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

static std::atomic<bool> g_running{ true };
static long long g_sendErr = 0, g_recvEagain = 0, g_recvOther = 0, g_ok = 0, g_total = 0;

int main(int argc, char* argv[]) {
    if (argc < 2) { printf("用法: diag_decode.exe <教师端IP> [组播组IP]\n"); return 1; }
    const char* teacherIP = argv[1];
    const char* group     = argc > 2 ? argv[2] : "";

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->thread_count = 1;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(ctx, codec, nullptr) < 0) { printf("decoder open failed\n"); return 1; }
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  fr  = av_frame_alloc();

    ClientReceiver rx;
    if (!rx.start(teacherIP, group, 5004, 5005, [&](const ReceivedFrame& f) {
        g_total++;
        pkt->data = (uint8_t*)f.data->data();
        pkt->size = (int)f.data->size();
        int sr = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (sr < 0) { g_sendErr++; return; }
        while (true) {
            int rr = avcodec_receive_frame(ctx, fr);
            if (rr == 0) { g_ok++; av_frame_unref(fr); continue; }
            if (rr == AVERROR(EAGAIN)) { if (g_recvEagain == 0) printf("[recv] EAGAIN size=%d key=%d\n", pkt->size, (int)f.keyframe); g_recvEagain++; }
            else { if (g_recvOther == 0) { char e[64]{}; av_strerror(rr, e, sizeof(e)); printf("[recv] err=%s size=%d key=%d\n", e, pkt->size, (int)f.keyframe); } g_recvOther++; }
            break;
        }
    })) { printf("启动失败\n"); return 1; }

    printf("diag 运行中 %s (12 秒后自动退出)\n", group[0] ? "组播" : "单播");
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(12)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        printf("\r帧=%lld 产出画面=%lld send错误=%lld EAGAIN=%lld 其它错=%lld   ",
               g_total, g_ok, g_sendErr, g_recvEagain, g_recvOther);
        fflush(stdout);
    }
    rx.stop();
    printf("\n== 结论: send错误=%lld EAGAIN(无画面产出)=%lld 其它=%lld ==\n",
           g_sendErr, g_recvEagain, g_recvOther);
    avcodec_free_context(&ctx); av_packet_free(&pkt); av_frame_free(&fr);
    return 0;
}
