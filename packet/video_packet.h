#pragma once
// packet/video_packet.h -- custom video packetization protocol (RTP-like) + lightweight FEC (forward error correction)
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#pragma pack(push, 1)
struct VideoPacketHeader {
    uint32_t magic;      // 0x534C4142 ('SLAB')
    uint16_t frameId;    // frame sequence number (increments, used for ordering/loss detection)
    uint16_t fragCount;  // total number of data fragments in this frame
    uint16_t fragIndex;  // this fragment's index (data frags 0..fragCount-1; parity frags fragCount..fragCount+K-1)
    uint8_t  flags;      // bit0: keyframe; bit1: parity (FEC) fragment
    uint8_t  reserved;   // low 4 bits: FEC parity group count K (0=none, 2/4); high 4 bits reserved
    uint32_t ptsMs;      // capture timestamp (ms), client uses it to compute end-to-end latency
    uint32_t payloadLen; // valid payload length of this fragment
};
#pragma pack(pop)
static_assert(sizeof(VideoPacketHeader) == 20, "VideoPacketHeader must be 20 bytes");

constexpr uint32_t kMagic = 0x534C4142;

// ===== Cursor packet (teacher -> client, same channel and port as video) =====
// Purpose: stream the teacher's mouse pointer position to the client in real time,
// drawn as an overlay on the picture.
// Normalized coordinates are relative to the teacher's captured picture (main monitor): (0,0)=top-left, (1,1)=bottom-right.
// The client maps the normalized coords into window pixels according to its own zoom/pan,
// unaffected by resolution differences.
// Shares the UDP socket/port with the video stream: multicast mode sends to the group (one
// bandwidth serves all clients), unicast mode sends to the target client IP -- the client's
// receive thread detects the magic on the same port and diverts it.
constexpr uint32_t kCursorMagic = 0x534C4352;   // 'SLCR'
#pragma pack(push, 1)
struct CursorPacket {
    uint32_t magic;      // kCursorMagic
    float    nx, ny;     // normalised pointer coords (0..1)
    uint8_t  visible;    // 1 = teacher pointer visible (and inside main monitor)
    uint16_t srcW, srcH; // teacher's captured source resolution (for the client 1:1 native view)
    uint8_t  reserved[3];
};
#pragma pack(pop)
static_assert(sizeof(CursorPacket) == 20, "CursorPacket must be 20 bytes");
// whole packet < 1472 bytes (Ethernet MTU 1500 - IP 20 - UDP 8), avoiding IP fragmentation
constexpr int kMaxFragPayload = 1452 - (int)sizeof(VideoPacketHeader);

// ===== Lightweight FEC (XOR parity) =====
// Each frame appends K parity fragments: parity[g] = XOR of all data fragments with index i % K == g (zero-padded to full length)
// Client can recover: any K lost fragments (each in a different group); more than 1 lost in the same group is unrecoverable (beyond design limit)
// - Normal frames (P frames, small): K=2, only applied to frames with >=4 fragments -- recovers any 1 fragment or 2 in different groups
// - Keyframes (IDR, critical for join/recovery): K=4 -- recovers up to 4 fragments (each in a different group), improving join success
// Parity fragment length = kMaxFragPayload (data fragments are zero-padded to full length before XOR), the recovered last fragment
// may carry trailing zeros -- harmless to the H.264 decoder (Annex-B ignores trailing zero bytes)
constexpr int kFecMaxGroups = 4;
constexpr int kFecMinFrags  = 4;   // only attach parity when a frame has >= this many data fragments (small frames lose little, parity overhead not worth it)

// simple packet-buffer pool: recycles the std::vector<uint8_t> buffers that carry UDP fragments,
// avoiding ~600-700 heap allocations/sec on the teacher capture thread during active video (a 1080p
// keyframe alone is ~140 data fragments + FEC parity). The sender thread releases each buffer after
// sendTo; the capture thread re-acquires it on the next frame.
class PacketBufferPool {
public:
    std::vector<uint8_t> acquire(size_t size) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!pool_.empty()) {
            std::vector<uint8_t> v = std::move(pool_.back());
            pool_.pop_back();
            v.resize(size);   // keep the capacity, adjust size (caller overwrites all `size` bytes)
            return v;
        }
        return std::vector<uint8_t>(size);
    }
    void release(std::vector<uint8_t>&& v) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pool_.size() >= kMaxPooled) return;   // cap memory: drop surplus buffers
        v.clear();                                // keep capacity
        pool_.push_back(std::move(v));
    }
private:
    std::mutex mtx_;
    std::vector<std::vector<uint8_t>> pool_;
    static constexpr size_t kMaxPooled = 1024;   // ~1.4MB of pooled buffers worst case
};

// compute the parity group count for this frame (written into reserved low 4 bits, 0 = no parity)
inline int fecGroupCount(int fragCount, bool keyframe) {
    if (fragCount < kFecMinFrags) return 0;
    return keyframe ? 4 : 2;
}

// cut a whole encoded frame into a set of UDP packets (data fragments + optional FEC parity fragments)
// NOTE: avoid std::min to keep windows.h's min/max macros from corrupting it
// pool: optional PacketBufferPool to recycle the fragment buffers (avoid per-frame allocations on the hot path)
inline std::vector<std::vector<uint8_t>> packetizeFrame(
    const uint8_t* data, int size, uint16_t frameId, uint32_t ptsMs, bool keyframe,
    PacketBufferPool* pool = nullptr) {
    std::vector<std::vector<uint8_t>> out;
    int fragCount = (size + kMaxFragPayload - 1) / kMaxFragPayload;
    int K = fecGroupCount(fragCount, keyframe);
    out.reserve((size_t)fragCount + (size_t)(K > 0 ? K : 0));
    uint8_t baseFlags = keyframe ? 1 : 0;
    for (int i = 0; i < fragCount; ++i) {
        int off = i * kMaxFragPayload;
        int len = (size - off < kMaxFragPayload) ? (size - off) : kMaxFragPayload;
        VideoPacketHeader h{};
        h.magic = kMagic;
        h.frameId = frameId;
        h.fragCount = (uint16_t)fragCount;
        h.fragIndex = (uint16_t)i;
        h.flags = baseFlags;
        h.reserved = (uint8_t)K;
        h.ptsMs = ptsMs;
        h.payloadLen = (uint32_t)len;
        std::vector<uint8_t> pkt = pool ? pool->acquire(sizeof(h) + len)
                                        : std::vector<uint8_t>(sizeof(h) + len);
        memcpy(pkt.data(), &h, sizeof(h));
        memcpy(pkt.data() + sizeof(h), data + off, len);
        out.push_back(std::move(pkt));
    }
    // FEC parity fragments (only for larger frames): XOR grouped by fragment index mod K, data fragments zero-padded to full length
    if (K > 0) {
        std::vector<uint8_t> parity[kFecMaxGroups];
        for (int g = 0; g < K; ++g) parity[g].assign(kMaxFragPayload, 0);
        for (int i = 0; i < fragCount; ++i) {
            int g = i % K;
            int off = i * kMaxFragPayload;
            int n = (i == fragCount - 1) ? (size - off) : kMaxFragPayload;
            for (int j = 0; j < n; ++j) parity[g][j] ^= data[off + j];
        }
        for (int g = 0; g < K; ++g) {
            VideoPacketHeader h{};
            h.magic = kMagic;
            h.frameId = frameId;
            h.fragCount = (uint16_t)fragCount;
            h.fragIndex = (uint16_t)(fragCount + g);
            h.flags = (uint8_t)(baseFlags | 2);   // bit1 = FEC parity fragment
            h.reserved = (uint8_t)K;
            h.ptsMs = ptsMs;
            h.payloadLen = (uint32_t)parity[g].size();
            std::vector<uint8_t> pkt = pool ? pool->acquire(sizeof(h) + parity[g].size())
                                            : std::vector<uint8_t>(sizeof(h) + parity[g].size());
            memcpy(pkt.data(), &h, sizeof(h));
            memcpy(pkt.data() + sizeof(h), parity[g].data(), parity[g].size());
            out.push_back(std::move(pkt));
        }
    }
    return out;
}
