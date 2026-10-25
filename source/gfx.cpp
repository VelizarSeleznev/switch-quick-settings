#include "gfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace qs {

namespace {

#include "fonts_data.inc"

// 4x4 Bayer matrix: dither thresholds for the 8 -> 4 bit color quantization.
constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Signed distance from (px, py) to a rounded box centered at the origin.
inline float RoundBoxDist(float px, float py, float bx, float by, float r) {
    float qx = std::fabs(px) - bx + r;
    float qy = std::fabs(py) - by + r;
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

}  // namespace

Color Fade(Color c, float k) {
    c.a = uint8_t(std::clamp(c.a * k + 0.5f, 0.f, 255.f));
    return c;
}

Color Mix(Color a, Color b, float t) {
    t = Clamp01(t);
    auto m = [t](uint8_t x, uint8_t y) { return uint8_t(x + (y - x) * t + 0.5f); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

uint32_t NextCodepoint(std::string_view s, size_t& i) {
    auto b = [&](size_t k) { return k < s.size() ? uint8_t(s[k]) : 0x80; };
    uint8_t c = uint8_t(s[i]);
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6) { cp = ((c & 0x1F) << 6) | (b(i + 1) & 0x3F); n = 2; }
    else if ((c >> 4) == 14) { cp = ((c & 0x0F) << 12) | ((b(i + 1) & 0x3F) << 6) | (b(i + 2) & 0x3F); n = 3; }
    else if ((c >> 3) == 30) {
        cp = ((c & 0x07) << 18) | ((b(i + 1) & 0x3F) << 12) | ((b(i + 2) & 0x3F) << 6) | (b(i + 3) & 0x3F);
        n = 4;
    } else { cp = '?'; n = 1; }
    i += n;
    return cp;
}

// ---------------------------------------------------------------- Fonts

Fonts::Fonts() {
    stbtt_InitFont(&info_[int(Font::Regular)], kFontRegular, 0);
    stbtt_InitFont(&info_[int(Font::Medium)], kFontMedium, 0);
    stbtt_InitFont(&info_[int(Font::Icons)], kFontIcons, 0);
}

const Fonts::Glyph& Fonts::Get(Font f, float px, uint32_t cp) {
    uint64_t key = (uint64_t(f) << 56) | (uint64_t(px * 4) << 32) | cp;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    if (cache_.size() > 700) cache_.clear();

    stbtt_fontinfo* info = &info_[int(f)];
    if (f != Font::Icons && !stbtt_FindGlyphIndex(info, int(cp))) cp = '?';
    float scale = stbtt_ScaleForMappingEmToPixels(info, px);
    Glyph g;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(info, int(cp), &adv, &lsb);
    g.advance = adv * scale;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(info, int(cp), scale, scale, &x0, &y0, &x1, &y1);
    g.w = x1 - x0;
    g.h = y1 - y0;
    g.xoff = x0;
    g.yoff = y0;
    if (g.w > 0 && g.h > 0) {
        g.bits.resize(size_t(g.w) * g.h);
        stbtt_MakeCodepointBitmap(info, g.bits.data(), g.w, g.h, g.w, scale, scale, int(cp));
    }
    return cache_.emplace(key, std::move(g)).first->second;
}

float Fonts::Kern(Font f, float px, uint32_t a, uint32_t b) {
    stbtt_fontinfo* info = &info_[int(f)];
    return stbtt_GetCodepointKernAdvance(info, int(a), int(b)) * stbtt_ScaleForMappingEmToPixels(info, px);
}

// ---------------------------------------------------------------- Canvas

void Canvas::Clear() {
    if (px_) std::memset(px_, 0, size_t(w_) * h_ * 2);
}

void Canvas::SetClip(int x, int y, int w, int h) {
    cx0_ = std::max(x, 0);
    cy0_ = std::max(y, 0);
    cx1_ = std::min(x + w, w_);
    cy1_ = std::min(y + h, h_);
}

uint32_t Canvas::Offset(int x, int y) const {
    if (!swizzled_) return uint32_t(y) * w_ + x;
    // Block-linear layout of the vi framebuffer (same math as libtesla).
    uint32_t p = ((y & 127) / 16) + (x / 32 * 8) + ((y / 16 / 8) * ((w_ / 2) / 16 * 8));
    p *= 16 * 16 * 4;
    p += ((y % 16) / 8) * 512 + ((x % 32) / 16) * 256 + ((y % 8) / 2) * 64 + ((x % 16) / 8) * 32 + (y % 2) * 16 +
         (x % 8) * 2;
    return p / 2;
}

void Canvas::Blend(int x, int y, Color c, int cov) {
    if (x < cx0_ || y < cy0_ || x >= cx1_ || y >= cy1_) return;
    int sa = (c.a * cov + 127) / 255;
    if (opacity_ < 1.f) sa = int(sa * opacity_ + 0.5f);
    if (sa <= 0) return;
    uint16_t& dst = px_[Offset(x, y)];
    int r, g, b, a;
    if (sa >= 255) {
        r = c.r; g = c.g; b = c.b; a = 255;
    } else {
        int da = (dst >> 12) * 17;
        int dr = (dst & 0xF) * 17, dg = ((dst >> 4) & 0xF) * 17, db = ((dst >> 8) & 0xF) * 17;
        int k = da * (255 - sa);           // dst weight * 255
        int outA = sa * 255 + k;           // alpha * 255
        r = (c.r * sa * 255 + dr * k) / outA;
        g = (c.g * sa * 255 + dg * k) / outA;
        b = (c.b * sa * 255 + db * k) / outA;
        a = (outA + 127) / 255;
    }
    int t = kBayer[y & 3][x & 3];
    auto q = [t](int v) { return std::min(15, (v * 15 * 16 / 255 + t) / 16); };
    int qa = (a * 15 + 127) / 255;
    dst = uint16_t(q(r) | (q(g) << 4) | (q(b) << 8) | (qa << 12));
}

void Canvas::FillRoundRect(float x, float y, float w, float h, float r, Color c) {
    if (w <= 0 || h <= 0 || c.a == 0) return;
    r = std::min({r, w / 2, h / 2});
    int x0 = std::max(int(std::floor(x)), cx0_), x1 = std::min(int(std::ceil(x + w)), cx1_);
    int y0 = std::max(int(std::floor(y)), cy0_), y1 = std::min(int(std::ceil(y + h)), cy1_);
    float mx = x + w / 2, my = y + h / 2, bx = w / 2, by = h / 2;
    for (int py = y0; py < y1; py++) {
        float fy = py + 0.5f;
        bool cornerRow = fy < y + r || fy > y + h - r;
        float rowCov = Clamp01(by - std::fabs(fy - my) + 0.5f);  // coverage from top/bottom edges
        for (int px = x0; px < x1; px++) {
            float fx = px + 0.5f;
            float cov;
            if (cornerRow && (fx < x + r || fx > x + w - r)) {
                cov = Clamp01(0.5f - RoundBoxDist(fx - mx, fy - my, bx, by, r));
            } else {
                cov = std::min(rowCov, Clamp01(bx - std::fabs(fx - mx) + 0.5f));
            }
            if (cov > 0) Blend(px, py, c, int(cov * 255 + 0.5f));
        }
    }
}

void Canvas::StrokeRoundRect(float x, float y, float w, float h, float r, float t, Color c) {
    if (w <= 0 || h <= 0) return;
    r = std::min({r, w / 2, h / 2});
    float ri = std::max(r - t, 0.f);
    int x0 = std::max(int(std::floor(x)), cx0_), x1 = std::min(int(std::ceil(x + w)), cx1_);
    int y0 = std::max(int(std::floor(y)), cy0_), y1 = std::min(int(std::ceil(y + h)), cy1_);
    float mx = x + w / 2, my = y + h / 2;
    for (int py = y0; py < y1; py++) {
        float fy = py + 0.5f;
        bool edgeRow = fy < y + t + r + 1 || fy > y + h - t - r - 1;
        for (int px = x0; px < x1; px++) {
            float fx = px + 0.5f;
            if (!edgeRow && fx > x + t + 1 && fx < x + w - t - 1) {
                int skipTo = int(x + w - t - 1);  // last column of the hollow middle
                if (skipTo > px) px = skipTo;
                continue;
            }
            float outer = Clamp01(0.5f - RoundBoxDist(fx - mx, fy - my, w / 2, h / 2, r));
            float inner = Clamp01(0.5f - RoundBoxDist(fx - mx, fy - my, w / 2 - t, h / 2 - t, ri));
            float cov = outer - inner;
            if (cov > 0) Blend(px, py, c, int(cov * 255 + 0.5f));
        }
    }
}

float Canvas::Text(float x, float y, std::string_view s, Font f, float px, Color c) {
    float pen = x;
    uint32_t prev = 0;
    int by = int(std::lround(y));
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = NextCodepoint(s, i);
        if (prev) pen += fonts.Kern(f, px, prev, cp);
        const Fonts::Glyph& g = fonts.Get(f, px, cp);
        int gx = int(std::lround(pen)) + g.xoff, gy = by + g.yoff;
        for (int yy = 0; yy < g.h; yy++)
            for (int xx = 0; xx < g.w; xx++) {
                uint8_t v = g.bits[size_t(yy) * g.w + xx];
                if (v) Blend(gx + xx, gy + yy, c, v);
            }
        pen += g.advance;
        prev = cp;
    }
    return pen - x;
}

float Canvas::Measure(std::string_view s, Font f, float px) {
    float pen = 0;
    uint32_t prev = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = NextCodepoint(s, i);
        if (prev) pen += fonts.Kern(f, px, prev, cp);
        pen += fonts.Get(f, px, cp).advance;
        prev = cp;
    }
    return pen;
}

std::string Canvas::Fit(std::string_view s, Font f, float px, float maxW) {
    if (Measure(s, f, px) <= maxW) return std::string(s);
    std::string out(s);
    while (!out.empty()) {
        // Drop the last code point (skip UTF-8 continuation bytes).
        size_t n = out.size() - 1;
        while (n > 0 && (uint8_t(out[n]) & 0xC0) == 0x80) n--;
        out.resize(n);
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (Measure(out + "…", f, px) <= maxW) break;
    }
    return out + "…";
}

void Canvas::Icon(uint32_t cp, float cx, float cy, float px, Color c) {
    const Fonts::Glyph& g = fonts.Get(Font::Icons, px, cp);
    int gx = int(std::lround(cx - g.w / 2.f)), gy = int(std::lround(cy - g.h / 2.f));
    for (int yy = 0; yy < g.h; yy++)
        for (int xx = 0; xx < g.w; xx++) {
            uint8_t v = g.bits[size_t(yy) * g.w + xx];
            if (v) Blend(gx + xx, gy + yy, c, v);
        }
}

}  // namespace qs
