#pragma once
#include <cstdint>
#include <vector>

namespace s2p {

struct DeltaRect {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
};

struct DeltaMoveRect {
    uint32_t srcX;
    uint32_t srcY;
    uint32_t dstX;
    uint32_t dstY;
    uint32_t w;
    uint32_t h;
};

class DeltaEncoder {
public:
    void Reset();

    // Coalesces and bounds arbitrary dirty rectangles so we never have to discard
    // DXGI dirty rects when > 64 rectangles are reported.
    static std::vector<DeltaRect> MergeAndBoundRects(const std::vector<DeltaRect>& rects,
                                                     uint32_t frameW,
                                                     uint32_t frameH,
                                                     size_t maxRects = 32);

    // Encodes changed regions of bgraFrame (width x height, stride in bytes) using
    // XOR-delta + fast LZ4 block compression + optional MoveRects and optional
    // 2x2 fast-motion subsampling when allowFastMotion && dirty coverage > 35%.
    bool Encode(const uint8_t* bgraFrame,
                uint32_t width,
                uint32_t height,
                uint32_t strideBytes,
                const std::vector<DeltaRect>& hints,
                bool forceKeyframe,
                std::vector<uint8_t>& outPayload,
                bool& outIsKeyframe,
                const std::vector<DeltaMoveRect>& moveRects = {},
                bool allowFastMotion = false,
                bool* outUsedFastMotion = nullptr);

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::vector<uint8_t> m_prevFrame; // tight width*height*4
    std::vector<uint8_t> m_scratchRaw;
    std::vector<uint8_t> m_scratchCompressed;
    std::vector<uint8_t> m_scratchMove;
    std::vector<uint32_t> m_lz4Hash;
    std::vector<uint8_t> m_dirtyGrid;
};

class DeltaDecoder {
public:
    void Reset();

    // Decodes payload into internal persistent BGRA framebuffer (tight width*height*4).
    // Supports both legacy DLT1 (RLE) and DLT2 (LZ4 + XOR + MoveRects + FastMotion).
    bool Decode(const uint8_t* payload,
                size_t payloadSize,
                uint32_t expectedWidth,
                uint32_t expectedHeight);

    const uint8_t* FrameData() const { return m_frame.data(); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t StrideBytes() const { return m_width * 4; }
    const DeltaRect& LastBoundingRect() const { return m_lastBounds; }
    bool LastDecodeUsedFastMotion() const { return m_lastFastMotion; }

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::vector<uint8_t> m_frame;
    std::vector<uint8_t> m_scratchDecomp;
    std::vector<uint8_t> m_scratchMove;
    DeltaRect m_lastBounds{0, 0, 0, 0};
    bool m_lastFastMotion = false;
};

} // namespace s2p
