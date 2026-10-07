#pragma once
// packet/fragment_reassembler.h -- client receive reassembly (group by frameId, slot by fragIndex) + FEC recovery
// FEC: each frame carries K parity fragments (K announced in the header's reserved low 4 bits; keyframe K=4, normal large frame K=2)
// Recovery rule: each missing fragment i needs its group's (i % K) parity fragment present, and the same group may lose at most 1
#include "packet/video_packet.h"
#include <unordered_map>
#include <vector>
#include <cstring>

class FragmentReassembler {
public:
    // process one network packet; when a whole frame is assembled return it (points into an internal buffer, valid until the next push), otherwise nullptr
    // after completion metadata is available via lastPtsMs()/lastKeyframe()/lastFrameId()
    const std::vector<uint8_t>* push(const uint8_t* buf, int len, int64_t nowMs) {
        if (len < (int)sizeof(VideoPacketHeader)) return nullptr;
        VideoPacketHeader h;
        memcpy(&h, buf, sizeof(h));
        if (h.magic != kMagic) return nullptr;
        if ((int)h.payloadLen != len - (int)sizeof(h)) return nullptr;
        // bounds check: prevent a corrupt/malicious header from causing out-of-bounds writes or memory blowup
        if (h.fragCount == 0 || h.fragCount > 256) return nullptr;
        // stale-frame filter: a duplicate delivery (multi-NIC / single-machine loopback where every
        // packet arrives via both the NIC and the loopback) re-sends every fragment of an ALREADY
        // completed frame. Without this guard the second copy re-creates the FragmentSet and assembles
        // the frame a second time -> the decoder sees the same H.264 frame twice -> corruption. Drop
        // any fragment whose frameId is in the recently-completed set.
        if (isStale(h.frameId)) return nullptr;
        int fecK = h.reserved & 0x0F;
        if (fecK != 0 && fecK != 2 && fecK != 4) fecK = 0;   // treat an abnormal K as no parity
        bool isParity = (h.flags & 2) != 0;
        if (!isParity && h.fragIndex >= h.fragCount) return nullptr;

        // memory-upper-bound guard: an abnormal/malicious packet stream can create a huge number of
        // incomplete sets via many different frameIds -> memory growth. When the incomplete-set count
        // reaches the cap, evict the set with the oldest lastTick, keeping memory bounded.
        static constexpr size_t kMaxPending = 256;
        auto it = frames_.find(h.frameId);
        if (it == frames_.end() && frames_.size() >= kMaxPending) {
            uint64_t oldestTick = UINT64_MAX;
            auto oldest = frames_.begin();
            for (auto k = frames_.begin(); k != frames_.end(); ++k)
                if ((uint64_t)k->second.lastTick < oldestTick) { oldestTick = (uint64_t)k->second.lastTick; oldest = k; }
            frames_.erase(oldest);
        }
        FragmentSet& fs = (it != frames_.end()) ? it->second : frames_[h.frameId];
        if (!fs.ready) {
            // new frame: init from the first packet (data or parity both carry fragCount and K)
            fs.total = h.fragCount;
            fs.fecK = fecK;
            fs.fragData.resize(fs.total);
            fs.have.assign(fs.total, false);
            fs.totalSize = 0;
            fs.received = 0;
            fs.ready = true;
        } else if (h.fragCount != fs.total || fecK != fs.fecK) {
            // frame header description mismatch (corrupt packet / ID-reuse residue): restart from the new header
            fs = FragmentSet{};
            fs.total = h.fragCount;
            fs.fecK = fecK;
            fs.fragData.resize(fs.total);
            fs.have.assign(fs.total, false);
            fs.ready = true;
            if (isParity && fecK <= 0) return nullptr;   // received a parity packet but no parity was announced: invalid frame
        }
        fs.lastTick = nowMs;
        fs.ptsMs = h.ptsMs;
        fs.key = (h.flags & 1) != 0;

        const uint8_t* payload = buf + sizeof(h);
        if (isParity) {
            // parity fragment: fragIndex falls in [fragCount, fragCount+fecK)
            if (fs.fecK <= 0) return nullptr;
            int pi = h.fragIndex - fs.total;
            if (pi < 0 || pi >= fs.fecK) return nullptr;
            if (fs.parityHave[pi]) return nullptr;         // duplicate packet, ignore
            fs.parityHave[pi] = true;
            fs.parity[pi].assign(payload, payload + h.payloadLen);
        } else {
            if (h.fragIndex >= fs.total) return nullptr;   // inconsistent with first packet count, drop
            if (fs.have[h.fragIndex]) return nullptr;      // duplicate packet (retransmit/multicast copy), ignore
            fs.have[h.fragIndex] = true;
            fs.fragData[h.fragIndex].assign(payload, payload + h.payloadLen);
            fs.totalSize += h.payloadLen;
            ++fs.received;
        }

        // completeness: all data fragments present -> assemble directly
        if (fs.received == fs.total) {
            return assemble(fs, h.frameId);
        }
        // or the missing fragments can be recovered via FEC parity
        if (fs.received < fs.total && tryRecover(fs)) {
            return assemble(fs, h.frameId);
        }
        return nullptr;
    }

    // capture timestamp of the most recently completed frame (ms, teacher clock) / whether keyframe / frame number (for loss stats)
    uint32_t lastPtsMs() const { return lastPts_; }
    bool     lastKeyframe() const { return lastKey_; }
    uint16_t lastFrameId() const { return lastId_; }

    // number of frames recovered via FEC parity (for stats)
    uint64_t recoveredFrames() const { return recovered_; }

    // number of frames abandoned (received >=1 fragment but not fully assembled within the timeout,
    // i.e. a fragment was lost and FEC couldn't recover). This is the REAL packet-loss signal that the
    // completed-frame-gap "lostFrames" counter misses: a frame that never assembles never advances the
    // last-completed frameId, so it never shows up as a gap. Abandoned frames ARE the hidden loss.
    uint64_t abandonedFrames() const { return abandoned_; }

    // purge frames not fully received within the timeout, preventing memory growth
    void cleanup(int64_t nowMs, int64_t timeoutMs = 150) {
        for (auto it = frames_.begin(); it != frames_.end();) {
            if (nowMs - it->second.lastTick > timeoutMs) {
                // a partially-received frame that timed out = lost fragment(s) FEC couldn't recover
                ++abandoned_;
                it = frames_.erase(it);
            } else ++it;
        }
    }

    // clear the pending frames and the completed-history ring (called on reconnect: the teacher may
    // have restarted and reset its frameId counter, so stale ids must not collide with fresh frames).
    void reset() {
        frames_.clear();
        maxCompletedId_ = 0;
        haveCompleted_ = false;
        lastPts_ = 0; lastKey_ = false; lastId_ = 0;
        // recovered_ / abandoned_ are cumulative stats: intentionally NOT reset
    }

private:
    struct FragmentSet {
        std::vector<std::vector<uint8_t>> fragData;   // data fragments
        std::vector<bool> have;                        // whether each data fragment has been received
        std::vector<uint8_t> parity[kFecMaxGroups];    // parity fragments
        bool parityHave[kFecMaxGroups] = { false, false, false, false };
        int total = 0, received = 0, totalSize = 0;
        int fecK = 0;                                  // this frame's parity group count (2=normal large frame, 4=keyframe, 0=none)
        uint32_t ptsMs = 0;
        bool key = false, ready = false;
        int64_t lastTick = 0;
    };

    // try to recover missing data fragments using parity; return true on success (and backfill fragData/have)
    bool tryRecover(FragmentSet& fs) {
        int K = fs.fecK;
        if (K < 2) return false;                        // this frame has no parity data
        int missing = fs.total - fs.received;
        if (missing <= 0 || missing > K) return false;
        // each missing fragment needs its group's (g = i % K) parity fragment present, and the same group loses at most 1
        int missGrp[kFecMaxGroups] = { 0, 0, 0, 0 };
        for (int i = 0; i < fs.total; ++i) if (!fs.have[i]) missGrp[i % K]++;
        for (int g = 0; g < K; ++g) {
            if (missGrp[g] > 1) return false;
            if (missGrp[g] == 1 && !fs.parityHave[g]) return false;
        }
        // recovery: missing fragment = its group's parity XOR (all received fragments of that group, zero-padded to full length)
        for (int i = 0; i < fs.total; ++i) {
            if (fs.have[i]) continue;
            int g = i % K;
            std::vector<uint8_t> rec(fs.parity[g].begin(), fs.parity[g].end());
            for (int j = g; j < fs.total; j += K) {
                if (!fs.have[j]) continue;
                const auto& d = fs.fragData[j];
                for (int k = 0; k < (int)rec.size(); ++k)
                    rec[k] ^= (k < (int)d.size()) ? d[k] : 0;
            }
            fs.fragData[i] = std::move(rec);
            fs.have[i] = true;
            fs.totalSize += (int)fs.fragData[i].size();
            ++fs.received;
        }
        ++recovered_;
        return true;
    }

    const std::vector<uint8_t>* assemble(FragmentSet& fs, uint16_t frameId) {
        completed_.clear();
        completed_.reserve(fs.totalSize);
        for (auto& frag : fs.fragData)
            completed_.insert(completed_.end(), frag.begin(), frag.end());
        lastPts_ = fs.ptsMs;
        lastKey_ = fs.key;
        lastId_ = frameId;
        markCompleted(frameId);   // remember for the stale-frame (double-delivery) filter
        frames_.erase(frameId);
        return &completed_;
    }

    // Recently-completed tracking for the double-delivery (stale-frame) filter. A single
    // monotonically-increasing max-completed id + a small reorder window is used instead of a fixed
    // ring of completed ids: a duplicate delivery on a multi-NIC / single-machine loopback can be
    // delayed by MANY frame periods (the NIC/loopback path lags arbitrarily), so a bounded history
    // cannot catch it, whereas "id behind maxCompleted by more than the reorder window" catches any lag.
    // The window tolerates the rare case where out-of-order UDP completes frame N+1 before N.
    bool isStale(uint16_t id) const {
        if (!haveCompleted_) return false;
        uint16_t d = (uint16_t)(maxCompletedId_ - id);   // wrap-safe backward distance
        return d > kReorderWindow && d < 32768;          // far behind = a replayed/duplicate frame
    }
    void markCompleted(uint16_t id) {
        if (!haveCompleted_) { maxCompletedId_ = id; haveCompleted_ = true; return; }
        uint16_t a = (uint16_t)(id - maxCompletedId_);   // wrap-safe forward distance
        if (a > 0 && a < 32768) maxCompletedId_ = id;    // only advance (id ahead); ignore regressions
    }

    std::unordered_map<uint16_t, FragmentSet> frames_;
    std::vector<uint8_t> completed_;
    uint32_t lastPts_ = 0;      // capture timestamp of the most recently completed frame
    bool     lastKey_ = false;  // whether the most recently completed frame is a keyframe
    uint16_t lastId_ = 0;       // frame number of the most recently completed frame
    static constexpr uint16_t kReorderWindow = 16;   // tolerate up to 16 frames of out-of-order completion
    uint16_t maxCompletedId_ = 0;   // max completed frameId (monotonic, wrap-safe)
    bool     haveCompleted_ = false;
    uint64_t recovered_ = 0;    // number of frames recovered via FEC
    uint64_t abandoned_ = 0;    // number of frames abandoned (partial frames that timed out = real loss)
};