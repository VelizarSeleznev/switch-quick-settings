// The quick settings panel: layout, drawing, touch/button handling, the
// slide-in and the small press/toggle animations. Platform-independent;
// main.cpp feeds it input and a framebuffer, the host preview drives it with
// scripted input.
//
// The panel is rendered into its own buffer, and only the parts that changed
// are drawn again (a tile row while it animates); each frame copies that
// buffer to the screen at the current slide offset.
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

    enum class Request { None, Launch, Reboot, PowerOff };

    explicit Ui(System& sys);

    // Starts showing the panel. tracking: an edge swipe is still under the
    // finger, so the panel follows it until release.
    void Open(Side side, bool tracking, double now);
    // Starts the close animation.
    void Close(double now);
    // Hides at once, without animation (HOME or power button).
    void HideNow();
    // Handles one input sample and advances the animations. Returns false once fully closed.
    bool Update(const Input& in);
    // True when the screen needs a new frame.
    bool NeedsPresent() const;
    // Draws the current frame (an empty one when closed) into the layer canvas.
    void Present(Canvas& screen);

    Side side() const { return side_; }
    int LayerX() const { return side_ == Side::Left ? 0 : kScreenW - kLayerW; }
    // What to do after the panel closed (launch an overlay, restart, ...).
    Request TakeRequest(OverlayEntry* launch);
    State& state() { return st_; }

private:
    enum class Page { Main, Headphones, Wifi, Overlays };
    enum class Drag { None, Pending, Brightness, Volume, Panel, TrackOpen };
    enum Id {
        kNone = 0, kHeadphones, kWifi, kAirplane, kBoost, kCharge, kOverlays,
        kHpIcon, kWifiIcon,                    // icon halves of the two split tiles
        kBrightness, kVolume, kReboot, kPower, kBack, kHpToggle, kWifiToggle,
        kDevice0 = 100, kOverlay0 = 200,
    };
    struct Item {
        int id;
        float x, y, w, h;  // panel coordinates, at rest
        int partner = kNone;  // other item in a pair that squeezes together on press
        bool left = false;    // left one of the pair
    };
    struct Geo {
        float x, y, w, h, r;
    };
    // Damped spring toward target; x may overshoot.
    struct Spring {
        float x = 0, v = 0, target = 0;
        void Step(float dt, float stiffness, float damping);
        bool Settled() const;
    };
    struct Anim {
        Spring on, press;
    };
    // Shown value of a toggle kept for a while after a tap, so the tile does
    // not flicker back while the console is still switching it.
    struct Hold {
        bool active = false, value = false;
        double until = 0;
    };

    void Layout();
    void Activate(int id, double now);
    void Secondary(int id);
    void MoveFocus(uint64_t dir);
    void SetPage(Page p);
    void AnimateReveal(float to, double now);
    void HandleTouch(const Input& in);
    void HandleKeys(const Input& in);
    void SetSlider(int id, float lx, bool commit);
    void StepAnims(float dt);
    void Refresh();
    void Keep(Hold& h, bool value, double seconds);
    void ApplyHolds();
    float PanelX() const;
    int HitTest(float lx, float ly) const;
    const Item* Find(int id) const;
    bool Focusable(int id) const;
    int AnimId(int id) const;
    bool TargetOn(int id) const;
    float On(int id) const;
    float Press(int id) const;
    Geo GeoOf(const Item& it) const;

    void DamageAll();
    void Damage(float x, float y, float w, float h);
    void DamageItem(int id);
    void Render();
    void DrawMain();
    void DrawHeadphones();
    void DrawWifi();
    void DrawOverlays();
    void DrawTile(const Item& it, uint32_t icon, const char* title, const std::string& sub, bool enabled,
                  bool split);
    void DrawSlider(const Item& it, uint32_t icon, float value, bool enabled);
    void DrawToggleCard(const Item& it, uint32_t icon, const char* title, const std::string& sub, bool enabled);
    void DrawRow(const Item& it, uint32_t icon, const std::string& title, const std::string& sub);
    void DrawPageHeader(const char* title);
    void DrawBattery(float right, float baseline);
    void DrawKeyHint(float& x, float baseline, const char* key, const char* label);
    void DrawRipple(int id, float x, float y, float w, float h, float r);
    void DrawFocus(int id, const Geo& g);

    System& sys_;
    std::vector<uint16_t> buf_;   // rendered panel, linear RGBA4444
    Canvas cv_;
    State st_;

    Side side_ = Side::Left;
    bool visible_ = false;
    float reveal_ = 0;            // 0 hidden .. 1 fully open
    float animFrom_ = 0, animTo_ = 0;
    double animStart_ = 0, animDur_ = 0;
    bool animating_ = false;
    float shownReveal_ = -1;      // reveal of the last presented frame
    bool shownVisible_ = false;
    // Part of the panel buffer to draw again (layer pixels; empty when x0 >= x1).
    float dmgX0_ = 0, dmgY0_ = 0, dmgX1_ = 0, dmgY1_ = 0;

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
    bool outside_ = false;
    float dragReveal0_ = 0;

    // Animations.
    std::map<int, Anim> anims_;
    bool animsActive_ = false;
    double lastStep_ = 0;
    int rippleId_ = kNone;        // item with the press ripple
    float rippleX_ = 0, rippleY_ = 0, rippleT_ = 0, rippleA_ = 0;

    Hold holdHp_, holdWifi_, holdBoost_, holdCharge_;

    int armed_ = kNone;           // power button waiting for a second tap
    double armedAt_ = 0;
    double lastRefresh_ = -1;
    double now_ = 0;

    Request request_ = Request::None;
    OverlayEntry launch_;
};

}  // namespace qs
