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

class DeltaEncoder {
public:
    void Reset();

    // Encodes changed regions of bgraFrame (width x height, stride in bytes).
    // If hints is non-empty and !forceKeyframe, only those rectangles are encoded.
    // Otherwise tiles are diffed against the previous frame.
    bool Encode(const uint8_t* bgraFrame,
                uint32_t width,
                uint32_t height,
                uint32_t strideBytes,
                const std::vector<DeltaRect>& hints,
                bool forceKeyframe,
                std::vector<uint8_t>& outPayload,
                bool& outIsKeyframe);

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::vector<uint8_t> m_prevFrame; // tight width*height*4
};

class DeltaDecoder {
public:
    void Reset();

    // Decodes payload into internal persistent BGRA framebuffer (tight width*height*4).
    bool Decode(const uint8_t* payload,
                size_t payloadSize,
                uint32_t expectedWidth,
                uint32_t expectedHeight);

    const uint8_t* FrameData() const { return m_frame.data(); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t StrideBytes() const { return m_width * 4; }

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::vector<uint8_t> m_frame;
};

} // namespace s2p
