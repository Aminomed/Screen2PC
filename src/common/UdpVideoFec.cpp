#include "UdpVideoFec.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace s2p {

namespace {

struct GfTables {
    std::array<uint8_t, 512> exp{};
    std::array<uint8_t, 256> log{};
    std::array<uint8_t, 256> inv{};
    std::array<std::array<uint8_t, 256>, 256> mul{};

    GfTables() {
        uint16_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            exp[i + 255] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) {
                x ^= 0x11D; // AES/QR primitive polynomial x^8 + x^4 + x^3 + x^2 + 1
            }
        }
        exp[510] = exp[0];
        exp[511] = exp[1];
        log[0] = 0;
        inv[0] = 0;
        for (int a = 1; a < 256; ++a) {
            inv[a] = exp[255 - log[a]];
        }
        for (int a = 0; a < 256; ++a) {
            for (int b = 0; b < 256; ++b) {
                if (a == 0 || b == 0) {
                    mul[a][b] = 0;
                } else {
                    mul[a][b] = exp[log[a] + log[b]];
                }
            }
        }
    }
};

const GfTables& GetGf() {
    static const GfTables t;
    return t;
}

inline uint8_t GfCauchyEntry(const GfTables& gf, uint8_t rowParJ, uint8_t colI, uint8_t dataK) {
    // Disjoint elements: colI in [0..dataK-1], y = (dataK + rowParJ) in [dataK..dataK+parM-1]
    const uint8_t denom = static_cast<uint8_t>(colI ^ (dataK + rowParJ));
    return gf.inv[denom];
}

size_t ComputeGroupStride(uint32_t frameBytes, uint16_t groupIndex, uint8_t groupDataK) {
    const size_t baseOff = static_cast<size_t>(groupIndex) * UdpVideoFecSender::kDefaultGroupDataShards * kUdpVideoShardSize;
    if (baseOff >= frameBytes) return 0;
    const size_t rem = frameBytes - baseOff;
    if (groupDataK <= 1) {
        return std::min<size_t>(rem, kUdpVideoShardSize);
    }
    return kUdpVideoShardSize;
}

} // namespace

namespace detail {

void RsEncodeParity(uint8_t dataK,
                    uint8_t parM,
                    size_t shardStride,
                    const uint8_t* const* dataPtrs,
                    uint8_t* const* parPtrs) {
    if (dataK == 0 || parM == 0 || shardStride == 0) return;
    const auto& gf = GetGf();
    for (uint8_t j = 0; j < parM; ++j) {
        uint8_t* dst = parPtrs[j];
        std::memset(dst, 0, shardStride);
        for (uint8_t i = 0; i < dataK; ++i) {
            const uint8_t coeff = GfCauchyEntry(gf, j, i, dataK);
            const uint8_t* src = dataPtrs[i];
            if (coeff == 1) {
                for (size_t b = 0; b < shardStride; ++b) {
                    dst[b] ^= src[b];
                }
            } else if (coeff != 0) {
                const uint8_t* mulRow = gf.mul[coeff].data();
                for (size_t b = 0; b < shardStride; ++b) {
                    dst[b] ^= mulRow[src[b]];
                }
            }
        }
    }
}

bool RsRecoverData(uint8_t dataK,
                   uint8_t parM,
                   size_t shardStride,
                   uint32_t presentMask,
                   uint8_t** shardPtrs) {
    if (dataK == 0 || dataK > 16 || parM > 4 || shardStride == 0) return false;

    // Fast path: all data shards are already present
    const uint32_t fullDataMask = static_cast<uint32_t>((1ULL << dataK) - 1ULL);
    if ((presentMask & fullDataMask) == fullDataMask) {
        return true;
    }

    // Collect exactly K available shards
    uint8_t chosenIdx[16]{};
    uint8_t chosenCount = 0;
    const uint8_t total = static_cast<uint8_t>(dataK + parM);
    for (uint8_t s = 0; s < total && chosenCount < dataK; ++s) {
        if (presentMask & (1u << s)) {
            chosenIdx[chosenCount++] = s;
        }
    }
    if (chosenCount < dataK) return false;

    const auto& gf = GetGf();
    // Build augmented [A | I] matrix (K x 2K)
    uint8_t mat[16][32]{};
    for (uint8_t r = 0; r < dataK; ++r) {
        const uint8_t idx = chosenIdx[r];
        if (idx < dataK) {
            mat[r][idx] = 1;
        } else {
            const uint8_t j = static_cast<uint8_t>(idx - dataK);
            for (uint8_t c = 0; c < dataK; ++c) {
                mat[r][c] = GfCauchyEntry(gf, j, c, dataK);
            }
        }
        mat[r][dataK + r] = 1;
    }

    // Gauss-Jordan elimination over GF(2^8)
    for (uint8_t col = 0; col < dataK; ++col) {
        uint8_t pivot = col;
        while (pivot < dataK && mat[pivot][col] == 0) {
            ++pivot;
        }
        if (pivot >= dataK) return false;
        if (pivot != col) {
            for (uint8_t c = 0; c < 2 * dataK; ++c) {
                std::swap(mat[pivot][c], mat[col][c]);
            }
        }
        const uint8_t invP = gf.inv[mat[col][col]];
        if (invP != 1) {
            const uint8_t* mulRow = gf.mul[invP].data();
            for (uint8_t c = 0; c < 2 * dataK; ++c) {
                mat[col][c] = mulRow[mat[col][c]];
            }
        }
        for (uint8_t r = 0; r < dataK; ++r) {
            if (r == col || mat[r][col] == 0) continue;
            const uint8_t factor = mat[r][col];
            const uint8_t* mulRow = gf.mul[factor].data();
            for (uint8_t c = 0; c < 2 * dataK; ++c) {
                mat[r][c] ^= mulRow[mat[col][c]];
            }
        }
    }

    // Reconstruct every missing data shard
    for (uint8_t dstCol = 0; dstCol < dataK; ++dstCol) {
        if (presentMask & (1u << dstCol)) continue;
        uint8_t* dst = shardPtrs[dstCol];
        std::memset(dst, 0, shardStride);
        for (uint8_t r = 0; r < dataK; ++r) {
            const uint8_t coeff = mat[dstCol][dataK + r];
            if (coeff == 0) continue;
            const uint8_t* src = shardPtrs[chosenIdx[r]];
            if (coeff == 1) {
                for (size_t b = 0; b < shardStride; ++b) {
                    dst[b] ^= src[b];
                }
            } else {
                const uint8_t* mulRow = gf.mul[coeff].data();
                for (size_t b = 0; b < shardStride; ++b) {
                    dst[b] ^= mulRow[src[b]];
                }
            }
        }
    }
    return true;
}

} // namespace detail

bool UdpVideoFecSender::BuildPackets(uint32_t monitorIndex,
                                     uint64_t frameIndex,
                                     bool isKeyframe,
                                     const VideoFrameHeader& frameHeader,
                                     const uint8_t* videoBytes,
                                     size_t videoSize,
                                     std::vector<std::vector<uint8_t>>& outPackets,
                                     size_t* outParityCount) {
    outPackets.clear();
    if (outParityCount) *outParityCount = 0;
    if (videoSize > 0 && !videoBytes) return false;

    const size_t totalBytes = sizeof(VideoFrameHeader) + videoSize;
    if (totalBytes > kUdpVideoMaxPayloadBytes) return false;

    std::vector<uint8_t> blob(totalBytes);
    std::memcpy(blob.data(), &frameHeader, sizeof(VideoFrameHeader));
    if (videoSize > 0) {
        std::memcpy(blob.data() + sizeof(VideoFrameHeader), videoBytes, videoSize);
    }

    const size_t totalShards = (totalBytes + kUdpVideoShardSize - 1) / kUdpVideoShardSize;
    const size_t groupCount = (totalShards + kDefaultGroupDataShards - 1) / kDefaultGroupDataShards;

    size_t parityTotal = 0;
    for (size_t g = 0; g < groupCount; ++g) {
        const size_t firstShard = g * kDefaultGroupDataShards;
        const uint8_t dataK = static_cast<uint8_t>(std::min(kDefaultGroupDataShards, totalShards - firstShard));
        // Robust FEC sizing:
        // - Keyframe blocks always get >= 2 parity shards (3 for large blocks)
        // - Delta/P blocks get 1 parity for tiny 1-3 shard packets, 2 for medium/large blocks
        uint8_t parM = 0;
        if (isKeyframe) {
            parM = (dataK >= 5) ? 3 : 2;
        } else {
            parM = (dataK >= 4) ? 2 : 1;
        }

        const size_t stride = ComputeGroupStride(static_cast<uint32_t>(totalBytes), static_cast<uint16_t>(g), dataK);
        std::array<std::vector<uint8_t>, kDefaultGroupDataShards> dataStorage{};
        std::array<const uint8_t*, kDefaultGroupDataShards> dataPtrs{};
        for (uint8_t i = 0; i < dataK; ++i) {
            const size_t off = (firstShard + i) * kUdpVideoShardSize;
            const size_t valid = std::min(kUdpVideoShardSize, totalBytes - off);
            dataStorage[i].assign(stride, 0);
            std::memcpy(dataStorage[i].data(), blob.data() + off, valid);
            dataPtrs[i] = dataStorage[i].data();

            UdpVideoShardHeader sh{};
            sh.magic = kVideoUdpFecMagic;
            sh.monitorIndex = monitorIndex;
            sh.frameIndex = frameIndex;
            sh.frameBytes = static_cast<uint32_t>(totalBytes);
            sh.groupIndex = static_cast<uint16_t>(g);
            sh.groupDataK = dataK;
            sh.groupParM = parM;
            sh.shardInGroup = i;
            sh.flags = isKeyframe ? 1 : 0;
            sh.shardBytes = static_cast<uint16_t>(valid);

            std::vector<uint8_t> pkt(sizeof(sh) + valid);
            std::memcpy(pkt.data(), &sh, sizeof(sh));
            std::memcpy(pkt.data() + sizeof(sh), dataStorage[i].data(), valid);
            outPackets.push_back(std::move(pkt));
        }

        if (parM > 0) {
            std::array<std::vector<uint8_t>, 4> parStorage{};
            std::array<uint8_t*, 4> parPtrs{};
            for (uint8_t j = 0; j < parM; ++j) {
                parStorage[j].resize(stride);
                parPtrs[j] = parStorage[j].data();
            }
            detail::RsEncodeParity(dataK, parM, stride, dataPtrs.data(), parPtrs.data());
            for (uint8_t j = 0; j < parM; ++j) {
                UdpVideoShardHeader sh{};
                sh.magic = kVideoUdpFecMagic;
                sh.monitorIndex = monitorIndex;
                sh.frameIndex = frameIndex;
                sh.frameBytes = static_cast<uint32_t>(totalBytes);
                sh.groupIndex = static_cast<uint16_t>(g);
                sh.groupDataK = dataK;
                sh.groupParM = parM;
                sh.shardInGroup = static_cast<uint8_t>(dataK + j);
                sh.flags = isKeyframe ? 1 : 0;
                sh.shardBytes = static_cast<uint16_t>(stride);

                std::vector<uint8_t> pkt(sizeof(sh) + stride);
                std::memcpy(pkt.data(), &sh, sizeof(sh));
                std::memcpy(pkt.data() + sizeof(sh), parStorage[j].data(), stride);
                outPackets.push_back(std::move(pkt));
                ++parityTotal;
            }
        }
    }

    if (outParityCount) *outParityCount = parityTotal;
    return true;
}

void UdpVideoFecReceiver::Reset() {
    m_frames.clear();
    m_lastCompletedFrame = 0;
    m_hasCompleted = false;
    m_stats = {};
}

bool UdpVideoFecReceiver::SolveGroup(GroupState& g, uint64_t& outRecoveredCount) {
    if (g.solved) return true;
    const uint32_t fullDataMask = (g.dataK == 32) ? 0xFFFFFFFFu : ((1u << g.dataK) - 1u);
    if ((g.presentMask & fullDataMask) == fullDataMask) {
        g.solved = true;
        return true;
    }
    if (g.receivedCount < g.dataK) {
        return false;
    }

    std::vector<uint8_t*> ptrs(g.shards.size(), nullptr);
    for (size_t i = 0; i < g.shards.size(); ++i) {
        if (g.shards[i].size() < g.shardStride) {
            g.shards[i].resize(g.shardStride, 0);
        }
        ptrs[i] = g.shards[i].data();
    }

    if (!detail::RsRecoverData(g.dataK, g.parM, g.shardStride, g.presentMask, ptrs.data())) {
        return false;
    }

    uint64_t recovered = 0;
    for (uint8_t i = 0; i < g.dataK; ++i) {
        if (!(g.presentMask & (1u << i))) {
            ++recovered;
        }
    }
    outRecoveredCount += recovered;
    g.presentMask |= fullDataMask;
    g.solved = true;
    return true;
}

bool UdpVideoFecReceiver::OnPacket(const uint8_t* data,
                                   size_t len,
                                   uint64_t nowMs,
                                   VideoFrameHeader& outHeader,
                                   std::vector<uint8_t>& outPayload,
                                   bool& outNeedKeyframe) {
    outNeedKeyframe = false;
    if (!data || len < sizeof(UdpVideoShardHeader)) return false;

    UdpVideoShardHeader sh{};
    std::memcpy(&sh, data, sizeof(sh));
    if (sh.magic != kVideoUdpFecMagic) return false;
    if (sh.frameBytes < sizeof(VideoFrameHeader) || sh.frameBytes > kUdpVideoMaxPayloadBytes) return false;
    if (sh.groupDataK == 0 || sh.groupDataK > 16 || sh.groupParM > 4) return false;
    const uint8_t totalInGroup = static_cast<uint8_t>(sh.groupDataK + sh.groupParM);
    if (sh.shardInGroup >= totalInGroup) return false;
    if (sizeof(sh) + sh.shardBytes > len) return false;
    if (sh.shardBytes > kUdpVideoShardSize) return false;

    ++m_stats.packetsReceived;

    if (m_hasCompleted && sh.frameIndex <= m_lastCompletedFrame) {
        return false; // Late/duplicate shard for already-delivered frame
    }

    const size_t totalShards = (sh.frameBytes + kUdpVideoShardSize - 1) / kUdpVideoShardSize;
    const size_t groupCount = (totalShards + UdpVideoFecSender::kDefaultGroupDataShards - 1) / UdpVideoFecSender::kDefaultGroupDataShards;
    if (groupCount == 0 || sh.groupIndex >= groupCount) return false;

    const size_t expectedK = std::min(UdpVideoFecSender::kDefaultGroupDataShards,
                                      totalShards - sh.groupIndex * UdpVideoFecSender::kDefaultGroupDataShards);
    if (sh.groupDataK != expectedK) return false;

    FrameAssembly* fa = nullptr;
    for (auto& existing : m_frames) {
        if (existing.frameIndex == sh.frameIndex) {
            fa = &existing;
            break;
        }
    }
    if (!fa) {
        if (m_frames.size() >= 8) {
            // Evict oldest incomplete frame
            m_frames.pop_front();
            ++m_stats.framesDropped;
            outNeedKeyframe = true;
        }
        FrameAssembly nf;
        nf.frameIndex = sh.frameIndex;
        nf.frameBytes = sh.frameBytes;
        nf.firstSeenMs = nowMs;
        nf.isKeyframe = (sh.flags & 1) != 0;
        nf.groups.resize(groupCount);
        for (size_t g = 0; g < groupCount; ++g) {
            const uint8_t dk = static_cast<uint8_t>(std::min(
                UdpVideoFecSender::kDefaultGroupDataShards,
                totalShards - g * UdpVideoFecSender::kDefaultGroupDataShards));
            nf.groups[g].dataK = dk;
            nf.groups[g].shardStride = static_cast<uint16_t>(ComputeGroupStride(sh.frameBytes, static_cast<uint16_t>(g), dk));
        }
        m_frames.push_back(std::move(nf));
        fa = &m_frames.back();
    }

    if (fa->frameBytes != sh.frameBytes || fa->groups.size() != groupCount) {
        return false;
    }

    auto& grp = fa->groups[sh.groupIndex];
    if (grp.solved) {
        return false; // Already have all data for this block
    }
    if (grp.shards.empty()) {
        grp.parM = sh.groupParM;
        grp.shards.resize(totalInGroup);
        grp.actualBytes.assign(totalInGroup, 0);
    } else if (grp.parM != sh.groupParM) {
        return false;
    }

    const uint32_t bit = (1u << sh.shardInGroup);
    if (grp.presentMask & bit) {
        return false; // Duplicate packet
    }

    if (sh.shardBytes > grp.shardStride) return false;
    grp.shards[sh.shardInGroup].assign(grp.shardStride, 0);
    if (sh.shardBytes > 0) {
        std::memcpy(grp.shards[sh.shardInGroup].data(), data + sizeof(sh), sh.shardBytes);
    }
    grp.actualBytes[sh.shardInGroup] = sh.shardBytes;
    grp.presentMask |= bit;
    ++grp.receivedCount;

    if (SolveGroup(grp, m_stats.packetsRecovered)) {
        ++fa->groupsSolved;
    }

    if (fa->groupsSolved < fa->groups.size()) {
        return false;
    }

    // All FEC groups in this frame are complete: assemble serialized blob!
    std::vector<uint8_t> blob(fa->frameBytes);
    size_t writeOff = 0;
    for (size_t g = 0; g < fa->groups.size(); ++g) {
        const auto& gr = fa->groups[g];
        for (uint8_t i = 0; i < gr.dataK; ++i) {
            const size_t rem = fa->frameBytes - writeOff;
            const size_t copyBytes = std::min(rem, static_cast<size_t>(gr.shardStride));
            std::memcpy(blob.data() + writeOff, gr.shards[i].data(), copyBytes);
            writeOff += copyBytes;
        }
    }

    if (blob.size() < sizeof(VideoFrameHeader)) return false;
    std::memcpy(&outHeader, blob.data(), sizeof(VideoFrameHeader));
    if (sizeof(VideoFrameHeader) + outHeader.dataSize != blob.size()) {
        return false;
    }
    outPayload.resize(outHeader.dataSize);
    if (outHeader.dataSize > 0) {
        std::memcpy(outPayload.data(), blob.data() + sizeof(VideoFrameHeader), outHeader.dataSize);
    }

    const uint64_t completedIdx = fa->frameIndex;
    m_lastCompletedFrame = completedIdx;
    m_hasCompleted = true;
    ++m_stats.framesCompleted;

    // Purge completed frame AND any older incomplete frames
    bool droppedOld = false;
    for (auto it = m_frames.begin(); it != m_frames.end();) {
        if (it->frameIndex <= completedIdx) {
            if (it->frameIndex < completedIdx) {
                ++m_stats.framesDropped;
                droppedOld = true;
            }
            it = m_frames.erase(it);
        } else {
            ++it;
        }
    }
    if (droppedOld) {
        outNeedKeyframe = true;
    }
    return true;
}

bool UdpVideoFecReceiver::PruneExpired(uint64_t nowMs, uint64_t maxAgeMs) {
    bool droppedAny = false;
    for (auto it = m_frames.begin(); it != m_frames.end();) {
        if (nowMs >= it->firstSeenMs && (nowMs - it->firstSeenMs) >= maxAgeMs) {
            ++m_stats.framesDropped;
            droppedAny = true;
            it = m_frames.erase(it);
        } else {
            ++it;
        }
    }
    return droppedAny;
}

} // namespace s2p
