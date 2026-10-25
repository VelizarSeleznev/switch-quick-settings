// Host preview: runs the real UI code against a fake console and writes PNG
// screenshots of the 1280x720 screen, so the look and the gesture logic can be
// checked without a Switch. The layer is rendered at its true 4-bit depth.
//
//   host/build.sh && host/out/preview [background.jpg]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "gfx.hpp"
#include "ui.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace qs;

namespace {

// The fake console keeps its state in base. With lag > 0 a change reaches
// base only that many seconds later, like Bluetooth or sys-clk on the
// console, so Refresh reports the old value for a while.
struct FakeSystem : System {
    State base;
    std::vector<std::string> log;
    double now = 0, lag = 0;
    std::vector<std::pair<double, std::function<void()>>> later;

    void Apply(std::function<void()> f) {
        if (lag <= 0) f();
        else later.push_back({now + lag, std::move(f)});
    }
    void Refresh(State& s) override {
        for (auto it = later.begin(); it != later.end();) {
            if (it->first <= now) {
                it->second();
                it = later.erase(it);
            } else {
                ++it;
            }
        }
        // Lists are refreshed separately.
        auto paired = s.paired;
        auto overlays = s.overlays;
        s = base;
        s.paired = paired;
        s.overlays = overlays;
    }
    void RefreshClock(State& s) override {
        s.hour = base.hour;
        s.minute = base.minute;
        s.battery = base.battery;
    }
    void ListDevices(State& s) override { s.paired = base.paired; }
    void ListOverlays(State& s) override { s.overlays = base.overlays; }
    void SetHeadphones(State& s, bool on) override {
        s.hpConnecting = on;
        s.hpConnected = false;
        Apply([this, on] {
            base.hpConnected = on;
            base.hpPaused = !on;
            base.radioOn = base.radioOn || on;
        });
        log.push_back(on ? "headphones on" : "headphones off");
    }
    void ToggleDevice(State& s, const AudioDevice& d) override { log.push_back("toggle " + d.name); }
    void SetWifi(State& s, bool on) override {
        s.wifiOn = on;
        Apply([this, on] { base.wifiOn = on; });
        log.push_back(on ? "wifi on" : "wifi off");
    }
    void SetAirplane(State& s, bool on) override {
        s.airplane = base.airplane = on;
        s.wifiOn = base.wifiOn = !on;
        s.radioOn = base.radioOn = !on;
        if (on) s.hpConnected = base.hpConnected = false;
        log.push_back(on ? "airplane on" : "airplane off");
    }
    void SetBoost(State& s, bool on) override {
        s.boost = on;
        Apply([this, on] {
            base.boost = on;
            base.cpuMHz = on ? 1785 : 1020;
        });
        log.push_back(on ? "boost on" : "boost off");
    }
    void SetChargingEnabled(State& s, bool on) override { s.chargingEnabled = base.chargingEnabled = on; }
    void SetBrightness(State& s, float v, bool commit) override {
        s.brightness = base.brightness = v;
        if (commit) log.push_back("brightness " + std::to_string(int(v * 100)));
    }
    void SetVolume(State& s, float v) override { s.volume = base.volume = v; }
    void Reboot() override { log.push_back("reboot"); }
    void PowerOff() override { log.push_back("power off"); }
    void LaunchOverlay(const OverlayEntry& o) override { log.push_back("launch " + o.path); }
};

State SampleState() {
    State s;
    s.hour = 17; s.minute = 42; s.weekday = 5; s.day = 24; s.month = 10;
    s.battery = 76; s.charging = false; s.pluggedIn = false;
    s.btReady = true; s.autoconnect = true; s.hpConnected = true; s.hpName = "WH-1000XM5";
    s.paired = {{{1}, "WH-1000XM5", true}, {{2}, "AirPods Pro", false}, {{3}, "JBL Flip 6", false}};
    s.wifiReady = true; s.wifiOn = true; s.wifiOnline = true; s.ssid = "Velizard-5G";
    s.wifiSignal = 3; s.ip = "10.0.0.99";
    s.sysclkReady = true; s.boost = false; s.cpuMHz = 1020; s.gpuMHz = 384; s.memMHz = 1600;
    s.socTemp = 44; s.pcbTemp = 38; s.powerMw = -5230; s.powerKnown = true;
    s.chargeCtlReady = true; s.chargingEnabled = true;
    s.lblReady = true; s.brightness = 0.62f;
    s.volReady = true; s.volume = 0.4f; s.volSteps = 15;
    s.overlays = {{"sdmc:/switch/.overlays/sys-patch-overlay.ovl", "sys-patch", "1.5.7"},
                  {"sdmc:/switch/.overlays/sys-clk-overlay.ovl", "sys-clk", "2.0.1"}};
    return s;
}

std::vector<uint8_t> g_bg;  // 1280x720 RGB

void LoadBackground(const char* path) {
    g_bg.assign(1280 * 720 * 3, 0);
    int w = 0, h = 0, n = 0;
    uint8_t* img = path ? stbi_load(path, &w, &h, &n, 3) : nullptr;
    for (int y = 0; y < 720; y++)
        for (int x = 0; x < 1280; x++) {
            uint8_t* d = &g_bg[(y * 1280 + x) * 3];
            if (img) {
                // Cover-fit the image.
                float s = std::max(1280.f / w, 720.f / h);
                int sx = std::min(w - 1, int((x - 640) / s + w / 2.f));
                int sy = std::min(h - 1, int((y - 360) / s + h / 2.f));
                memcpy(d, &img[(sy * w + sx) * 3], 3);
            } else {
                d[0] = uint8_t(60 + 120 * x / 1280);
                d[1] = uint8_t(90 + 80 * y / 720);
                d[2] = uint8_t(160 - 60 * x / 1280);
            }
        }
    if (img) stbi_image_free(img);
}

void Save(const std::string& name, const std::vector<uint16_t>& layer, int layerX) {
    std::vector<uint8_t> out = g_bg;
    for (int y = 0; y < 720; y++)
        for (int x = 0; x < Ui::kLayerW; x++) {
            uint16_t p = layer[y * Ui::kLayerW + x];
            float a = (p >> 12) / 15.f;
            if (a <= 0) continue;
            uint8_t c[3] = {uint8_t((p & 0xF) * 17), uint8_t(((p >> 4) & 0xF) * 17), uint8_t(((p >> 8) & 0xF) * 17)};
            uint8_t* d = &out[(y * 1280 + x + layerX) * 3];
            for (int k = 0; k < 3; k++) d[k] = uint8_t(c[k] * a + d[k] * (1 - a) + 0.5f);
        }
    std::string path = "host/out/" + name + ".png";
    stbi_write_png(path.c_str(), 1280, 720, 3, out.data(), 1280 * 3);
    printf("wrote %s\n", path.c_str());
}

struct Harness {
    FakeSystem sys;
    std::vector<uint16_t> layer = std::vector<uint16_t>(Ui::kLayerW * Ui::kLayerH);
    Canvas cv{Ui::kLayerW, Ui::kLayerH};
    Ui ui;
    double t = 0;
    Touch touch;

    explicit Harness(State s = SampleState()) : ui(sys) {
        sys.base = s;
        cv.Bind(layer.data(), false);
        sys.Refresh(ui.state());
        sys.ListOverlays(ui.state());
    }
    // Render cost of presented frames, for a rough budget check.
    double renderMs = 0, renderMaxMs = 0;
    int renders = 0;

    void Frame(uint64_t down = 0) {
        sys.now = t;
        Input in{down, 0, touch, t};
        ui.Update(in);
        if (ui.NeedsPresent()) {
            auto t0 = std::chrono::steady_clock::now();
            ui.Present(cv);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            renderMs += ms;
            renderMaxMs = std::max(renderMaxMs, ms);
            renders++;
        }
    }
    void ResetStats() { renderMs = renderMaxMs = 0; renders = 0; }
    // Runs frames at 60 fps.
    void Run(double seconds, uint64_t down = 0) {
        int frames = std::max(1, int(seconds * 60));
        for (int i = 0; i < frames; i++) {
            t += 1 / 60.0;
            Frame(i == 0 ? down : 0);
        }
    }
    bool Open() { return ui.Update({0, 0, {}, t}); }
    // Like the console: a released touch carries no position.
    void Release() { touch = {false, 0, 0}; }
    void Press(uint64_t k) { Run(1 / 60.0, k); Run(0.3); }
    void Tap(float x, float y) {
        touch = {true, x, y};
        Run(0.05);
        Release();
        Run(0.3);
    }
    // Press and release inside one frame (main.cpp replays both samples).
    void QuickTap(float x, float y) {
        t += 1 / 60.0;
        touch = {true, x, y};
        Frame();
        Release();
        Frame();
        Run(0.3);
    }
    void Swipe(float x0, float y0, float x1, float y1, double dur) {
        int n = std::max(2, int(dur * 60));
        for (int i = 0; i <= n; i++) {
            touch = {true, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n};
            Run(1 / 60.0);
        }
        Release();
    }
    void Snap(const std::string& name) {
        ui.Present(cv);
        Save(name, layer, ui.LayerX());
    }
};

int g_fail = 0;
void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) g_fail++;
}

}  // namespace

// Screen position of a point given in panel coordinates (left panel, fully open).
constexpr float P(float v) { return 12 + v; }

int main(int argc, char** argv) {
    LoadBackground(argc > 1 ? argv[1] : nullptr);

    // Panel geometry used by the taps below (see the constants in ui.cpp).
    const float rowY[3] = {P(222 + 36), P(304 + 36), P(386 + 36)};
    const float leftIcon = P(20 + 34), leftBody = P(20 + 130);
    const float rightIcon = P(20 + 197 + 34), rightBody = P(20 + 197 + 130);
    const float leftMid = P(20 + 90), rightMid = P(20 + 197 + 90);
    const float brightY = P(100 + 25), volumeY = P(158 + 25);
    const float backX = P(12 + 24), backY = P(14 + 24), cardY = P(80 + 36);

    {   // Main page, left.
        Harness h;
        h.ui.Open(Side::Left, false, h.t);
        h.Run(0.5);
        h.Snap("main_left");
        // Controller focus on Wi-Fi.
        h.Press(key::Down);
        h.Press(key::Right);
        h.Snap("main_focus");
        h.Press(key::A);
        Check(!h.sys.base.wifiOn, "A on focused Wi-Fi toggles it off");
        h.Snap("main_wifi_off");
    }
    {   // Right side, boost on, charging.
        State s = SampleState();
        s.boost = true; s.cpuMHz = 1785; s.gpuMHz = 921; s.charging = true; s.pluggedIn = true; s.powerMw = 9100;
        s.hpConnected = false; s.hpPaused = true; s.volume = 0;
        Harness h(s);
        h.ui.Open(Side::Right, false, h.t);
        h.Run(0.5);
        h.Snap("main_right");
    }
    {   // No sys-clk, docked.
        State s = SampleState();
        s.sysclkReady = false; s.docked = true; s.overlays.clear(); s.battery = 12;
        Harness h(s);
        h.ui.Open(Side::Left, false, h.t);
        h.Run(0.5);
        h.Snap("main_docked_nosysclk");
    }
    {   // Edge swipe from the left: panel follows the finger, then opens.
        Harness h;
        h.touch = {true, 60, 300};
        h.ui.Open(Side::Left, true, h.t);
        h.touch = {true, 60, 300};
        h.Run(1 / 60.0);
        for (int i = 0; i < 10; i++) {
            h.touch.x = 60 + i * 12;
            h.Run(1 / 60.0);
        }
        h.Snap("swipe_midway");
        h.Release();
        h.Run(0.5);
        Check(h.Open(), "edge swipe released past a third stays open");

        // Split tiles: the icon switches, the rest opens the details.
        h.QuickTap(leftIcon, rowY[0]);
        Check(!h.sys.base.hpConnected && h.sys.base.hpPaused, "tap on the headphones icon disconnects them");
        h.QuickTap(leftIcon, rowY[0]);
        Check(h.sys.base.hpConnected && !h.sys.base.hpPaused, "second tap on the icon connects them");
        h.QuickTap(leftBody, rowY[0]);
        h.Snap("headphones");
        h.QuickTap(200, cardY);
        Check(!h.sys.base.hpConnected, "the tile's right part opens the headphones page (its switch works)");
        h.QuickTap(200, cardY);
        h.QuickTap(backX, backY);
        h.QuickTap(rightIcon, rowY[0]);
        Check(!h.sys.base.wifiOn, "back arrow returns; tap on the Wi-Fi icon turns Wi-Fi off");
        h.QuickTap(rightBody, rowY[0]);
        h.QuickTap(200, cardY);
        Check(h.sys.base.wifiOn, "Wi-Fi page switch turns it back on");
        h.Run(0.6);
        h.Snap("wifi");
        h.QuickTap(backX, backY);

        // Airplane mode stays on while Wi-Fi or headphones come back one by one.
        h.QuickTap(leftMid, rowY[1]);
        Check(h.sys.base.airplane && !h.sys.base.wifiOn && !h.sys.base.radioOn, "airplane mode turns radios off");
        h.QuickTap(rightIcon, rowY[0]);
        Check(h.sys.base.wifiOn && h.sys.base.airplane, "Wi-Fi turns on and airplane mode stays on");
        h.QuickTap(leftIcon, rowY[0]);
        Check(h.sys.base.hpConnected && h.sys.base.airplane, "headphones connect and airplane mode stays on");
        h.Run(0.6);
        h.Snap("airplane");
        h.QuickTap(leftMid, rowY[1]);
        Check(!h.sys.base.airplane && h.sys.base.wifiOn, "second tap turns airplane mode off");

        // Sliders.
        h.Swipe(P(20 + 40), brightY, P(20 + 200), brightY, 0.3);
        h.Run(0.1);
        Check(std::fabs(h.sys.base.brightness - (220.f - 45) / 334) < 0.02f, "brightness slider drag");
        h.Swipe(P(20 + 60), volumeY, P(20 + 250), volumeY, 0.3);
        h.Run(0.1);
        Check(std::fabs(h.sys.base.volume - (270.f - 45) / 334) < 0.02f &&
                  std::fabs(h.ui.state().volume * 15 - std::round(h.ui.state().volume * 15)) < 1e-4f,
              "volume slider drag, shown at a whole step after release");
        // Reboot needs two taps.
        h.Tap(leftMid, P(624 + 26));
        h.Snap("reboot_armed");
        Check(h.sys.log.empty() || h.sys.log.back() != "reboot", "first tap on restart only arms it");
        // Swipe the panel back to the left edge to close it.
        h.Swipe(300, 400, 80, 410, 0.15);
        h.Run(0.6);
        Check(!h.ui.Update({0, 0, {}, h.t}), "swiping the panel toward the edge closes it");
    }
    {   // The console takes a while to switch: the tile must not flicker back meanwhile.
        Harness h;
        h.sys.lag = 1.2;
        h.ui.Open(Side::Left, false, h.t);
        h.Run(0.6);
        auto steady = [&](auto shown, bool want) {
            bool ok = true;
            for (int i = 0; i < 150; i++) {
                h.t += 1 / 60.0;
                h.Frame();
                ok = ok && shown() == want;
            }
            return ok;
        };
        h.QuickTap(rightMid, rowY[1]);
        Check(steady([&] { return h.ui.state().boost; }, true) && h.sys.base.boost,
              "overclock stays on while sys-clk applies it");
        h.QuickTap(leftIcon, rowY[0]);
        Check(steady([&] { return h.ui.state().hpConnected || h.ui.state().hpConnecting; }, false),
              "headphones stay off while they disconnect");
        h.QuickTap(rightIcon, rowY[0]);
        Check(steady([&] { return h.ui.state().wifiOn; }, false), "Wi-Fi stays off while it switches");
    }
    {   // Press animation: the pressed tile widens and pushes its neighbor, then bounces back.
        Harness h;
        h.ui.Open(Side::Left, false, h.t);
        h.Run(1.2);
        h.ResetStats();
        h.touch = {true, leftMid, rowY[2]};
        h.Run(0.25);
        h.Snap("press_hold");
        h.Release();
        h.Run(0.1);
        h.Snap("press_bounce");
        h.Run(0.8);
        printf("  press animation: %d frames drawn, avg %.2f ms, max %.2f ms\n", h.renders,
               h.renders ? h.renderMs / h.renders : 0, h.renderMaxMs);
        h.ResetStats();
        h.Press(key::Down);  // focus ring: whole panel redrawn
        printf("  full redraw: avg %.2f ms over %d frames\n", h.renders ? h.renderMs / h.renders : 0, h.renders);
        h.QuickTap(rightMid, rowY[1]);
        h.Run(0.05);
        h.Snap("toggle_morph");
    }
    {   // A quick flick from the right edge opens; a hesitant tiny pull closes again.
        Harness h;
        h.touch = {true, 1240, 300};
        h.ui.Open(Side::Right, true, h.t);
        h.Run(0.05);
        h.Release();
        h.Run(0.5);
        Check(h.ui.Update({0, 0, {}, h.t}), "quick flick from the right edge opens");

        Harness g;
        g.touch = {true, 1262, 300};
        g.ui.Open(Side::Right, true, g.t);
        g.Run(0.6);
        g.Release();
        g.Run(0.6);
        Check(!g.ui.Update({0, 0, {}, g.t}), "slow tiny pull from the edge closes again");
    }
    {   // Overlays page and launching one.
        Harness h;
        h.ui.Open(Side::Left, false, h.t);
        h.Run(0.5);
        h.Tap(rightMid, rowY[2]);
        h.Snap("overlays");
        h.Tap(P(20 + 100), P(84 + 30));
        h.Run(0.6);
        OverlayEntry o;
        Check(h.ui.TakeRequest(&o) == Ui::Request::Launch && o.name == "sys-patch", "tapping an overlay launches it");
    }
    {   // Tap outside the panel closes it.
        Harness h;
        h.ui.Open(Side::Left, false, h.t);
        h.Run(0.5);
        h.Tap(900, 300);
        h.Run(0.5);
        Check(!h.ui.Update({0, 0, {}, h.t}), "tap outside closes");
    }
    printf(g_fail ? "%d check(s) failed\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
