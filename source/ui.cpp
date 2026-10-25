#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qs {

namespace {

// Geometry (layer pixels; the layer is 448x720 at 720p).
constexpr float kMargin = 12;                       // gap between panel and screen edge
constexpr float kO = kMargin;                       // panel origin inside the layer
constexpr float kPW = Ui::kLayerW - 2 * kMargin;    // panel width
constexpr float kPH = Ui::kLayerH - 2 * kMargin;    // panel height
constexpr float kPad = 20;
constexpr float kCW = kPW - 2 * kPad;               // content width
constexpr float kGap = 10;
constexpr float kTileW = (kCW - kGap) / 2, kTileH = 72;
constexpr float kTilesY = 222;
constexpr float kIconZone = 64;                     // icon half of a split tile (touch area)
constexpr float kIconBox = 44, kIconInset = 10;     // icon container
constexpr float kTextX = kIconInset + kIconBox + 8;  // tile title and subtitle
constexpr float kSliderY = 100, kSliderH = 50, kSliderStep = 58;
constexpr float kStatsY = 470, kStatsH = 114;
constexpr float kPowerH = 52, kPowerY = kPH - kPad - kPowerH;
constexpr float kRowH = 60, kRowStep = 68;
constexpr int kMaxRows = 6;
constexpr float kBounce = 12;                       // how far a pressed tile pushes its neighbor
constexpr double kRefreshEvery = 0.5;

// "Pastel Dream" palette: the cream, rose and cyan of the installed home menu theme.
constexpr Color kPanel = Hex(0xFFF8F3);
constexpr Color kBorder = Hex(0xEDB8C3);
constexpr Color kSurface = Hex(0xF2D8D5);
constexpr Color kSurfaceLow = Hex(0xF9EAE6);
constexpr Color kSurfaceHigh = Hex(0xE8C5C1);
constexpr Color kOnSurface = Hex(0x4A3238);
constexpr Color kMuted = Hex(0x8F747B);
constexpr Color kPrimary = Hex(0xF29BB2);
constexpr Color kPrimaryHigh = Hex(0xD9607F);
constexpr Color kOnPrimary = Hex(0x55162A);
constexpr Color kOnPrimaryHigh = Hex(0xFFFFFF);
constexpr Color kOnPrimaryMuted = Hex(0x7E3A4E);
constexpr Color kDanger = Hex(0xE2667A);
constexpr Color kOnDanger = Hex(0xFFFFFF);
constexpr Color kFocus = Hex(0x4FBFD8);
constexpr Color kRipple = Hex(0x7A2A40, 0x22);
constexpr Color kWarn = Hex(0xD9475E);
constexpr Color kKeyCap = Hex(0xEBD5D0);

// Material Symbols code points (subset built by tools/make_fonts.py).
namespace icon {
constexpr uint32_t Headphones = 0xF01F, HeadsetOff = 0xE33A, BluetoothOff = 0xE1A9;
constexpr uint32_t Wifi = 0xE63E, WifiOff = 0xE648, Wifi1 = 0xEBE4, Wifi2 = 0xEBD6, Wifi3 = 0xEBE1;
constexpr uint32_t Speed = 0xE9E4, Flight = 0xE539, Charging = 0xE1A3, Light = 0xE518;
constexpr uint32_t VolumeUp = 0xE050, VolumeDown = 0xE04D, VolumeOff = 0xE04F;
constexpr uint32_t Power = 0xF8C7, Restart = 0xF053, Chevron = 0xE5CC, Back = 0xE5C4, Layers = 0xE53B;
constexpr uint32_t Bolt = 0xEA0B;
}  // namespace icon

// UI strings (English only).
struct Text {
    const char* weekdays[7];
    const char* months[12];
    const char *headphones, *on, *off, *connecting, *btOff, *wifi, *noNetwork, *airplane, *handheldOnly, *boost,
        *needSysclk, *chargePause, *notCharging, *charging, *pluggedIn, *onBattery, *overlays, *count, *none,
        *reboot, *powerOff, *again, *devices, *noDevices, *autoOn, *autoPaused, *connected, *noOverlays, *select,
        *more, *close, *mhz, *soc, *pcb, *draw, *charge, *watt, *noSysclk, *noBt, *network, *signal, *ip,
        *notConnected, *airplaneOn;
    const char* signalWords[4];
};

const Text kEn = {
    {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
    {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November",
     "December"},
    "Headphones", "On", "Off", "Connecting…", "Bluetooth off", "Wi-Fi", "No network", "Airplane mode",
    "Handheld only", "Overclock", "Needs sys-clk", "Pause charging", "Not charging", "Charging", "Plugged in",
    "On battery", "Overlays", "%d", "None", "Restart", "Power off", "Tap again", "Paired devices",
    "No paired headphones", "Auto-connect on", "Auto-connect paused", "Connected", "No other overlays", "select",
    "details", "close", "MHz", "SoC", "Board", "Draw", "Charge", "W",
    "sys-clk is not running: overclock and sensors unavailable", "Bluetooth unavailable", "Network", "Signal",
    "IP address", "Not connected", "Airplane mode is on: Wi-Fi and headphones can be turned on one by one",
    {"Weak", "Weak", "Good", "Excellent"},
};

inline float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
inline float Clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

bool IsTile(int id) { return id >= 1 && id <= 6; }  // kHeadphones .. kOverlays

}  // namespace

static const Text* const T = &kEn;

Ui::Ui(System& sys) : sys_(sys), buf_(size_t(kLayerW) * kLayerH), cv_(kLayerW, kLayerH) {
    cv_.Bind(buf_.data(), false);
}

// ------------------------------------------------------------ springs

void Ui::Spring::Step(float dt, float stiffness, float damping) {
    // Small fixed steps keep the stiff press spring stable at any frame rate.
    int n = std::max(1, int(std::ceil(dt * 240)));
    float h = dt / n;
    for (int i = 0; i < n; i++) {
        float a = -stiffness * (x - target) - damping * v;
        v += a * h;
        x += v * h;
    }
    if (Settled()) {
        x = target;
        v = 0;
    }
}

bool Ui::Spring::Settled() const { return std::fabs(x - target) < 0.003f && std::fabs(v) < 0.03f; }

// ------------------------------------------------------------ open / close

void Ui::Open(Side side, bool tracking, double now) {
    side_ = side;
    visible_ = true;
    page_ = Page::Main;
    focus_ = kHeadphones;
    focusVisible_ = false;
    armed_ = kNone;
    pressed_ = kNone;
    rippleId_ = kNone;
    request_ = Request::None;
    anims_.clear();
    // Only the cheap part now; the full poll runs once the panel stops moving.
    sys_.RefreshClock(st_);
    lastRefresh_ = now - kRefreshEvery;
    now_ = lastStep_ = now;
    reveal_ = 0;
    animating_ = false;
    if (tracking) {
        drag_ = Drag::TrackOpen;
        wasDown_ = true;
        startT_ = lastT_ = now;
        lastX_ = -1;
        vx_ = 0;
    } else {
        drag_ = Drag::None;
        wasDown_ = false;
        AnimateReveal(1, now);
    }
    Layout();
    DamageAll();
}

void Ui::Close(double now) {
    drag_ = Drag::None;
    pressed_ = kNone;
    AnimateReveal(0, now);
}

void Ui::HideNow() {
    visible_ = false;
    reveal_ = 0;
    animating_ = false;
    drag_ = Drag::None;
    pressed_ = kNone;
}

void Ui::AnimateReveal(float to, double now) {
    animFrom_ = reveal_;
    animTo_ = to;
    animStart_ = now;
    animDur_ = std::max(0.09, 0.24 * std::fabs(to - reveal_));
    animating_ = true;
}

Ui::Request Ui::TakeRequest(OverlayEntry* launch) {
    Request r = request_;
    request_ = Request::None;
    if (launch) *launch = launch_;
    return r;
}

float Ui::PanelX() const {
    float hidden = kPW + kMargin + 8;
    return side_ == Side::Left ? kMargin - (1 - reveal_) * hidden : kMargin + (1 - reveal_) * hidden;
}

// ------------------------------------------------------------ update

bool Ui::Update(const Input& in) {
    now_ = in.now;
    if (!visible_) return false;
    float dt = float(std::clamp(now_ - lastStep_, 0.0, 0.05));
    lastStep_ = now_;

    // Status IPC takes a few milliseconds: never while the panel moves, and
    // not in the middle of a press animation unless that runs long.
    bool busy = animating_ || drag_ != Drag::None || (animsActive_ && now_ - lastRefresh_ < 1.5);
    if (now_ - lastRefresh_ >= kRefreshEvery && !busy) Refresh();
    if (armed_ != kNone && now_ - armedAt_ > 3) {
        armed_ = kNone;
        DamageItem(kReboot);
    }

    bool closing = animating_ && animTo_ == 0;
    if (!closing) {
        HandleKeys(in);
        HandleTouch(in);
    }
    Layout();
    StepAnims(dt);

    if (animating_) {
        float t = float((now_ - animStart_) / animDur_);
        if (t >= 1) {
            t = 1;
            animating_ = false;
        }
        reveal_ = animFrom_ + (animTo_ - animFrom_) * EaseOut(t);
        if (!animating_ && animTo_ == 0) {
            visible_ = false;
            reveal_ = 0;
        }
    }
    return visible_;
}

void Ui::Refresh() {
    sys_.Refresh(st_);
    ApplyHolds();
    lastRefresh_ = now_;
    DamageAll();
}

void Ui::Keep(Hold& h, bool value, double seconds) {
    h.active = true;
    h.value = value;
    h.until = now_ + seconds;
}

// Call right after the console state was read: it compares what the console
// reports with the held values, so on a stale State it would end holds early.
void Ui::ApplyHolds() {
    // Returns true while the hold still decides what is shown.
    auto holding = [&](Hold& h, bool reached) {
        if (h.active && (reached || now_ > h.until)) h.active = false;
        return h.active;
    };
    if (holding(holdHp_, holdHp_.value ? st_.hpConnected : !(st_.hpConnected || st_.hpConnecting))) {
        if (holdHp_.value) st_.hpConnecting = !st_.hpConnected;
        else st_.hpConnected = st_.hpConnecting = false;
    }
    if (holding(holdWifi_, st_.wifiOn == holdWifi_.value)) st_.wifiOn = holdWifi_.value;
    if (holding(holdBoost_, st_.boost == holdBoost_.value)) st_.boost = holdBoost_.value;
    if (holding(holdCharge_, st_.chargingEnabled == holdCharge_.value)) st_.chargingEnabled = holdCharge_.value;
}

void Ui::HandleKeys(const Input& in) {
    uint64_t d = in.down;
    if (!d) return;
    DamageAll();
    if (d & key::B) {
        if (page_ != Page::Main) SetPage(Page::Main);
        else Close(now_);
        return;
    }
    auto ensureFocus = [&] {
        if (Find(focus_) && Focusable(focus_)) return;
        for (const Item& it : items_)
            if (Focusable(it.id)) {
                focus_ = it.id;
                break;
            }
    };
    uint64_t dirs = key::AnyUp | key::AnyDown | key::AnyLeft | key::AnyRight;
    if (d & dirs) {
        if (!focusVisible_) {
            focusVisible_ = true;
            ensureFocus();
            return;
        }
        bool horizontal = d & (key::AnyLeft | key::AnyRight);
        float sign = (d & key::AnyRight) ? 1.f : -1.f;
        if (focus_ == kBrightness && horizontal && st_.lblReady && !st_.docked) {
            sys_.SetBrightness(st_, std::clamp(st_.brightness + 0.05f * sign, 0.f, 1.f), true);
            return;
        }
        if (focus_ == kVolume && horizontal && st_.volReady) {
            float step = 1.f / float(std::max(1, st_.volSteps));
            sys_.SetVolume(st_, std::clamp(std::round(st_.volume / step + sign) * step, 0.f, 1.f));
            return;
        }
        MoveFocus(d);
        return;
    }
    if (d & key::A) {
        if (!focusVisible_) {
            focusVisible_ = true;
            ensureFocus();
            return;
        }
        Activate(focus_, now_);
    } else if (d & key::X) {
        focusVisible_ = true;
        Secondary(focus_);
    }
}

void Ui::SetSlider(int id, float lx, bool commit) {
    const Item* s = Find(id);
    if (!s) return;
    float v = Clamp01((lx - s->x - s->h / 2) / (s->w - s->h));
    if (id == kBrightness) {
        sys_.SetBrightness(st_, v, commit);
    } else {
        sys_.SetVolume(st_, v);
        if (commit) {  // the console has whole steps only
            float steps = float(std::max(1, st_.volSteps));
            st_.volume = std::round(st_.volume * steps) / steps;
        }
    }
    DamageItem(id);
}

void Ui::HandleTouch(const Input& in) {
    const Touch& t = in.touch;
    bool left = side_ == Side::Left;
    // A released touch reports no position: use where the finger last was.
    float x = t.down ? t.x : lastX_, y = t.down ? t.y : lastY_;
    float lx = x - LayerX() - PanelX();
    float ly = y - kMargin;

    if (t.down && !wasDown_) {
        startX_ = lastX_ = t.x;
        startY_ = lastY_ = t.y;
        startT_ = lastT_ = now_;
        vx_ = 0;
        outside_ = lx < 0 || lx > kPW || ly < 0 || ly > kPH;
        pressed_ = outside_ ? kNone : HitTest(lx, ly);
        if (focusVisible_) {
            focusVisible_ = false;
            DamageAll();
        }
        bool brightnessOk = st_.lblReady && !st_.docked;
        if ((pressed_ == kBrightness && brightnessOk) || (pressed_ == kVolume && st_.volReady)) {
            drag_ = pressed_ == kBrightness ? Drag::Brightness : Drag::Volume;
            SetSlider(pressed_, lx, false);
        } else {
            drag_ = Drag::Pending;
            if (pressed_ != kNone && pressed_ != kBrightness && pressed_ != kVolume) {
                if (rippleId_ != kNone) DamageItem(rippleId_);
                rippleId_ = pressed_;
                rippleX_ = lx;
                rippleY_ = ly;
                rippleT_ = rippleA_ = 0;
            }
        }
    } else if (t.down && wasDown_) {
        if (lastX_ < 0) {  // first sample of an edge swipe handed over from the input thread
            startX_ = lastX_ = t.x;
            startY_ = lastY_ = t.y;
            lastT_ = now_;
        }
        double dt = now_ - lastT_;
        if (dt > 0) vx_ = vx_ * 0.5f + float((t.x - lastX_) / dt) * 0.5f;
        lastX_ = t.x;
        lastY_ = t.y;
        if (dt > 0) lastT_ = now_;
        float dx = t.x - startX_, dy = t.y - startY_;
        switch (drag_) {
            case Drag::TrackOpen: {
                float dist = left ? t.x : kScreenW - t.x;
                reveal_ = std::clamp(dist / (kPW * 0.85f), 0.f, 1.f);
                animating_ = false;
                break;
            }
            case Drag::Brightness:
            case Drag::Volume:
                SetSlider(drag_ == Drag::Brightness ? kBrightness : kVolume, lx, false);
                break;
            case Drag::Pending:
                if (std::hypot(dx, dy) > 18) {
                    float toEdge = left ? -dx : dx;
                    if (std::fabs(dx) > std::fabs(dy) && toEdge > 0) {
                        drag_ = Drag::Panel;
                        dragReveal0_ = reveal_;
                        animating_ = false;
                    } else {
                        drag_ = Drag::None;
                    }
                    pressed_ = kNone;
                }
                break;
            case Drag::Panel: {
                float toEdge = left ? -dx : dx;
                reveal_ = std::clamp(dragReveal0_ - std::max(0.f, toEdge) / kPW, 0.f, 1.f);
                break;
            }
            case Drag::None:
                break;
        }
    } else if (!t.down && wasDown_) {
        float inward = left ? vx_ : -vx_;  // > 0: moving away from the edge
        switch (drag_) {
            case Drag::TrackOpen:
                if (inward > 300 || reveal_ > 0.3f || now_ - startT_ < 0.25) AnimateReveal(1, now_);
                else Close(now_);
                break;
            case Drag::Panel:
                if (inward < -500 || (inward < 300 && reveal_ < 0.7f)) Close(now_);
                else AnimateReveal(1, now_);
                break;
            case Drag::Brightness:
            case Drag::Volume:
                SetSlider(drag_ == Drag::Brightness ? kBrightness : kVolume, lx, true);
                break;
            case Drag::Pending:
                if (outside_) {
                    Close(now_);
                } else {
                    int id = HitTest(lx, ly);
                    if (id != kNone && id == pressed_) {
                        // Split tiles: the icon switches, the rest opens the details.
                        if (id == kHeadphones || id == kWifi) Secondary(id);
                        else Activate(AnimId(id), now_);
                    }
                }
                break;
            case Drag::None:
                break;
        }
        drag_ = Drag::None;
        pressed_ = kNone;
    }
    wasDown_ = t.down;
}

// ------------------------------------------------------------ actions

void Ui::Activate(int id, double now) {
    DamageAll();
    lastRefresh_ = now - kRefreshEvery + 0.3;  // show the real result soon
    if (id != kReboot && id != kPower) armed_ = kNone;
    switch (id) {
        case kHeadphones:
        case kHpToggle:
            if (st_.btReady) {
                bool on = !(st_.hpConnected || st_.hpConnecting);
                sys_.SetHeadphones(st_, on);
                Keep(holdHp_, on, on ? 15 : 6);
            }
            return;
        case kWifi:
        case kWifiToggle:
            if (st_.wifiReady) {
                bool on = !st_.wifiOn;
                sys_.SetWifi(st_, on);
                Keep(holdWifi_, on, 5);
            }
            return;
        case kAirplane:
            if (st_.wifiReady && st_.btReady && !st_.docked) {
                bool on = !st_.airplane;
                sys_.SetAirplane(st_, on);
                Keep(holdWifi_, !on, 5);
                holdHp_.active = false;
            }
            return;
        case kBoost:
            if (st_.sysclkReady) {
                bool on = !st_.boost;
                sys_.SetBoost(st_, on);
                Keep(holdBoost_, on, 3);
            }
            return;
        case kCharge:
            if (st_.chargeCtlReady) {
                bool on = !st_.chargingEnabled;
                sys_.SetChargingEnabled(st_, on);
                Keep(holdCharge_, on, 3);
            }
            return;
        case kOverlays:
            SetPage(Page::Overlays);
            return;
        case kReboot:
        case kPower:
            if (armed_ == id) {
                request_ = id == kReboot ? Request::Reboot : Request::PowerOff;
                armed_ = kNone;
                Close(now);
            } else {
                armed_ = id;
                armedAt_ = now;
            }
            return;
        case kBack:
            SetPage(Page::Main);
            return;
        default:
            break;
    }
    if (id >= kOverlay0) {
        size_t i = size_t(id - kOverlay0);
        if (i < st_.overlays.size()) {
            launch_ = st_.overlays[i];
            request_ = Request::Launch;
            Close(now);
        }
    } else if (id >= kDevice0) {
        size_t i = size_t(id - kDevice0);
        if (i < st_.paired.size()) {
            sys_.ToggleDevice(st_, st_.paired[i]);
            holdHp_.active = false;
        }
    }
}

void Ui::Secondary(int id) {
    if (id == kHeadphones) SetPage(Page::Headphones);
    else if (id == kWifi) SetPage(Page::Wifi);
    else if (id == kOverlays) SetPage(Page::Overlays);
}

void Ui::SetPage(Page p) {
    Page from = page_;
    page_ = p;
    if (p == Page::Headphones) {
        sys_.ListDevices(st_);
        ApplyHolds();
    }
    if (p == Page::Overlays) sys_.ListOverlays(st_);
    Layout();
    if (p == Page::Main) {
        focus_ = from == Page::Overlays ? kOverlays : (from == Page::Wifi ? kWifi : kHeadphones);
    } else {
        focus_ = items_.size() > 1 ? items_[1].id : kBack;
    }
    pressed_ = kNone;
    rippleId_ = kNone;
    armed_ = kNone;
    DamageAll();
}

void Ui::MoveFocus(uint64_t d) {
    const Item* cur = Find(focus_);
    if (!cur) {
        for (const Item& it : items_)
            if (Focusable(it.id)) {
                focus_ = it.id;
                break;
            }
        return;
    }
    float cx = cur->x + cur->w / 2, cy = cur->y + cur->h / 2;
    float best = 1e9f;
    int bestId = kNone;
    for (const Item& it : items_) {
        if (it.id == cur->id || !Focusable(it.id)) continue;
        float dx = it.x + it.w / 2 - cx, dy = it.y + it.h / 2 - cy;
        float primary, cross;
        if (d & key::AnyUp) { primary = -dy; cross = std::fabs(dx); }
        else if (d & key::AnyDown) { primary = dy; cross = std::fabs(dx); }
        else if (d & key::AnyLeft) { primary = -dx; cross = std::fabs(dy); }
        else { primary = dx; cross = std::fabs(dy); }
        if (primary <= 1) continue;
        float score = primary + cross * 2;
        if (score < best) { best = score; bestId = it.id; }
    }
    if (bestId != kNone) focus_ = bestId;
}

// ------------------------------------------------------------ layout

void Ui::Layout() {
    items_.clear();
    switch (page_) {
        case Page::Main: {
            items_.push_back({kBrightness, kPad, kSliderY, kCW, kSliderH});
            items_.push_back({kVolume, kPad, kSliderY + kSliderStep, kCW, kSliderH});
            // Icon halves first: HitTest takes the first match.
            items_.push_back({kHpIcon, kPad, kTilesY, kIconZone, kTileH});
            items_.push_back({kWifiIcon, kPad + kTileW + kGap, kTilesY, kIconZone, kTileH});
            const int ids[6] = {kHeadphones, kWifi, kAirplane, kBoost, kCharge, kOverlays};
            for (int i = 0; i < 6; i++) {
                bool l = i % 2 == 0;
                items_.push_back({ids[i], kPad + (l ? 0 : kTileW + kGap), kTilesY + (i / 2) * (kTileH + kGap), kTileW,
                                  kTileH, ids[l ? i + 1 : i - 1], l});
            }
            items_.push_back({kReboot, kPad, kPowerY, kTileW, kPowerH, kPower, true});
            items_.push_back({kPower, kPad + kTileW + kGap, kPowerY, kTileW, kPowerH, kReboot, false});
            break;
        }
        case Page::Headphones: {
            items_.push_back({kBack, 12, 14, 48, 48});
            items_.push_back({kHpToggle, kPad, 80, kCW, 72});
            int n = std::min<int>(int(st_.paired.size()), kMaxRows - 1);
            for (int i = 0; i < n; i++) items_.push_back({kDevice0 + i, kPad, 196 + i * kRowStep, kCW, kRowH});
            break;
        }
        case Page::Wifi:
            items_.push_back({kBack, 12, 14, 48, 48});
            items_.push_back({kWifiToggle, kPad, 80, kCW, 72});
            break;
        case Page::Overlays: {
            items_.push_back({kBack, 12, 14, 48, 48});
            int n = std::min<int>(int(st_.overlays.size()), kMaxRows + 1);
            for (int i = 0; i < n; i++) items_.push_back({kOverlay0 + i, kPad, 84 + i * kRowStep, kCW, kRowH});
            break;
        }
    }
}

int Ui::HitTest(float lx, float ly) const {
    for (const Item& it : items_)
        if (lx >= it.x && lx < it.x + it.w && ly >= it.y && ly < it.y + it.h) return it.id;
    return kNone;
}

const Ui::Item* Ui::Find(int id) const {
    for (const Item& it : items_)
        if (it.id == id) return &it;
    return nullptr;
}

bool Ui::Focusable(int id) const { return id != kHpIcon && id != kWifiIcon; }

int Ui::AnimId(int id) const {
    if (id == kHpIcon) return kHeadphones;
    if (id == kWifiIcon) return kWifi;
    return id;
}

bool Ui::TargetOn(int id) const {
    switch (id) {
        case kHeadphones: return st_.btReady && (st_.hpConnected || st_.hpConnecting);
        case kHpToggle: return st_.hpConnected || st_.hpConnecting;
        case kWifi: return st_.wifiReady && st_.wifiOn;
        case kWifiToggle: return st_.wifiOn;
        case kAirplane: return st_.airplane && !st_.docked;
        case kBoost: return st_.sysclkReady && st_.boost;
        case kCharge: return st_.chargeCtlReady && !st_.chargingEnabled;
        case kReboot:
        case kPower: return armed_ == id;
        default: break;
    }
    if (id >= kDevice0 && id < kOverlay0) {
        size_t i = size_t(id - kDevice0);
        return i < st_.paired.size() && st_.paired[i].connected;
    }
    return false;
}

float Ui::On(int id) const {
    auto it = anims_.find(AnimId(id));
    return it == anims_.end() ? (TargetOn(AnimId(id)) ? 1.f : 0.f) : it->second.on.x;
}

float Ui::Press(int id) const {
    auto it = anims_.find(AnimId(id));
    return it == anims_.end() ? 0.f : it->second.press.x;
}

void Ui::StepAnims(float dt) {
    bool active = false;
    int pressedAnim = drag_ == Drag::Pending ? AnimId(pressed_) : kNone;
    for (const Item& it : items_) {
        if (it.id == kHpIcon || it.id == kWifiIcon || it.id == kBrightness || it.id == kVolume) continue;
        bool on = TargetOn(it.id);
        auto [entry, fresh] = anims_.try_emplace(it.id);
        Anim& a = entry->second;
        if (fresh) a.on.x = a.on.target = on ? 1.f : 0.f;
        a.on.target = on ? 1.f : 0.f;
        a.press.target = pressedAnim == it.id ? 1.f : 0.f;
        if (a.on.Settled() && a.press.Settled() && a.on.x == a.on.target && a.press.x == a.press.target) continue;
        // Toggles morph with a little overshoot; a released press bounces back past rest.
        a.on.Step(dt, 320, 24);
        a.press.Step(dt, 900, 22);
        DamageItem(it.id);
        active = true;
    }
    if (rippleId_ != kNone) {
        bool held = pressed_ == rippleId_ && drag_ == Drag::Pending;
        rippleT_ = std::min(1.f, rippleT_ + dt / 0.4f);
        rippleA_ = held ? std::min(1.f, rippleA_ + dt / 0.06f) : rippleA_ - dt / 0.3f;
        DamageItem(rippleId_);
        if (!held && rippleA_ <= 0) rippleId_ = kNone;
        active = true;
    }
    animsActive_ = active;
}

Ui::Geo Ui::GeoOf(const Item& it) const {
    if (it.id == kHpIcon || it.id == kWifiIcon) {
        const Item* tile = Find(AnimId(it.id));
        Geo t = tile ? GeoOf(*tile) : Geo{it.x, it.y, it.w, it.h, 0};
        float on = On(it.id);
        return {t.x + kIconInset, t.y + (t.h - kIconBox) / 2, kIconBox, kIconBox,
                std::max(6.f, Lerp(kIconBox / 2, 14, on))};
    }
    Geo g{it.x, it.y, it.w, it.h, 0};
    float on = On(it.id);
    if (IsTile(it.id)) g.r = Lerp(kTileH / 2, 20, on);
    else if (it.id == kReboot || it.id == kPower) g.r = Lerp(kPowerH / 2, 14, on);
    else if (it.id == kHpToggle || it.id == kWifiToggle) g.r = Lerp(36, 22, on);
    else if (it.id == kBack) g.r = it.w / 2;
    else if (it.id == kBrightness || it.id == kVolume) g.r = it.h / 2;
    else g.r = Lerp(kRowH / 2, 16, on);
    g.r = std::max(6.f, g.r);
    if (it.partner != kNone) {
        int l = it.left ? it.id : it.partner, r = it.left ? it.partner : it.id;
        float d = kBounce * (Press(l) - Press(r));
        if (it.left) g.w += d;
        else {
            g.x += d;
            g.w -= d;
        }
    }
    return g;
}

// ------------------------------------------------------------ drawing

void Ui::DamageAll() {
    dmgX0_ = 0;
    dmgY0_ = 0;
    dmgX1_ = kLayerW;
    dmgY1_ = kLayerH;
}

void Ui::Damage(float x, float y, float w, float h) {
    // Panel coordinates in, layer pixels kept.
    float x0 = kO + x, y0 = kO + y, x1 = x0 + w, y1 = y0 + h;
    if (dmgX0_ >= dmgX1_) {
        dmgX0_ = x0; dmgY0_ = y0; dmgX1_ = x1; dmgY1_ = y1;
        return;
    }
    dmgX0_ = std::min(dmgX0_, x0);
    dmgY0_ = std::min(dmgY0_, y0);
    dmgX1_ = std::max(dmgX1_, x1);
    dmgY1_ = std::max(dmgY1_, y1);
}

void Ui::DamageItem(int id) {
    const Item* it = Find(id);
    if (!it) return;
    float x0 = it->x, x1 = it->x + it->w;
    if (it->partner != kNone) {
        if (const Item* p = Find(it->partner)) {
            x0 = std::min(x0, p->x);
            x1 = std::max(x1, p->x + p->w);
        }
    }
    // Room for the bounce and the focus ring.
    Damage(x0 - kBounce - 8, it->y - 8, x1 - x0 + 2 * kBounce + 16, it->h + 16);
}

bool Ui::NeedsPresent() const {
    if (!visible_) return shownVisible_;
    return dmgX0_ < dmgX1_ || !shownVisible_ || reveal_ != shownReveal_;
}

void Ui::Present(Canvas& screen) {
    if (!visible_ || reveal_ <= 0.001f) {
        screen.Clear();
    } else {
        if (dmgX0_ < dmgX1_) Render();
        screen.Blit(buf_.data(), int(std::lround(PanelX() - kMargin)));
    }
    shownVisible_ = visible_;
    shownReveal_ = reveal_;
}

void Ui::Render() {
    int x0 = int(std::floor(dmgX0_)), y0 = int(std::floor(dmgY0_));
    int x1 = int(std::ceil(dmgX1_)), y1 = int(std::ceil(dmgY1_));
    dmgX0_ = dmgX1_ = dmgY0_ = dmgY1_ = 0;
    cv_.SetClip(x0, y0, x1 - x0, y1 - y0);
    cv_.Clear();
    cv_.FillRoundRect(kO, kO, kPW, kPH, 30, kPanel);
    cv_.StrokeRoundRect(kO, kO, kPW, kPH, 30, 1.5f, kBorder);
    switch (page_) {
        case Page::Main: DrawMain(); break;
        case Page::Headphones: DrawHeadphones(); break;
        case Page::Wifi: DrawWifi(); break;
        case Page::Overlays: DrawOverlays(); break;
    }
    cv_.ResetClip();
}

void Ui::DrawRipple(int id, float x, float y, float w, float h, float r) {
    if (rippleId_ != id || rippleA_ <= 0) return;
    float cx = kO + rippleX_, cy = kO + rippleY_;
    float far = 0;
    for (float px : {x, x + w})
        for (float py : {y, y + h}) far = std::max(far, std::hypot(px - cx, py - cy));
    float radius = 12 + (far - 12) * EaseOut(rippleT_);
    cv_.FillCircleInRoundRect(cx, cy, radius, x, y, w, h, r, Fade(kRipple, rippleA_));
}

void Ui::DrawFocus(int id, const Geo& g) {
    if (focusVisible_ && focus_ == id)
        cv_.StrokeRoundRect(kO + g.x - 4, kO + g.y - 4, g.w + 8, g.h + 8, g.r + 4, 3, kFocus);
}

void Ui::DrawBattery(float right, float baseline) {
    float bw = 30, bh = 15, x = right - bw - 3, y = baseline - 13;
    bool low = st_.battery <= 15 && !st_.charging;
    Color c = low ? kWarn : kOnSurface;
    cv_.StrokeRoundRect(x, y, bw, bh, 4.5f, 1.6f, Fade(c, 0.9f));
    cv_.FillRoundRect(x + bw + 1, y + 4.5f, 2.5f, 6, 1.2f, Fade(c, 0.9f));
    float fill = (bw - 5) * std::clamp(st_.battery, 0, 100) / 100.f;
    if (fill > 0.5f) cv_.FillRoundRect(x + 2.5f, y + 2.5f, fill, bh - 5, 2, c);
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", st_.battery);
    float tw = cv_.Measure(pct, Font::Medium, 17);
    float tx = x - 8 - tw;
    cv_.Text(tx, baseline, pct, Font::Medium, 17, kOnSurface);
    if (st_.charging) cv_.Icon(icon::Bolt, tx - 11, baseline - 6, 18, kDanger);
}

void Ui::DrawKeyHint(float& x, float baseline, const char* k, const char* label) {
    cv_.FillCircle(x + 10, baseline - 5, 10, kKeyCap);
    float kw = cv_.Measure(k, Font::Medium, 12);
    cv_.Text(x + 10 - kw / 2, baseline - 1, k, Font::Medium, 12, kOnSurface);
    x += 25;
    x += cv_.Text(x, baseline, label, Font::Regular, 14, kMuted) + 16;
}

void Ui::DrawTile(const Item& it, uint32_t ic, const char* title, const std::string& sub, bool enabled, bool split) {
    float t = enabled ? Clamp01(On(it.id)) : 0;
    Color bg = enabled ? Mix(kSurface, kPrimary, t) : kSurfaceLow;
    Color fg = enabled ? Mix(kOnSurface, kOnPrimary, t) : Fade(kOnSurface, 0.4f);
    Color sc = enabled ? Mix(kMuted, kOnPrimaryMuted, t) : Fade(kMuted, 0.6f);
    Geo g = GeoOf(it);
    float x = kO + g.x, y = kO + g.y;
    cv_.FillRoundRect(x, y, g.w, g.h, g.r, bg);
    DrawRipple(it.id, x, y, g.w, g.h, g.r);
    float iconX = x + kIconInset + kIconBox / 2, iconY = y + g.h / 2;
    if (split) {
        const Item* half = Find(it.id == kHeadphones ? kHpIcon : kWifiIcon);
        Geo b = half ? GeoOf(*half) : Geo{g.x + kIconInset, g.y + 12, kIconBox, kIconBox, kIconBox / 2};
        float bx = kO + b.x, by = kO + b.y;
        cv_.FillRoundRect(bx, by, b.w, b.h, b.r, enabled ? Mix(kSurfaceHigh, kPrimaryHigh, t) : kSurface);
        if (half) DrawRipple(half->id, bx, by, b.w, b.h, b.r);
        cv_.Icon(ic, iconX, iconY, 26, enabled ? Mix(kOnSurface, kOnPrimaryHigh, t) : fg);
    } else {
        iconX = x + 30;  // no container: icon and text sit closer, long titles fit
        cv_.Icon(ic, iconX, iconY, 26, fg);
    }
    bool chevron = split || it.id == kOverlays;
    float tx = split ? kTextX : 54;
    float textX = x + tx, maxW = g.w - tx - (chevron ? 26 : 12);
    cv_.Text(textX, y + 31, cv_.Fit(title, Font::Medium, 16, maxW), Font::Medium, 16, fg);
    cv_.Text(textX, y + 52, cv_.Fit(sub, Font::Regular, 14, maxW), Font::Regular, 14, sc);
    if (chevron) cv_.Icon(icon::Chevron, x + g.w - 20, y + g.h / 2, 22, Fade(fg, 0.8f));
    DrawFocus(it.id, g);
}

void Ui::DrawSlider(const Item& it, uint32_t ic, float value, bool enabled) {
    Geo g = GeoOf(it);
    float x = kO + g.x, y = kO + g.y;
    float fillW = g.h + (g.w - g.h) * Clamp01(value);
    cv_.FillRoundRect(x, y, g.w, g.h, g.r, enabled ? kSurface : kSurfaceLow);
    cv_.FillRoundRect(x, y, fillW, g.h, g.r, enabled ? kPrimary : Fade(kPrimary, 0.4f));
    cv_.Icon(ic, x + g.h / 2, y + g.h / 2, 24, enabled ? kOnPrimary : Fade(kOnPrimary, 0.5f));
    DrawFocus(it.id, g);
}

void Ui::DrawToggleCard(const Item& it, uint32_t ic, const char* title, const std::string& sub, bool enabled) {
    float t = enabled ? Clamp01(On(it.id)) : 0, tk = enabled ? On(it.id) : 0;
    Geo g = GeoOf(it);
    float x = kO + g.x, y = kO + g.y;
    cv_.FillRoundRect(x, y, g.w, g.h, g.r, enabled ? Mix(kSurface, kPrimary, t) : kSurfaceLow);
    DrawRipple(it.id, x, y, g.w, g.h, g.r);
    Color fg = enabled ? Mix(kOnSurface, kOnPrimary, t) : Fade(kOnSurface, 0.4f);
    cv_.Icon(ic, x + 34, y + g.h / 2, 28, fg);
    cv_.Text(x + 62, y + 31, title, Font::Medium, 18, fg);
    cv_.Text(x + 62, y + 53, cv_.Fit(sub, Font::Regular, 14, g.w - 140), Font::Regular, 14,
             Mix(kMuted, kOnPrimaryMuted, t));
    // Switch: the knob slides (with the spring's overshoot) and grows when on.
    float sw = 52, sh = 32, sx = x + g.w - sw - 18, sy = y + (g.h - sh) / 2;
    cv_.FillRoundRect(sx, sy, sw, sh, sh / 2, Mix(Hex(0xE3CCC7), kOnPrimary, t));
    float kx = sx + sh / 2 + tk * (sw - sh);
    cv_.FillCircle(kx, sy + sh / 2, 9 + 3 * t, Mix(Hex(0xFFFFFF), kPrimary, t));
    DrawFocus(it.id, g);
}

void Ui::DrawRow(const Item& it, uint32_t ic, const std::string& title, const std::string& sub) {
    float t = Clamp01(On(it.id));
    Color bg = Mix(kSurface, kPrimary, t);
    Color fg = Mix(kOnSurface, kOnPrimary, t);
    Color sc = Mix(kMuted, kOnPrimaryMuted, t);
    Geo g = GeoOf(it);
    float x = kO + g.x, y = kO + g.y;
    cv_.FillRoundRect(x, y, g.w, g.h, g.r, bg);
    DrawRipple(it.id, x, y, g.w, g.h, g.r);
    cv_.Icon(ic, x + 30, y + g.h / 2, 24, fg);
    float maxW = g.w - 70;
    if (sub.empty()) {
        cv_.Text(x + 56, y + g.h / 2 + 6, cv_.Fit(title, Font::Medium, 16, maxW), Font::Medium, 16, fg);
    } else {
        cv_.Text(x + 56, y + 27, cv_.Fit(title, Font::Medium, 16, maxW), Font::Medium, 16, fg);
        cv_.Text(x + 56, y + 46, cv_.Fit(sub, Font::Regular, 13.5f, maxW), Font::Regular, 13.5f, sc);
    }
    DrawFocus(it.id, g);
}

void Ui::DrawPageHeader(const char* title) {
    if (const Item* b = Find(kBack)) {
        Geo g = GeoOf(*b);
        float x = kO + g.x, y = kO + g.y;
        cv_.FillRoundRect(x, y, g.w, g.h, g.r, kSurface);
        DrawRipple(kBack, x, y, g.w, g.h, g.r);
        cv_.Icon(icon::Back, x + g.w / 2, y + g.h / 2, 24, kOnSurface);
        DrawFocus(kBack, g);
    }
    cv_.Text(kO + 70, kO + 46, title, Font::Medium, 22, kOnSurface);
}

void Ui::DrawMain() {
    char buf[96];
    const float x0 = kO + kPad, top = kO;

    // Header: clock, date, battery.
    snprintf(buf, sizeof buf, "%02d:%02d", st_.hour, st_.minute);
    cv_.Text(x0, top + 58, buf, Font::Medium, 44, kOnSurface);
    const char* wd = T->weekdays[std::clamp(st_.weekday, 0, 6)];
    const char* mo = T->months[std::clamp(st_.month - 1, 0, 11)];
    snprintf(buf, sizeof buf, "%s, %s %d", wd, mo, st_.day);
    cv_.Text(x0 + 1, top + 84, buf, Font::Regular, 16, kMuted);
    DrawBattery(kO + kPW - kPad, top + 40);

    for (const Item& it : items_) {
        switch (it.id) {
            case kBrightness:
                DrawSlider(it, icon::Light, st_.brightness, st_.lblReady && !st_.docked);
                break;
            case kVolume: {
                uint32_t ic = st_.volume <= 0.001f ? icon::VolumeOff
                                                   : (st_.volume < 0.5f ? icon::VolumeDown : icon::VolumeUp);
                DrawSlider(it, ic, st_.volume, st_.volReady);
                break;
            }
            case kHeadphones: {
                std::string sub;
                if (!st_.btReady) sub = T->noBt;
                else if (st_.hpConnecting) sub = T->connecting;
                else if (st_.hpConnected) sub = st_.hpName;
                else if (!st_.radioOn) sub = T->btOff;
                else sub = T->off;
                bool on = st_.hpConnected || st_.hpConnecting;
                uint32_t ic = on ? icon::Headphones : (st_.radioOn ? icon::HeadsetOff : icon::BluetoothOff);
                DrawTile(it, ic, T->headphones, sub, st_.btReady, true);
                break;
            }
            case kWifi: {
                std::string sub = !st_.wifiOn ? T->off : (st_.wifiOnline && !st_.ssid.empty() ? st_.ssid : T->noNetwork);
                const uint32_t bars[4] = {icon::Wifi1, icon::Wifi2, icon::Wifi3, icon::Wifi};
                uint32_t ic = !st_.wifiOn ? icon::WifiOff : (st_.wifiOnline ? bars[std::clamp(st_.wifiSignal, 0, 3)]
                                                                            : icon::Wifi);
                DrawTile(it, ic, T->wifi, sub, st_.wifiReady, true);
                break;
            }
            case kAirplane: {
                bool usable = st_.wifiReady && st_.btReady && !st_.docked;
                const char* sub = st_.docked ? T->handheldOnly : (st_.airplane ? T->on : T->off);
                DrawTile(it, icon::Flight, T->airplane, sub, usable, false);
                break;
            }
            case kBoost: {
                std::string sub;
                if (!st_.sysclkReady) sub = T->needSysclk;
                else if (st_.boost) {
                    snprintf(buf, sizeof buf, "%u · %u %s", st_.cpuMHz, st_.gpuMHz, T->mhz);
                    sub = buf;
                } else sub = T->off;
                DrawTile(it, icon::Speed, T->boost, sub, st_.sysclkReady, false);
                break;
            }
            case kCharge: {
                bool paused = !st_.chargingEnabled;
                std::string sub = paused ? T->notCharging
                                         : (st_.charging ? T->charging : (st_.pluggedIn ? T->pluggedIn : T->onBattery));
                DrawTile(it, icon::Charging, T->chargePause, sub, st_.chargeCtlReady, false);
                break;
            }
            case kOverlays: {
                std::string sub = T->none;
                if (!st_.overlays.empty()) {
                    snprintf(buf, sizeof buf, T->count, int(st_.overlays.size()));
                    sub = buf;
                }
                DrawTile(it, icon::Layers, T->overlays, sub, true, false);
                break;
            }
            case kReboot:
            case kPower: {
                float t = Clamp01(On(it.id));
                Geo g = GeoOf(it);
                float x = kO + g.x, y = kO + g.y;
                cv_.FillRoundRect(x, y, g.w, g.h, g.r, Mix(kSurface, kDanger, t));
                DrawRipple(it.id, x, y, g.w, g.h, g.r);
                Color fg = Mix(kOnSurface, kOnDanger, t);
                cv_.Icon(it.id == kReboot ? icon::Restart : icon::Power, x + 28, y + g.h / 2, 22, fg);
                const char* label = armed_ == it.id ? T->again : (it.id == kReboot ? T->reboot : T->powerOff);
                cv_.Text(x + 48, y + g.h / 2 + 5.5f, cv_.Fit(label, Font::Medium, 15, g.w - 60), Font::Medium, 15, fg);
                DrawFocus(it.id, g);
                break;
            }
            default:
                break;
        }
    }

    // Sensors card (sys-clk).
    float cy = top + kStatsY;
    cv_.FillRoundRect(x0, cy, kCW, kStatsH, 24, kSurfaceLow);
    if (st_.sysclkReady) {
        struct Stat { std::string value; std::string label; };
        auto deg = [](int c) { return c < 0 ? std::string("—") : std::to_string(c) + "°"; };
        char pw[16] = "—";
        if (st_.powerKnown) snprintf(pw, sizeof pw, "%.1f", std::abs(st_.powerMw) / 1000.0);
        std::string mhz = std::string(" · ") + T->mhz;
        Stat stats[6] = {
            {std::to_string(st_.cpuMHz), "CPU" + mhz}, {std::to_string(st_.gpuMHz), "GPU" + mhz},
            {std::to_string(st_.memMHz), "RAM" + mhz}, {deg(st_.socTemp), T->soc},
            {deg(st_.pcbTemp), T->pcb}, {pw, std::string(st_.charging ? T->charge : T->draw) + ", " + T->watt},
        };
        float colW = kCW / 3;
        for (int i = 0; i < 6; i++) {
            float sx = x0 + 18 + (i % 3) * colW, sy = cy + 38 + (i / 3) * 50;
            cv_.Text(sx, sy, stats[i].value, Font::Medium, 21, kOnSurface);
            cv_.Text(sx, sy + 18, cv_.Fit(stats[i].label, Font::Regular, 13, colW - 20), Font::Regular, 13, kMuted);
        }
    } else {
        std::string msg = T->noSysclk;
        size_t colon = msg.find(':');
        if (cv_.Measure(msg, Font::Regular, 14) > kCW - 36 && colon != std::string::npos) {
            cv_.Text(x0 + 18, cy + kStatsH / 2 - 4, msg.substr(0, colon), Font::Regular, 14, kMuted);
            cv_.Text(x0 + 18, cy + kStatsH / 2 + 16, cv_.Fit(msg.substr(colon + 2), Font::Regular, 14, kCW - 36),
                     Font::Regular, 14, kMuted);
        } else {
            cv_.Text(x0 + 18, cy + kStatsH / 2 + 5, msg, Font::Regular, 14, kMuted);
        }
    }

    // Controller hints.
    if (focusVisible_) {
        float hx = x0 + 4;
        float hb = top + kPowerY - 14;
        DrawKeyHint(hx, hb, "A", T->select);
        DrawKeyHint(hx, hb, "X", T->more);
        DrawKeyHint(hx, hb, "B", T->close);
    }
}

void Ui::DrawHeadphones() {
    DrawPageHeader(T->headphones);
    for (const Item& it : items_) {
        if (it.id == kHpToggle) {
            std::string sub = st_.hpConnecting ? T->connecting
                              : st_.hpConnected ? st_.hpName
                              : (st_.hpName.empty() ? T->off : st_.hpName + " · " + T->off);
            if (!st_.btReady) sub = T->noBt;
            else if (!st_.radioOn && !st_.hpConnecting) sub = T->btOff;
            bool on = st_.hpConnected || st_.hpConnecting;
            DrawToggleCard(it, on ? icon::Headphones : icon::HeadsetOff, T->headphones, sub, st_.btReady);
        } else if (it.id >= kDevice0 && it.id < kOverlay0) {
            const AudioDevice& d = st_.paired[size_t(it.id - kDevice0)];
            bool connecting = st_.hpConnecting && !d.connected && d.name == st_.hpName;
            DrawRow(it, icon::Headphones, d.name, d.connected ? T->connected : (connecting ? T->connecting : ""));
        }
    }
    cv_.Text(kO + kPad + 4, kO + 184, st_.paired.empty() ? T->noDevices : T->devices, Font::Medium, 14, kMuted);
    if (st_.autoconnect)
        cv_.Text(kO + kPad + 4, kO + kPH - 26, st_.hpPaused ? T->autoPaused : T->autoOn, Font::Regular, 14, kMuted);
}

void Ui::DrawWifi() {
    DrawPageHeader(T->wifi);
    if (const Item* it = Find(kWifiToggle)) {
        std::string sub = !st_.wifiOn ? T->off : (st_.wifiOnline && !st_.ssid.empty() ? st_.ssid : T->noNetwork);
        DrawToggleCard(*it, st_.wifiOn ? icon::Wifi : icon::WifiOff, T->wifi, sub, st_.wifiReady);
    }
    // Connection details.
    float x = kO + kPad, y = kO + 168;
    cv_.FillRoundRect(x, y, kCW, 3 * 56 + 8, 24, kSurfaceLow);
    bool online = st_.wifiOn && st_.wifiOnline;
    std::string rows[3][2] = {
        {T->network, online && !st_.ssid.empty() ? st_.ssid : T->notConnected},
        {T->signal, online ? T->signalWords[std::clamp(st_.wifiSignal, 0, 3)] : "—"},
        {T->ip, online && !st_.ip.empty() ? st_.ip : "—"},
    };
    for (int i = 0; i < 3; i++) {
        float ry = y + 4 + i * 56;
        cv_.Text(x + 20, ry + 34, rows[i][0], Font::Regular, 15, kMuted);
        std::string v = cv_.Fit(rows[i][1], Font::Medium, 16, kCW - 170);
        cv_.Text(x + kCW - 20 - cv_.Measure(v, Font::Medium, 16), ry + 34, v, Font::Medium, 16, kOnSurface);
        if (i < 2) cv_.FillRect(x + 20, ry + 56, kCW - 40, 1, Hex(0xEBD3CF));
    }
    if (st_.airplane) {
        // Two lines at most: split at the colon like the sys-clk notice.
        std::string msg = T->airplaneOn;
        size_t colon = msg.find(':');
        float ty = y + 3 * 56 + 40;
        if (colon != std::string::npos) {
            cv_.Text(x + 4, ty, msg.substr(0, colon), Font::Medium, 14, kMuted);
            cv_.Text(x + 4, ty + 20, cv_.Fit(msg.substr(colon + 2), Font::Regular, 14, kCW - 8), Font::Regular, 14,
                     kMuted);
        } else {
            cv_.Text(x + 4, ty, cv_.Fit(msg, Font::Regular, 14, kCW - 8), Font::Regular, 14, kMuted);
        }
    }
}

void Ui::DrawOverlays() {
    DrawPageHeader(T->overlays);
    for (const Item& it : items_) {
        if (it.id < kOverlay0) continue;
        const OverlayEntry& o = st_.overlays[size_t(it.id - kOverlay0)];
        DrawRow(it, icon::Layers, o.name, o.version);
    }
    if (st_.overlays.empty()) cv_.Text(kO + kPad + 4, kO + 104, T->noOverlays, Font::Regular, 15, kMuted);
}

}  // namespace qs
