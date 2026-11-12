#include "CodecDelta.h"
#include <algorithm>
#include <cstring>

namespace s2p {

namespace {

constexpr uint32_t kDeltaMagic = 0x444C5431; // "DLT1"
constexpr uint32_t kTileSize   = 64;

#pragma pack(push, 1)
struct DeltaBlobHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t rectCount;
    uint8_t  isKeyframe;
    uint8_t  pad[3];
};

struct DeltaRectHeader {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t compressedBytes;
};
#pragma pack(pop)

// Fast 32-bit pixel RLE:
// Control byte:
//   bit 7 == 1 -> repeat next uint32_t pixel ((ctrl & 0x7F) + 1) times (max 128)
//   bit 7 == 0 -> literal run of ((ctrl & 0x7F) + 1) uint32_t pixels (max 128)
void CompressPixelsRle(const uint32_t* src, size_t pixelCount, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < pixelCount) {
        // Check if we have a run of >= 2 identical pixels
        size_t run = 1;
        while (i + run < pixelCount && run < 128 && src[i + run] == src[i]) {
            ++run;
        }
        if (run >= 2) {
            uint8_t ctrl = static_cast<uint8_t>(0x80 | (run - 1));
            out.push_back(ctrl);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&src[i]);
            out.insert(out.end(), p, p + 4);
            i += run;
        } else {
            size_t litStart = i;
            size_t litLen = 0;
            while (i < pixelCount && litLen < 128) {
                if (i + 1 < pixelCount && src[i] == src[i + 1]) {
                    // Break before a repeat of >= 2
                    if (litLen > 0) break;
                }
                ++i;
                ++litLen;
            }
            uint8_t ctrl = static_cast<uint8_t>(litLen - 1);
            out.push_back(ctrl);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&src[litStart]);
            out.insert(out.end(), p, p + litLen * 4);
        }
    }
}

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

} // namespace

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
                          bool& outIsKeyframe) {
    if (!bgraFrame || width == 0 || height == 0 || strideBytes < width * 4) {
        return false;
    }

    const bool dimChanged = (width != m_width || height != m_height || m_prevFrame.size() != static_cast<size_t>(width) * height * 4);
    outIsKeyframe = forceKeyframe || dimChanged;

    if (dimChanged) {
        m_width = width;
        m_height = height;
        m_prevFrame.resize(static_cast<size_t>(width) * height * 4);
    }

    std::vector<DeltaRect> rects;
    if (outIsKeyframe) {
        rects.push_back({0, 0, width, height});
    } else if (!hints.empty()) {
        for (const auto& r : hints) {
            uint32_t x0 = std::min(r.x, width);
            uint32_t y0 = std::min(r.y, height);
            uint32_t x1 = std::min(r.x + r.w, width);
            uint32_t y1 = std::min(r.y + r.h, height);
            if (x1 > x0 && y1 > y0) {
                rects.push_back({x0, y0, x1 - x0, y1 - y0});
            }
        }
    } else {
        // Tile scan 64x64 against previous frame
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
                    rects.push_back({tx, ty, tw, th});
                }
            }
        }
    }

    outPayload.clear();
    outPayload.resize(sizeof(DeltaBlobHeader));

    std::vector<uint32_t> linearPixels;
    std::vector<uint8_t> compressed;

    for (const auto& r : rects) {
        linearPixels.resize(static_cast<size_t>(r.w) * r.h);
        for (uint32_t row = 0; row < r.h; ++row) {
            const uint8_t* srcRow = bgraFrame + static_cast<size_t>(r.y + row) * strideBytes + static_cast<size_t>(r.x) * 4;
            uint8_t* dstPrev = m_prevFrame.data() + (static_cast<size_t>(r.y + row) * width + r.x) * 4;
            std::memcpy(dstPrev, srcRow, static_cast<size_t>(r.w) * 4);
            std::memcpy(&linearPixels[static_cast<size_t>(row) * r.w], srcRow, static_cast<size_t>(r.w) * 4);
        }

        compressed.clear();
        compressed.reserve(linearPixels.size() * 2);
        CompressPixelsRle(linearPixels.data(), linearPixels.size(), compressed);

        DeltaRectHeader rh{};
        rh.x = r.x;
        rh.y = r.y;
        rh.w = r.w;
        rh.h = r.h;
        rh.compressedBytes = static_cast<uint32_t>(compressed.size());

        size_t offset = outPayload.size();
        outPayload.resize(offset + sizeof(DeltaRectHeader) + compressed.size());
        std::memcpy(outPayload.data() + offset, &rh, sizeof(rh));
        if (!compressed.empty()) {
            std::memcpy(outPayload.data() + offset + sizeof(rh), compressed.data(), compressed.size());
        }
    }

    DeltaBlobHeader bh{};
    bh.magic = kDeltaMagic;
    bh.width = width;
    bh.height = height;
    bh.rectCount = static_cast<uint32_t>(rects.size());
    bh.isKeyframe = outIsKeyframe ? 1 : 0;
    std::memcpy(outPayload.data(), &bh, sizeof(bh));

    return true;
}

void DeltaDecoder::Reset() {
    m_width = 0;
    m_height = 0;
    m_frame.clear();
}

bool DeltaDecoder::Decode(const uint8_t* payload,
                          size_t payloadSize,
                          uint32_t expectedWidth,
                          uint32_t expectedHeight) {
    if (!payload || payloadSize < sizeof(DeltaBlobHeader)) return false;

    DeltaBlobHeader bh{};
    std::memcpy(&bh, payload, sizeof(bh));
    if (bh.magic != kDeltaMagic || bh.width == 0 || bh.height == 0) return false;
    if (expectedWidth != 0 && (bh.width != expectedWidth || bh.height != expectedHeight)) {
        return false;
    }

    if (m_width != bh.width || m_height != bh.height || m_frame.size() != static_cast<size_t>(bh.width) * bh.height * 4) {
        m_width = bh.width;
        m_height = bh.height;
        m_frame.assign(static_cast<size_t>(m_width) * m_height * 4, 0);
    }

    size_t pos = sizeof(DeltaBlobHeader);
    std::vector<uint32_t> linearPixels;

    for (uint32_t i = 0; i < bh.rectCount; ++i) {
        if (pos + sizeof(DeltaRectHeader) > payloadSize) return false;
        DeltaRectHeader rh{};
        std::memcpy(&rh, payload + pos, sizeof(rh));
        pos += sizeof(rh);

        if (pos + rh.compressedBytes > payloadSize) return false;
        if (rh.x + rh.w > m_width || rh.y + rh.h > m_height) return false;

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
    }

    return pos == payloadSize;
}

} // namespace s2p
