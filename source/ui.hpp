// The quick settings panel: layout, drawing, touch/button handling and the
// slide-in animation. Platform-independent; main.cpp feeds it input and
// presents the canvas, the host preview drives it with scripted input.
#pragma once

#include <map>

#include "gfx.hpp"
#include "qs.hpp"

namespace qs {

enum class Side { Left, Right };

struct Touch {
    bool down = false;
    float x = 0, y = 0;  // screen coordinates, 1280x720
};

struct Input {
    uint64_t down = 0, held = 0;  // key:: bits
    Touch touch;
    double now = 0;               // seconds
};

class Ui {
public:
    static constexpr int kLayerW = 448, kLayerH = 720, kScreenW = 1280;

    enum class Request { None, ScreenOff, Launch, Reboot, PowerOff };

    Ui(System& sys, Canvas& canvas, bool russian);

    // Starts showing the panel. tracking: an edge swipe is still under the
    // finger, so the panel follows it until release.
    void Open(Side side, bool tracking, double now);
    // Starts the close animation.
    void Close(double now);
    // Hides at once, without animation (HOME or power button).
    void HideNow();
    // Handles one frame of input and animation. Returns false once fully closed.
    bool Update(const Input& in);
    bool NeedsDraw() const { return dirty_; }
    void Draw();

    Side side() const { return side_; }
    int LayerX() const { return side_ == Side::Left ? 0 : kScreenW - kLayerW; }
    // What to do after the panel closed (screen off, launch an overlay, ...).
    Request TakeRequest(OverlayEntry* launch);
    State& state() { return st_; }

private:
    enum class Page { Main, Headphones, Overlays };
    enum class Drag { None, Pending, Slider, Panel, TrackOpen };
    enum Id {
        kNone = 0, kHeadphones, kWifi, kBoost, kScreen, kCharge, kOverlays,
        kSlider, kAuto, kReboot, kPower, kBack, kHpToggle,
        kDevice0 = 100, kOverlay0 = 200,
    };
    struct Item {
        int id;
        float x, y, w, h;  // panel coordinates
    };

    void Layout();
    void Activate(int id, double now);
    void Secondary(int id);
    void MoveFocus(uint64_t dir);
    void SetPage(Page p);
    void AnimateReveal(float to, double now);
    void HandleTouch(const Input& in);
    void HandleKeys(const Input& in);
    float PanelX() const;
    int HitTest(float lx, float ly) const;
    const Item* Find(int id) const;
    float OnAmount(int id, bool on);

    void DrawMain();
    void DrawHeadphones();
    void DrawOverlays();
    void DrawTile(const Item& it, uint32_t icon, const char* title, const std::string& sub, bool on, bool enabled,
                  bool chevron);
    void DrawRow(const Item& it, uint32_t icon, const std::string& title, const std::string& sub, bool on);
    void DrawPageHeader(const char* title);
    void DrawBattery(float right, float baseline);
    void DrawKeyHint(float& x, float baseline, const char* key, const char* label);
    void DrawPressed(const Item& it, float r);
    void DrawFocus(const Item& it, float r);

    System& sys_;
    Canvas& cv_;
    State st_;
    bool ru_;

    Side side_ = Side::Left;
    bool visible_ = false;
    float reveal_ = 0;            // 0 hidden .. 1 fully open
    float animFrom_ = 0, animTo_ = 0;
    double animStart_ = 0, animDur_ = 0;
    bool animating_ = false;
    bool dirty_ = true;

    Page page_ = Page::Main;
    std::vector<Item> items_;
    int focus_ = kHeadphones;
    bool focusVisible_ = false;

    // Touch state.
    bool wasDown_ = false;
    Drag drag_ = Drag::None;
    float startX_ = 0, startY_ = 0, lastX_ = 0, lastY_ = 0;
    double startT_ = 0;
    float vx_ = 0;                // finger speed along x, px/s
    double lastT_ = 0;
    int pressed_ = kNone;
    bool longFired_ = false;
    bool outside_ = false;
    float dragReveal0_ = 0;

    int armed_ = kNone;           // power button waiting for a second tap
    double armedAt_ = 0;
    double lastRefresh_ = -1;
    double now_ = 0;
    std::map<int, float> onAnim_;
    double lastDraw_ = 0;
    bool tileAnim_ = false;       // a tile color transition is still running
    float ox_ = 0, oy_ = 0;       // panel origin while drawing

    Request request_ = Request::None;
    OverlayEntry launch_;
};

}  // namespace qs
