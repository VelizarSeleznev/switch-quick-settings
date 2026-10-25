// Software renderer for the overlay layer.
//
// The layer is RGBA4444 with straight alpha (what vi composites for Tesla
// overlays). All blending is done in 8 bits per channel and quantized to 4 bits
// with a 4x4 ordered dither, so soft edges and dark tones keep their shades.
// The same code draws into the swizzled Switch framebuffer and into a linear
// buffer for the host preview.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "stb_truetype.h"

namespace qs {

struct Color {
    uint8_t r, g, b, a;
};

constexpr Color Hex(uint32_t rgb, uint8_t a = 255) {
    return {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), a};
}
Color Fade(Color c, float k);           // alpha * k
Color Mix(Color a, Color b, float t);   // a..b

enum class Font { Regular, Medium, Icons, Count };

class Fonts {
public:
    struct Glyph {
        int w = 0, h = 0, xoff = 0, yoff = 0;
        float advance = 0;
        std::vector<uint8_t> bits;
    };

    Fonts();
    const Glyph& Get(Font f, float px, uint32_t cp);
    float Kern(Font f, float px, uint32_t a, uint32_t b);

private:
    stbtt_fontinfo info_[int(Font::Count)];
    std::unordered_map<uint64_t, Glyph> cache_;
};

class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h) { ResetClip(); }

    // Target buffer: w*h RGBA4444 pixels, Switch block-linear layout or linear.
    void Bind(uint16_t* pixels, bool swizzled) { px_ = pixels; swizzled_ = swizzled; }
    int W() const { return w_; }
    int H() const { return h_; }

    void Clear();
    void SetOpacity(float o) { opacity_ = o; }
    void SetClip(int x, int y, int w, int h);
    void ResetClip() { SetClip(0, 0, w_, h_); }

    void FillRect(float x, float y, float w, float h, Color c) { FillRoundRect(x, y, w, h, 0, c); }
    void FillRoundRect(float x, float y, float w, float h, float r, Color c);
    void StrokeRoundRect(float x, float y, float w, float h, float r, float t, Color c);
    void FillCircle(float cx, float cy, float r, Color c) { FillRoundRect(cx - r, cy - r, 2 * r, 2 * r, r, c); }

    // Draws UTF-8 text with its baseline at y; returns the advance width.
    float Text(float x, float y, std::string_view s, Font f, float px, Color c);
    float Measure(std::string_view s, Font f, float px);
    // Shortens s with "…" so it fits into maxW.
    std::string Fit(std::string_view s, Font f, float px, float maxW);
    // Draws one icon glyph centered on (cx, cy).
    void Icon(uint32_t cp, float cx, float cy, float px, Color c);

    Fonts fonts;

private:
    void Blend(int x, int y, Color c, int cov);
    uint32_t Offset(int x, int y) const;

    int w_, h_;
    uint16_t* px_ = nullptr;
    bool swizzled_ = false;
    float opacity_ = 1.f;
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;  // clip, exclusive max
};

// Decodes one UTF-8 code point and advances i.
uint32_t NextCodepoint(std::string_view s, size_t& i);

}  // namespace qs
