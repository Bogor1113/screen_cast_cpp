// diag_fec.cpp -- FEC verification: after randomly dropping UDP packets, verify the correctness of XOR parity recovery
// usage: diag_fec.exe [lossPct] [runSeconds]
//       requires the teacher to stream unicast to 127.0.0.1 (e.g. teacher_cli.exe 127.0.0.1)
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <random>
#include <thread>
#include <vector>
#include "net/udp_socket.h"
#include "packet/fragment_reassembler.h"
#include "decode/ffmpeg_decoder.h"

int main(int argc, char* argv[]) {
    double lossPct = argc > 1 ? atof(argv[1]) : 1.0;
    int seconds    = argc > 2 ? atoi(argv[2]) : 10;

    UDPSocket sock;
    if (!sock.createSocket()) { printf("socket 创建失败\n"); return 1; }
    sock.bindUnicast(5004);
    sock.setRecvBuffer(8 * 1024 * 1024);
    sock.setRecvTimeout(50);

    std::mt19937 rng((unsigned)GetTickCount());
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    FragmentReassembler ra;
    FFmpegDecoder dec;
    uint64_t rx = 0, dropped = 0, frames = 0, recovered = 0, gapLost = 0, decOk = 0, decFail = 0;
    uint64_t fecAffected = 0;   // frames with FEC parity (K>0 among damaged frames)
    std::vector<uint8_t> fecKMap(65536, 0);   // frameId -> that frame's most recently announced K (to judge whether a damaged frame has parity)
    uint16_t lastId = 0; bool haveLast = false;
    char buf[1500];

    printf("diag_fec 运行中: 丢包率 %.1f%%, %d 秒 (需 teacher_cli.exe 127.0.0.1 同时推流)\n", lossPct, seconds);
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
        int n = sock.recvFrom(buf, sizeof(buf));
        if (n <= 0) continue;
        rx++;
        if (dist(rng) * 100.0 < lossPct) { dropped++; continue; }   // simulate packet loss

        int64_t now = (int64_t)GetTickCount64();
        // record the frame's announced FEC group count (for "with-parity damaged frame" judgment; small frames K=0 have no parity, their loss is inherently unrecoverable)
        VideoPacketHeader hh;
        memcpy(&hh, buf, sizeof(hh));
        if (hh.magic == kMagic) fecKMap[hh.frameId] = hh.reserved & 0x0F;
        const std::vector<uint8_t>* frame = ra.push((const uint8_t*)buf, n, now);
        ra.cleanup(now, 150);
        if (!frame) continue;

        frames++;
        recovered = ra.recoveredFrames();
        // completed-frame sequence gap = number of incomplete (recovery-failed) frames (counted by assembled completion frame id, consistent with client)
        if (haveLast) {
            uint16_t cid = ra.lastFrameId();
            uint16_t gap = (uint16_t)(cid - lastId - 1);
            if (gap > 0 && gap <= 512) {
                gapLost += gap;
                for (uint16_t g = 1; g <= gap; ++g)
                    if (fecKMap[(uint16_t)(lastId + g)] > 0) fecAffected++;   // this damaged frame has parity
            }
            lastId = cid;
        } else {
            lastId = ra.lastFrameId();
            haveLast = true;
        }

        if (dec.decode(frame->data(), (int)frame->size(), 0)) decOk++; else decFail++;
        if (frames % 50 == 0)
            printf("\r收包=%llu 丢弃=%llu(%.1f%%) 完成帧=%llu FEC恢复=%llu 恢复失败=%llu 解码成功=%llu 失败=%llu   ",
                   (unsigned long long)rx, (unsigned long long)dropped, dropped ? dropped * 100.0 / rx : 0.0,
                   (unsigned long long)frames, (unsigned long long)recovered,
                   (unsigned long long)gapLost, (unsigned long long)decOk, (unsigned long long)decFail);
    }
    printf("\n== 结果: 收包=%llu 丢弃=%llu 完成帧=%llu FEC恢复=%llu 恢复失败=%llu 解码成功=%llu 失败=%llu ==\n",
           (unsigned long long)rx, (unsigned long long)dropped, (unsigned long long)frames,
           (unsigned long long)recovered, (unsigned long long)gapLost,
           (unsigned long long)decOk, (unsigned long long)decFail);
    double affected = (double)(recovered + fecAffected);
    printf("== FEC 有效性: 带奇偶受损帧恢复率 %.1f%% (恢复 %llu / 带奇偶受损 %llu); 总受损帧 %llu(含无奇偶小帧 %llu) ==\n",
           affected > 0 ? recovered * 100.0 / affected : 0.0,
           (unsigned long long)recovered, (unsigned long long)(recovered + fecAffected),
           (unsigned long long)(recovered + gapLost), (unsigned long long)(gapLost - fecAffected));
    return 0;
}
