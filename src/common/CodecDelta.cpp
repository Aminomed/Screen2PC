#include "CodecDelta.h"
#include <algorithm>
#include <cstring>

namespace s2p {

namespace {

constexpr uint32_t kDeltaMagicV1 = 0x444C5431; // "DLT1" (legacy RLE)
constexpr uint32_t kDeltaMagicV2 = 0x444C5432; // "DLT2" (LZ4 + XOR + MoveRects + FastMotion)
constexpr uint32_t kTileSize     = 64;

enum DeltaRectFlags : uint8_t {
    kRectEncodingRawLz4      = 0, // Raw BGRA pixels compressed with LZ4
    kRectEncodingXorLz4      = 1, // XOR against m_prevFrame compressed with LZ4
    kRectEncodingSub2x2Lz4   = 2, // 2x2 subsampled BGRA compressed with LZ4 (fast motion burst)
    kRectEncodingRawUncomp   = 3, // Raw BGRA uncompressed fallback
};

#pragma pack(push, 1)
struct DeltaBlobHeaderV1 {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t rectCount;
    uint8_t  isKeyframe;
    uint8_t  pad[3];
};

struct DeltaRectHeaderV1 {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t compressedBytes;
};

struct DeltaBlobHeaderV2 {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t rectCount;
    uint16_t moveCount;
    uint8_t  isKeyframe;
    uint8_t  flags; // bit 0 = usedFastMotion
};

struct DeltaMoveRectWire {
    uint16_t srcX;
    uint16_t srcY;
    uint16_t dstX;
    uint16_t dstY;
    uint16_t w;
    uint16_t h;
};

struct DeltaRectHeaderV2 {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint8_t  encoding; // DeltaRectFlags
    uint8_t  pad[3];
    uint32_t uncompressedBytes;
    uint32_t compressedBytes;
};
#pragma pack(pop)

// ============================================================================
// Self-contained Fast LZ4 Block Compressor & Decompressor (bounds-checked)
// ============================================================================
inline uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint32_t HashLz4(uint32_t val) {
    return (val * 2654435761U) >> (32 - 14); // 16K hash table entries
}

void CompressLz4Block(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out) {
    out.clear();
    if (srcSize == 0) return;

    out.reserve(srcSize + (srcSize / 255) + 16);

    if (srcSize < 16) {
        uint8_t token = static_cast<uint8_t>(srcSize << 4);
        out.push_back(token);
        out.insert(out.end(), src, src + srcSize);
        return;
    }

    uint32_t hashTable[1 << 14] = {};
    const uint8_t* const srcEnd = src + srcSize;
    const uint8_t* const matchLimit = srcEnd - 5;
    const uint8_t* const mflimit = srcEnd - 12;

    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    ++ip;

    while (ip < mflimit) {
        const uint8_t* match = nullptr;
        uint32_t step = 1;
        uint32_t searchMatchNb = 1U << 6;

        do {
            uint32_t h = HashLz4(ReadU32(ip));
            uint32_t refIdx = hashTable[h];
            hashTable[h] = static_cast<uint32_t>(ip - src);
            match = src + refIdx;

            if (refIdx < static_cast<uint32_t>(ip - src) &&
                (ip - match) <= 0xFFFF &&
                ReadU32(match) == ReadU32(ip)) {
                break;
            }
            ip += step;
            step = (searchMatchNb++ >> 6);
        } while (ip < mflimit);

        if (ip >= mflimit) break;

        // Expand match backwards if possible
        while (ip > anchor && match > src && ip[-1] == match[-1]) {
            --ip;
            --match;
        }

        size_t litLen = static_cast<size_t>(ip - anchor);
        size_t tokenIdx = out.size();
        out.push_back(0); // placeholder for token

        if (litLen >= 15) {
            size_t rem = litLen - 15;
            while (rem >= 255) {
                out.push_back(255);
                rem -= 255;
            }
            out.push_back(static_cast<uint8_t>(rem));
        }
        out.insert(out.end(), anchor, anchor + litLen);

        uint16_t offset = static_cast<uint16_t>(ip - match);
        out.push_back(static_cast<uint8_t>(offset & 0xFF));
        out.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));

        ip += 4;
        match += 4;
        while (ip < matchLimit && *ip == *match) {
            ++ip;
            ++match;
        }
        size_t matchLen = static_cast<size_t>(ip - (anchor + litLen) - 4);
        uint8_t token = static_cast<uint8_t>((std::min<size_t>(litLen, 15) << 4) | std::min<size_t>(matchLen, 15));
        out[tokenIdx] = token;

        if (matchLen >= 15) {
            size_t rem = matchLen - 15;
            while (rem >= 255) {
                out.push_back(255);
                rem -= 255;
            }
            out.push_back(static_cast<uint8_t>(rem));
        }

        anchor = ip;
        if (ip < mflimit) {
            hashTable[HashLz4(ReadU32(ip - 2))] = static_cast<uint32_t>(ip - 2 - src);
        }
    }

    // Emit trailing literals
    size_t lastLit = static_cast<size_t>(srcEnd - anchor);
    uint8_t token = static_cast<uint8_t>(std::min<size_t>(lastLit, 15) << 4);
    out.push_back(token);
    if (lastLit >= 15) {
        size_t rem = lastLit - 15;
        while (rem >= 255) {
            out.push_back(255);
            rem -= 255;
        }
        out.push_back(static_cast<uint8_t>(rem));
    }
    out.insert(out.end(), anchor, srcEnd);
}

bool DecompressLz4Block(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t expectedDstSize) {
    if (expectedDstSize == 0) return srcSize == 0;
    const uint8_t* ip = src;
    const uint8_t* const ipEnd = src + srcSize;
    uint8_t* op = dst;
    uint8_t* const opEnd = dst + expectedDstSize;

    while (ip < ipEnd) {
        uint8_t token = *ip++;
        size_t litLen = token >> 4;
        if (litLen == 15) {
            uint8_t s = 0;
            do {
                if (ip >= ipEnd) return false;
                s = *ip++;
                litLen += s;
            } while (s == 255);
        }

        if (litLen > static_cast<size_t>(ipEnd - ip) ||
            litLen > static_cast<size_t>(opEnd - op)) {
            return false;
        }
        if (litLen > 0) {
            std::memcpy(op, ip, litLen);
            ip += litLen;
            op += litLen;
        }

        if (ip >= ipEnd) break;

        if (ipEnd - ip < 2) return false;
        uint16_t offset = static_cast<uint16_t>(ip[0] | (static_cast<uint16_t>(ip[1]) << 8));
        ip += 2;
        if (offset == 0 || offset > static_cast<size_t>(op - dst)) {
            return false;
        }

        size_t matchLen = (token & 0x0F) + 4;
        if ((token & 0x0F) == 15) {
            uint8_t s = 0;
            do {
                if (ip >= ipEnd) return false;
                s = *ip++;
                matchLen += s;
            } while (s == 255);
        }

        if (matchLen > static_cast<size_t>(opEnd - op)) {
            return false;
        }

        const uint8_t* match = op - offset;
        if (offset >= matchLen) {
            std::memcpy(op, match, matchLen);
            op += matchLen;
        } else if (offset == 4) {
            uint32_t pat;
            std::memcpy(&pat, match, 4);
            while (matchLen >= 4) {
                std::memcpy(op, &pat, 4);
                op += 4;
                matchLen -= 4;
            }
            while (matchLen-- > 0) {
                *op++ = *match++;
            }
        } else {
            while (matchLen-- > 0) {
                *op++ = *match++;
            }
        }
    }

    return op == opEnd && ip == ipEnd;
}

// Legacy V1 RLE decompressor for backward compatibility
bool DecompressPixelsRle(const uint8_t* src, size_t srcBytes, uint32_t* dst, size_t expectedPixels) {
    size_t pos = 0;
    size_t outIdx = 0;
    while (pos < srcBytes && outIdx < expectedPixels) {
        uint8_t ctrl = src[pos++];
        size_t count = static_cast<size_t>(ctrl & 0x7F) + 1;
        if (outIdx + count > expectedPixels) return false;
        if (ctrl & 0x80) {
            if (pos + 4 > srcBytes) return false;
            uint32_t px = 0;
            std::memcpy(&px, src + pos, 4);
            pos += 4;
            for (size_t k = 0; k < count; ++k) {
                dst[outIdx++] = px;
            }
        } else {
            size_t bytesNeeded = count * 4;
            if (pos + bytesNeeded > srcBytes) return false;
            std::memcpy(&dst[outIdx], src + pos, bytesNeeded);
            pos += bytesNeeded;
            outIdx += count;
        }
    }
    return outIdx == expectedPixels && pos == srcBytes;
}

void ApplyMoveRectInPlace(uint8_t* frame, uint32_t width, uint32_t height,
                          const DeltaMoveRect& mv, std::vector<uint8_t>& scratch) {
    if (mv.w == 0 || mv.h == 0) return;
    if (static_cast<uint64_t>(mv.srcX) + mv.w > width ||
        static_cast<uint64_t>(mv.srcY) + mv.h > height ||
        static_cast<uint64_t>(mv.dstX) + mv.w > width ||
        static_cast<uint64_t>(mv.dstY) + mv.h > height) {
        return;
    }
    if (mv.srcX == mv.dstX && mv.srcY == mv.dstY) return;

    size_t rowBytes = static_cast<size_t>(mv.w) * 4;
    scratch.resize(rowBytes * mv.h);
    for (uint32_t r = 0; r < mv.h; ++r) {
        const uint8_t* s = frame + (static_cast<size_t>(mv.srcY + r) * width + mv.srcX) * 4;
        std::memcpy(scratch.data() + static_cast<size_t>(r) * rowBytes, s, rowBytes);
    }
    for (uint32_t r = 0; r < mv.h; ++r) {
        uint8_t* d = frame + (static_cast<size_t>(mv.dstY + r) * width + mv.dstX) * 4;
        std::memcpy(d, scratch.data() + static_cast<size_t>(r) * rowBytes, rowBytes);
    }
}

} // namespace

std::vector<DeltaRect> DeltaEncoder::MergeAndBoundRects(const std::vector<DeltaRect>& rects,
                                                        uint32_t frameW,
                                                        uint32_t frameH,
                                                        size_t maxRects) {
    std::vector<DeltaRect> out;
    out.reserve(std::min<size_t>(rects.size(), maxRects));

    for (const auto& r : rects) {
        uint32_t x0 = std::min(r.x, frameW);
        uint32_t y0 = std::min(r.y, frameH);
        uint32_t x1 = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(r.x) + r.w, frameW));
        uint32_t y1 = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(r.y) + r.h, frameH));
        if (x1 <= x0 || y1 <= y0) continue;

        DeltaRect cur{x0, y0, x1 - x0, y1 - y0};
        bool merged = false;
        for (auto& ex : out) {
            uint32_t exR = ex.x + ex.w;
            uint32_t exB = ex.y + ex.h;
            uint32_t curR = cur.x + cur.w;
            uint32_t curB = cur.y + cur.h;

            // Merge if overlapping or within 16 px and union doesn't bloat area excessively
            if (cur.x <= exR + 16 && curR + 16 >= ex.x &&
                cur.y <= exB + 16 && curB + 16 >= ex.y) {
                uint32_t ux0 = std::min(ex.x, cur.x);
                uint32_t uy0 = std::min(ex.y, cur.y);
                uint32_t ux1 = std::max(exR, curR);
                uint32_t uy1 = std::max(exB, curB);
                uint64_t unionArea = static_cast<uint64_t>(ux1 - ux0) * (uy1 - uy0);
                uint64_t sumArea = static_cast<uint64_t>(ex.w) * ex.h + static_cast<uint64_t>(cur.w) * cur.h;
                if (unionArea <= sumArea + 64 * 64 || out.size() >= maxRects) {
                    ex = {ux0, uy0, ux1 - ux0, uy1 - uy0};
                    merged = true;
                    break;
                }
            }
        }
        if (!merged) {
            out.push_back(cur);
        }
    }

    // If still above maxRects, iteratively merge the pair with minimal area increase
    while (out.size() > maxRects && maxRects > 0) {
        size_t bestI = 0, bestJ = 1;
        uint64_t bestCost = UINT64_MAX;
        for (size_t i = 0; i < out.size(); ++i) {
            for (size_t j = i + 1; j < out.size(); ++j) {
                uint32_t ux0 = std::min(out[i].x, out[j].x);
                uint32_t uy0 = std::min(out[i].y, out[j].y);
                uint32_t ux1 = std::max(out[i].x + out[i].w, out[j].x + out[j].w);
                uint32_t uy1 = std::max(out[i].y + out[i].h, out[j].y + out[j].h);
                uint64_t unionArea = static_cast<uint64_t>(ux1 - ux0) * (uy1 - uy0);
                uint64_t sumArea = static_cast<uint64_t>(out[i].w) * out[i].h + static_cast<uint64_t>(out[j].w) * out[j].h;
                uint64_t cost = (unionArea > sumArea) ? (unionArea - sumArea) : 0;
                if (cost < bestCost) {
                    bestCost = cost;
                    bestI = i;
                    bestJ = j;
                }
            }
        }
        uint32_t ux0 = std::min(out[bestI].x, out[bestJ].x);
        uint32_t uy0 = std::min(out[bestI].y, out[bestJ].y);
        uint32_t ux1 = std::max(out[bestI].x + out[bestI].w, out[bestJ].x + out[bestJ].w);
        uint32_t uy1 = std::max(out[bestI].y + out[bestI].h, out[bestJ].y + out[bestJ].h);
        out[bestI] = {ux0, uy0, ux1 - ux0, uy1 - uy0};
        out.erase(out.begin() + static_cast<ptrdiff_t>(bestJ));
    }

    return out;
}

void DeltaEncoder::Reset() {
    m_width = 0;
    m_height = 0;
    m_prevFrame.clear();
}

bool DeltaEncoder::Encode(const uint8_t* bgraFrame,
                          uint32_t width,
                          uint32_t height,
                          uint32_t strideBytes,
                          const std::vector<DeltaRect>& hints,
                          bool forceKeyframe,
                          std::vector<uint8_t>& outPayload,
                          bool& outIsKeyframe,
                          const std::vector<DeltaMoveRect>& moveRects,
                          bool allowFastMotion,
                          bool* outUsedFastMotion) {
    if (outUsedFastMotion) *outUsedFastMotion = false;
    if (!bgraFrame || width == 0 || height == 0 ||
        width > 16384 || height > 16384 ||
        static_cast<uint64_t>(width) * height > 64ULL * 1024ULL * 1024ULL ||
        static_cast<uint64_t>(strideBytes) < static_cast<uint64_t>(width) * 4) {
        return false;
    }

    const bool dimChanged = (width != m_width || height != m_height ||
                             m_prevFrame.size() != static_cast<size_t>(width) * height * 4);
    outIsKeyframe = forceKeyframe || dimChanged;

    if (dimChanged) {
        m_width = width;
        m_height = height;
        m_prevFrame.assign(static_cast<size_t>(width) * height * 4, 0);
    }

    // Apply moveRects to our internal m_prevFrame first (unless keyframe)
    std::vector<DeltaMoveRect> validMoves;
    if (!outIsKeyframe && !moveRects.empty()) {
        validMoves.reserve(std::min<size_t>(moveRects.size(), 256));
        for (const auto& mv : moveRects) {
            if (mv.w == 0 || mv.h == 0) continue;
            if (static_cast<uint64_t>(mv.srcX) + mv.w <= width &&
                static_cast<uint64_t>(mv.srcY) + mv.h <= height &&
                static_cast<uint64_t>(mv.dstX) + mv.w <= width &&
                static_cast<uint64_t>(mv.dstY) + mv.h <= height) {
                ApplyMoveRectInPlace(m_prevFrame.data(), width, height, mv, m_scratchMove);
                validMoves.push_back(mv);
                if (validMoves.size() >= 256) break;
            }
        }
    }

    std::vector<DeltaRect> rects;
    if (outIsKeyframe) {
        rects.push_back({0, 0, width, height});
    } else if (!hints.empty()) {
        // Refine hints with 64x64 tile check against m_prevFrame so unchanged sub-regions are skipped!
        std::vector<DeltaRect> mergedHints = MergeAndBoundRects(hints, width, height, 32);
        std::vector<DeltaRect> dirtyTiles;
        for (const auto& hr : mergedHints) {
            uint32_t txStart = (hr.x / kTileSize) * kTileSize;
            uint32_t tyStart = (hr.y / kTileSize) * kTileSize;
            uint32_t txEnd = std::min(width, hr.x + hr.w);
            uint32_t tyEnd = std::min(height, hr.y + hr.h);
            for (uint32_t ty = tyStart; ty < tyEnd; ty += kTileSize) {
                uint32_t th = std::min(kTileSize, tyEnd - ty);
                for (uint32_t tx = txStart; tx < txEnd; tx += kTileSize) {
                    uint32_t tw = std::min(kTileSize, txEnd - tx);
                    bool dirty = false;
                    for (uint32_t row = 0; row < th; ++row) {
                        const uint8_t* curRow = bgraFrame + static_cast<size_t>(ty + row) * strideBytes + static_cast<size_t>(tx) * 4;
                        const uint8_t* prevRow = m_prevFrame.data() + (static_cast<size_t>(ty + row) * width + tx) * 4;
                        if (std::memcmp(curRow, prevRow, static_cast<size_t>(tw) * 4) != 0) {
                            dirty = true;
                            break;
                        }
                    }
                    if (dirty) {
                        dirtyTiles.push_back({tx, ty, tw, th});
                    }
                }
            }
        }
        rects = MergeAndBoundRects(dirtyTiles, width, height, 32);
    } else {
        std::vector<DeltaRect> dirtyTiles;
        for (uint32_t ty = 0; ty < height; ty += kTileSize) {
            uint32_t th = std::min(kTileSize, height - ty);
            for (uint32_t tx = 0; tx < width; tx += kTileSize) {
                uint32_t tw = std::min(kTileSize, width - tx);
                bool dirty = false;
                for (uint32_t row = 0; row < th; ++row) {
                    const uint8_t* curRow = bgraFrame + static_cast<size_t>(ty + row) * strideBytes + static_cast<size_t>(tx) * 4;
                    const uint8_t* prevRow = m_prevFrame.data() + (static_cast<size_t>(ty + row) * width + tx) * 4;
                    if (std::memcmp(curRow, prevRow, static_cast<size_t>(tw) * 4) != 0) {
                        dirty = true;
                        break;
                    }
                }
                if (dirty) {
                    dirtyTiles.push_back({tx, ty, tw, th});
                }
            }
        }
        rects = MergeAndBoundRects(dirtyTiles, width, height, 32);
    }

    uint64_t dirtyArea = 0;
    for (const auto& r : rects) {
        dirtyArea += static_cast<uint64_t>(r.w) * r.h;
    }
    const uint64_t totalArea = static_cast<uint64_t>(width) * height;
    const bool useFastMotion = (!outIsKeyframe && allowFastMotion &&
                                totalArea > 0 && (dirtyArea * 100ULL >= totalArea * 35ULL));
    if (outUsedFastMotion) *outUsedFastMotion = useFastMotion;

    outPayload.clear();
    outPayload.resize(sizeof(DeltaBlobHeaderV2) + validMoves.size() * sizeof(DeltaMoveRectWire));

    for (size_t i = 0; i < validMoves.size(); ++i) {
        DeltaMoveRectWire mw{};
        mw.srcX = static_cast<uint16_t>(validMoves[i].srcX);
        mw.srcY = static_cast<uint16_t>(validMoves[i].srcY);
        mw.dstX = static_cast<uint16_t>(validMoves[i].dstX);
        mw.dstY = static_cast<uint16_t>(validMoves[i].dstY);
        mw.w    = static_cast<uint16_t>(validMoves[i].w);
        mw.h    = static_cast<uint16_t>(validMoves[i].h);
        std::memcpy(outPayload.data() + sizeof(DeltaBlobHeaderV2) + i * sizeof(DeltaMoveRectWire), &mw, sizeof(mw));
    }

    for (const auto& r : rects) {
        const size_t fullBytes = static_cast<size_t>(r.w) * r.h * 4;
        DeltaRectFlags encoding = outIsKeyframe ? kRectEncodingRawLz4 : kRectEncodingXorLz4;
        size_t uncompBytes = fullBytes;

        if (useFastMotion && r.w >= 4 && r.h >= 4) {
            encoding = kRectEncodingSub2x2Lz4;
            uint32_t sw = (r.w + 1) / 2;
            uint32_t sh = (r.h + 1) / 2;
            uncompBytes = static_cast<size_t>(sw) * sh * 4;
            m_scratchRaw.resize(uncompBytes);
            uint32_t* dstPx = reinterpret_cast<uint32_t*>(m_scratchRaw.data());

            for (uint32_t sy = 0; sy < sh; ++sy) {
                uint32_t y0 = std::min(r.h - 1, sy * 2);
                const uint32_t* srcRow0 = reinterpret_cast<const uint32_t*>(
                    bgraFrame + static_cast<size_t>(r.y + y0) * strideBytes + static_cast<size_t>(r.x) * 4);
                for (uint32_t sx = 0; sx < sw; ++sx) {
                    uint32_t x0 = std::min(r.w - 1, sx * 2);
                    dstPx[static_cast<size_t>(sy) * sw + sx] = srcRow0[x0];
                }
            }

            // Reconstruct the exact 2x2-expanded pixels into m_prevFrame so encoder & decoder stay 100% in sync!
            for (uint32_t row = 0; row < r.h; ++row) {
                uint32_t* dstPrev = reinterpret_cast<uint32_t*>(
                    m_prevFrame.data() + (static_cast<size_t>(r.y + row) * width + r.x) * 4);
                const uint32_t* subRow = dstPx + static_cast<size_t>(row / 2) * sw;
                for (uint32_t col = 0; col < r.w; ++col) {
                    dstPrev[col] = subRow[col / 2];
                }
            }
        } else if (encoding == kRectEncodingXorLz4) {
            m_scratchRaw.resize(fullBytes);
            for (uint32_t row = 0; row < r.h; ++row) {
                const uint8_t* srcRow = bgraFrame + static_cast<size_t>(r.y + row) * strideBytes + static_cast<size_t>(r.x) * 4;
                uint8_t* prevRow = m_prevFrame.data() + (static_cast<size_t>(r.y + row) * width + r.x) * 4;
                uint8_t* xorRow = m_scratchRaw.data() + static_cast<size_t>(row) * r.w * 4;

                size_t rowBytes = static_cast<size_t>(r.w) * 4;
                size_t b = 0;
                for (; b + 8 <= rowBytes; b += 8) {
                    uint64_t sv, pv;
                    std::memcpy(&sv, srcRow + b, 8);
                    std::memcpy(&pv, prevRow + b, 8);
                    uint64_t xv = sv ^ pv;
                    std::memcpy(xorRow + b, &xv, 8);
                    std::memcpy(prevRow + b, &sv, 8);
                }
                for (; b < rowBytes; ++b) {
                    xorRow[b] = srcRow[b] ^ prevRow[b];
                    prevRow[b] = srcRow[b];
                }
            }
        } else {
            m_scratchRaw.resize(fullBytes);
            for (uint32_t row = 0; row < r.h; ++row) {
                const uint8_t* srcRow = bgraFrame + static_cast<size_t>(r.y + row) * strideBytes + static_cast<size_t>(r.x) * 4;
                uint8_t* prevRow = m_prevFrame.data() + (static_cast<size_t>(r.y + row) * width + r.x) * 4;
                uint8_t* rawRow = m_scratchRaw.data() + static_cast<size_t>(row) * r.w * 4;
                std::memcpy(prevRow, srcRow, static_cast<size_t>(r.w) * 4);
                std::memcpy(rawRow, srcRow, static_cast<size_t>(r.w) * 4);
            }
        }

        CompressLz4Block(m_scratchRaw.data(), uncompBytes, m_scratchCompressed);
        const uint8_t* payloadData = m_scratchCompressed.data();
        size_t payloadLen = m_scratchCompressed.size();

        if (payloadLen >= uncompBytes && encoding == kRectEncodingRawLz4) {
            encoding = kRectEncodingRawUncomp;
            payloadData = m_scratchRaw.data();
            payloadLen = uncompBytes;
        }

        DeltaRectHeaderV2 rh{};
        rh.x = static_cast<uint16_t>(r.x);
        rh.y = static_cast<uint16_t>(r.y);
        rh.w = static_cast<uint16_t>(r.w);
        rh.h = static_cast<uint16_t>(r.h);
        rh.encoding = static_cast<uint8_t>(encoding);
        rh.uncompressedBytes = static_cast<uint32_t>(uncompBytes);
        rh.compressedBytes = static_cast<uint32_t>(payloadLen);

        size_t offset = outPayload.size();
        outPayload.resize(offset + sizeof(DeltaRectHeaderV2) + payloadLen);
        std::memcpy(outPayload.data() + offset, &rh, sizeof(rh));
        if (payloadLen > 0) {
            std::memcpy(outPayload.data() + offset + sizeof(rh), payloadData, payloadLen);
        }
    }

    DeltaBlobHeaderV2 bh{};
    bh.magic = kDeltaMagicV2;
    bh.width = width;
    bh.height = height;
    bh.rectCount = static_cast<uint32_t>(rects.size());
    bh.moveCount = static_cast<uint16_t>(validMoves.size());
    bh.isKeyframe = outIsKeyframe ? 1 : 0;
    bh.flags = useFastMotion ? 1 : 0;
    std::memcpy(outPayload.data(), &bh, sizeof(bh));

    return true;
}

void DeltaDecoder::Reset() {
    m_width = 0;
    m_height = 0;
    m_frame.clear();
    m_lastBounds = {0, 0, 0, 0};
    m_lastFastMotion = false;
}

bool DeltaDecoder::Decode(const uint8_t* payload,
                          size_t payloadSize,
                          uint32_t expectedWidth,
                          uint32_t expectedHeight) {
    if (!payload || payloadSize < sizeof(uint32_t)) return false;

    uint32_t magic = 0;
    std::memcpy(&magic, payload, sizeof(uint32_t));

    if (magic == kDeltaMagicV1) {
        if (payloadSize < sizeof(DeltaBlobHeaderV1)) return false;
        DeltaBlobHeaderV1 bh{};
        std::memcpy(&bh, payload, sizeof(bh));
        if (bh.width == 0 || bh.height == 0 ||
            bh.width > 16384 || bh.height > 16384 ||
            static_cast<uint64_t>(bh.width) * bh.height > 64ULL * 1024ULL * 1024ULL) {
            return false;
        }
        if (expectedWidth != 0 && (bh.width != expectedWidth || bh.height != expectedHeight)) {
            return false;
        }
        if (m_width != bh.width || m_height != bh.height || m_frame.size() != static_cast<size_t>(bh.width) * bh.height * 4) {
            m_width = bh.width;
            m_height = bh.height;
            m_frame.assign(static_cast<size_t>(m_width) * m_height * 4, 0);
        }
        size_t pos = sizeof(DeltaBlobHeaderV1);
        if (static_cast<uint64_t>(bh.rectCount) * sizeof(DeltaRectHeaderV1) > payloadSize - pos) {
            return false;
        }
        std::vector<uint32_t> linearPixels;
        uint32_t minX = m_width, minY = m_height, maxX = 0, maxY = 0;
        for (uint32_t i = 0; i < bh.rectCount; ++i) {
            if (payloadSize - pos < sizeof(DeltaRectHeaderV1)) return false;
            DeltaRectHeaderV1 rh{};
            std::memcpy(&rh, payload + pos, sizeof(rh));
            pos += sizeof(rh);
            if (rh.compressedBytes > payloadSize - pos || rh.w == 0 || rh.h == 0) return false;
            if (static_cast<uint64_t>(rh.x) + rh.w > m_width ||
                static_cast<uint64_t>(rh.y) + rh.h > m_height) {
                return false;
            }
            size_t pxCount = static_cast<size_t>(rh.w) * rh.h;
            linearPixels.resize(pxCount);
            if (!DecompressPixelsRle(payload + pos, rh.compressedBytes, linearPixels.data(), pxCount)) {
                return false;
            }
            pos += rh.compressedBytes;
            for (uint32_t row = 0; row < rh.h; ++row) {
                uint8_t* dstRow = m_frame.data() + (static_cast<size_t>(rh.y + row) * m_width + rh.x) * 4;
                std::memcpy(dstRow, &linearPixels[static_cast<size_t>(row) * rh.w], static_cast<size_t>(rh.w) * 4);
            }
            minX = std::min(minX, rh.x);
            minY = std::min(minY, rh.y);
            maxX = std::max(maxX, rh.x + rh.w);
            maxY = std::max(maxY, rh.y + rh.h);
        }
        if (maxX > minX && maxY > minY) {
            m_lastBounds = {minX, minY, maxX - minX, maxY - minY};
        } else {
            m_lastBounds = {0, 0, 0, 0};
        }
        m_lastFastMotion = false;
        return pos == payloadSize;
    }

    if (magic != kDeltaMagicV2 || payloadSize < sizeof(DeltaBlobHeaderV2)) {
        return false;
    }

    DeltaBlobHeaderV2 bh{};
    std::memcpy(&bh, payload, sizeof(bh));
    if (bh.width == 0 || bh.height == 0 ||
        bh.width > 16384 || bh.height > 16384 ||
        static_cast<uint64_t>(bh.width) * bh.height > 64ULL * 1024ULL * 1024ULL) {
        return false;
    }
    if (expectedWidth != 0 && (bh.width != expectedWidth || bh.height != expectedHeight)) {
        return false;
    }

    if (m_width != bh.width || m_height != bh.height || m_frame.size() != static_cast<size_t>(bh.width) * bh.height * 4) {
        m_width = bh.width;
        m_height = bh.height;
        m_frame.assign(static_cast<size_t>(m_width) * m_height * 4, 0);
    }

    size_t pos = sizeof(DeltaBlobHeaderV2);
    size_t movesBytes = static_cast<size_t>(bh.moveCount) * sizeof(DeltaMoveRectWire);
    if (movesBytes > payloadSize - pos) return false;

    uint32_t minX = m_width, minY = m_height, maxX = 0, maxY = 0;

    for (uint16_t m = 0; m < bh.moveCount; ++m) {
        DeltaMoveRectWire mw{};
        std::memcpy(&mw, payload + pos, sizeof(mw));
        pos += sizeof(mw);
        if (mw.w == 0 || mw.h == 0 ||
            static_cast<uint64_t>(mw.srcX) + mw.w > m_width ||
            static_cast<uint64_t>(mw.srcY) + mw.h > m_height ||
            static_cast<uint64_t>(mw.dstX) + mw.w > m_width ||
            static_cast<uint64_t>(mw.dstY) + mw.h > m_height) {
            return false;
        }
        DeltaMoveRect mv{mw.srcX, mw.srcY, mw.dstX, mw.dstY, mw.w, mw.h};
        ApplyMoveRectInPlace(m_frame.data(), m_width, m_height, mv, m_scratchMove);
        minX = std::min<uint32_t>(minX, mw.dstX);
        minY = std::min<uint32_t>(minY, mw.dstY);
        maxX = std::max<uint32_t>(maxX, static_cast<uint32_t>(mw.dstX) + mw.w);
        maxY = std::max<uint32_t>(maxY, static_cast<uint32_t>(mw.dstY) + mw.h);
    }

    if (static_cast<uint64_t>(bh.rectCount) * sizeof(DeltaRectHeaderV2) > payloadSize - pos) {
        return false;
    }

    for (uint32_t i = 0; i < bh.rectCount; ++i) {
        if (payloadSize - pos < sizeof(DeltaRectHeaderV2)) return false;
        DeltaRectHeaderV2 rh{};
        std::memcpy(&rh, payload + pos, sizeof(rh));
        pos += sizeof(rh);

        if (rh.compressedBytes > payloadSize - pos || rh.w == 0 || rh.h == 0) return false;
        if (static_cast<uint64_t>(rh.x) + rh.w > m_width ||
            static_cast<uint64_t>(rh.y) + rh.h > m_height) {
            return false;
        }

        const size_t fullRectBytes = static_cast<size_t>(rh.w) * rh.h * 4;
        const uint8_t* compPtr = payload + pos;
        pos += rh.compressedBytes;

        if (rh.encoding == kRectEncodingRawUncomp) {
            if (rh.compressedBytes != fullRectBytes || rh.uncompressedBytes != fullRectBytes) return false;
            for (uint32_t row = 0; row < rh.h; ++row) {
                uint8_t* dstRow = m_frame.data() + (static_cast<size_t>(rh.y + row) * m_width + rh.x) * 4;
                std::memcpy(dstRow, compPtr + static_cast<size_t>(row) * rh.w * 4, static_cast<size_t>(rh.w) * 4);
            }
        } else if (rh.encoding == kRectEncodingRawLz4 || rh.encoding == kRectEncodingXorLz4) {
            if (rh.uncompressedBytes != fullRectBytes) return false;
            m_scratchDecomp.resize(fullRectBytes);
            if (!DecompressLz4Block(compPtr, rh.compressedBytes, m_scratchDecomp.data(), fullRectBytes)) {
                return false;
            }
            if (rh.encoding == kRectEncodingRawLz4) {
                for (uint32_t row = 0; row < rh.h; ++row) {
                    uint8_t* dstRow = m_frame.data() + (static_cast<size_t>(rh.y + row) * m_width + rh.x) * 4;
                    std::memcpy(dstRow, m_scratchDecomp.data() + static_cast<size_t>(row) * rh.w * 4, static_cast<size_t>(rh.w) * 4);
                }
            } else {
                for (uint32_t row = 0; row < rh.h; ++row) {
                    uint8_t* dstRow = m_frame.data() + (static_cast<size_t>(rh.y + row) * m_width + rh.x) * 4;
                    const uint8_t* xorRow = m_scratchDecomp.data() + static_cast<size_t>(row) * rh.w * 4;
                    size_t rowBytes = static_cast<size_t>(rh.w) * 4;
                    size_t b = 0;
                    for (; b + 8 <= rowBytes; b += 8) {
                        uint64_t dv, xv;
                        std::memcpy(&dv, dstRow + b, 8);
                        std::memcpy(&xv, xorRow + b, 8);
                        dv ^= xv;
                        std::memcpy(dstRow + b, &dv, 8);
                    }
                    for (; b < rowBytes; ++b) {
                        dstRow[b] ^= xorRow[b];
                    }
                }
            }
        } else if (rh.encoding == kRectEncodingSub2x2Lz4) {
            uint32_t sw = (rh.w + 1) / 2;
            uint32_t sh = (rh.h + 1) / 2;
            size_t subBytes = static_cast<size_t>(sw) * sh * 4;
            if (rh.uncompressedBytes != subBytes) return false;
            m_scratchDecomp.resize(subBytes);
            if (!DecompressLz4Block(compPtr, rh.compressedBytes, m_scratchDecomp.data(), subBytes)) {
                return false;
            }
            const uint32_t* subPx = reinterpret_cast<const uint32_t*>(m_scratchDecomp.data());
            for (uint32_t row = 0; row < rh.h; ++row) {
                uint32_t* dstRow = reinterpret_cast<uint32_t*>(
                    m_frame.data() + (static_cast<size_t>(rh.y + row) * m_width + rh.x) * 4);
                const uint32_t* subRow = subPx + static_cast<size_t>(row / 2) * sw;
                for (uint32_t col = 0; col < rh.w; ++col) {
                    dstRow[col] = subRow[col / 2];
                }
            }
        } else {
            return false;
        }

        minX = std::min<uint32_t>(minX, rh.x);
        minY = std::min<uint32_t>(minY, rh.y);
        maxX = std::max<uint32_t>(maxX, static_cast<uint32_t>(rh.x) + rh.w);
        maxY = std::max<uint32_t>(maxY, static_cast<uint32_t>(rh.y) + rh.h);
    }

    if (maxX > minX && maxY > minY) {
        m_lastBounds = {minX, minY, maxX - minX, maxY - minY};
    } else {
        m_lastBounds = {0, 0, 0, 0};
    }
    m_lastFastMotion = (bh.flags & 1) != 0;
    return pos == payloadSize;
}

} // namespace s2p
