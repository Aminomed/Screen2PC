#pragma once
#include "Protocol.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace s2p {

class UdpVideoFecSender {
public:
    static constexpr size_t kDefaultGroupDataShards = 10;

    // Splits [VideoFrameHeader + encoded video bytes] into MTU-safe UDP datagrams
    // protected by systematic GF(2^8) Cauchy Reed-Solomon FEC parity packets.
    bool BuildPackets(uint32_t monitorIndex,
                      uint64_t frameIndex,
                      bool isKeyframe,
                      const VideoFrameHeader& frameHeader,
                      const uint8_t* videoBytes,
                      size_t videoSize,
                      std::vector<std::vector<uint8_t>>& outPackets,
                      size_t* outParityCount = nullptr);
};

struct UdpVideoStats {
    uint64_t framesCompleted = 0;
    uint64_t framesDropped = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsRecovered = 0;
};

class UdpVideoFecReceiver {
public:
    UdpVideoFecReceiver() = default;

    void Reset();

    // Feeds one incoming UDP datagram. Returns true and fills outHeader/outPayload
    // if this packet completed a frame (either directly or via RS-FEC recovery).
    // Sets outNeedKeyframe = true if an earlier incomplete frame was dropped with gap.
    bool OnPacket(const uint8_t* data,
                  size_t len,
                  uint64_t nowMs,
                  VideoFrameHeader& outHeader,
                  std::vector<uint8_t>& outPayload,
                  bool& outNeedKeyframe);

    // Purges frames waiting longer than maxAgeMs and reports if a gap happened.
    bool PruneExpired(uint64_t nowMs, uint64_t maxAgeMs = 18);

    UdpVideoStats Stats() const { return m_stats; }

private:
    struct GroupState {
        uint8_t dataK = 0;
        uint8_t parM = 0;
        uint16_t shardStride = 0;
        uint16_t receivedCount = 0;
        uint32_t presentMask = 0; // bit i for shard i in [0 .. K+M-1]
        std::vector<std::vector<uint8_t>> shards; // size K+M, each padded to shardStride
        std::vector<uint16_t> actualBytes;        // size K+M
        bool solved = false;
    };

    struct FrameAssembly {
        uint64_t frameIndex = 0;
        uint32_t frameBytes = 0;
        uint64_t firstSeenMs = 0;
        bool isKeyframe = false;
        std::vector<GroupState> groups;
        uint32_t groupsSolved = 0;
    };

    std::deque<FrameAssembly> m_frames;
    uint64_t m_lastCompletedFrame = 0;
    bool m_hasCompleted = false;
    UdpVideoStats m_stats{};

    static bool SolveGroup(GroupState& g, uint64_t& outRecoveredCount);
};

// Expose GF(2^8) Cauchy RS helpers for unit testing
namespace detail {
void RsEncodeParity(uint8_t dataK,
                    uint8_t parM,
                    size_t shardStride,
                    const uint8_t* const* dataPtrs,
                    uint8_t* const* parPtrs);

bool RsRecoverData(uint8_t dataK,
                   uint8_t parM,
                   size_t shardStride,
                   uint32_t presentMask,
                   uint8_t** shardPtrs);
} // namespace detail

} // namespace s2p
