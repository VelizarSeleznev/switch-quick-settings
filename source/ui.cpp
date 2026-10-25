#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qs {

namespace {

// Geometry (layer pixels; the layer is 448x720 at 720p).
constexpr float kMargin = 12;                       // gap between panel and screen edge
constexpr float kPW = Ui::kLayerW - 2 * kMargin;    // panel width
constexpr float kPH = Ui::kLayerH - 2 * kMargin;    // panel height
constexpr float kPad = 20;
constexpr float kCW = kPW - 2 * kPad;               // content width
constexpr float kTileW = (kCW - 10) / 2, kTileH = 76, kGap = 10;
constexpr float kTileR = 24;
constexpr float kSliderH = 56;
constexpr float kStatsY = 440, kStatsH = 124;
constexpr float kPowerH = 52, kPowerY = kPH - kPad - kPowerH;
constexpr float kRowH = 60, kRowStep = 68;
constexpr int kMaxRows = 6;
constexpr double kRefreshEvery = 0.5;

// "Pastel Dream" palette: the cream, rose and cyan of the installed home menu theme.
constexpr Color kPanel = Hex(0xFFF8F3);
constexpr Color kBorder = Hex(0xEDB8C3);
constexpr Color kSurface = Hex(0xF2D8D5);
constexpr Color kSurfaceLow = Hex(0xF9EAE6);
constexpr Color kOnSurface = Hex(0x4A3238);
constexpr Color kMuted = Hex(0x8F747B);
constexpr Color kPrimary = Hex(0xF29BB2);
constexpr Color kOnPrimary = Hex(0x55162A);
constexpr Color kOnPrimaryMuted = Hex(0x7E3A4E);
constexpr Color kDanger = Hex(0xE2667A);
constexpr Color kOnDanger = Hex(0xFFFFFF);
constexpr Color kFocus = Hex(0x4FBFD8);
constexpr Color kPressed = Hex(0x5A1A2D, 0x1C);
constexpr Color kWarn = Hex(0xD9475E);
constexpr Color kKeyCap = Hex(0xEBD5D0);

// Material Symbols code points (subset built by tools/make_fonts.py).
namespace icon {
constexpr uint32_t Headphones = 0xF01F, HeadsetOff = 0xE33A, Wifi = 0xE63E, WifiOff = 0xE648;
constexpr uint32_t Speed = 0xE9E4, Flight = 0xE539, Charging = 0xE1A3, Light = 0xE518, Auto = 0xE1AB;
constexpr uint32_t Power = 0xF8C7, Restart = 0xF053, Chevron = 0xE5CC, Back = 0xE5C4, Layers = 0xE53B;
constexpr uint32_t Bolt = 0xEA0B;
}  // namespace icon

struct Text {
    const char* weekdays[7];
    const char* months[12];
    bool dayFirst;
    const char *headphones, *on, *off, *connecting, *btOff, *wifi, *noNetwork, *airplane, *handheldOnly, *boost,
        *needSysclk, *chargePause, *notCharging, *charging, *pluggedIn, *onBattery, *overlays, *count, *none,
        *reboot, *powerOff, *again, *devices, *noDevices, *autoOn, *autoPaused, *connected, *noOverlays, *select,
        *more, *close, *mhz, *soc, *pcb, *draw, *charge, *watt, *noSysclk, *noBt;
};

const Text kRu = {
    {"Воскресенье", "Понедельник", "Вторник", "Среда", "Четверг", "Пятница", "Суббота"},
    {"января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября",
     "декабря"},
    true,
    "Наушники", "Вкл.", "Выкл.", "Подключение…", "Bluetooth выкл.", "Wi-Fi", "Нет сети", "Режим полёта",
    "Только в руках", "Разгон", "Нужен sys-clk", "Пауза зарядки", "Не заряжается", "Заряжается", "Питание есть",
    "От батареи", "Оверлеи", "%d шт.", "Нет", "Перезагрузка", "Выключение", "Ещё раз", "Сопряжённые устройства",
    "Нет сопряжённых наушников", "Автоподключение включено", "Автоподключение на паузе", "Подключены",
    "Других оверлеев нет", "выбрать", "подробнее", "закрыть", "МГц", "SoC", "Плата", "Расход", "Заряд", "Вт",
    "sys-clk не запущен: разгон и датчики недоступны", "Bluetooth недоступен",
};

const Text kEn = {
    {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
    {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November",
     "December"},
    false,
    "Headphones", "On", "Off", "Connecting…", "Bluetooth off", "Wi-Fi", "No network", "Airplane mode",
    "Handheld only", "Overclock", "Needs sys-clk", "Pause charging", "Not charging", "Charging", "Plugged in",
    "On battery", "Overlays", "%d", "None", "Restart", "Power off", "Tap again", "Paired devices",
    "No paired headphones", "Auto-connect on", "Auto-connect paused", "Connected", "No other overlays", "select",
    "details", "close", "MHz", "SoC", "Board", "Draw", "Charge", "W",
    "sys-clk is not running: overclock and sensors unavailable", "Bluetooth unavailable",
};

inline float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

}  // namespace

static const Text* T = &kEn;

Ui::Ui(System& sys, bool russian)
    : sys_(sys), buf_(size_t(kLayerW) * kLayerH), cv_(kLayerW, kLayerH), ru_(russian) {
    T = ru_ ? &kRu : &kEn;
    cv_.Bind(buf_.data(), false);
}

// ------------------------------------------------------------ open / close

void Ui::Open(Side side, bool tracking, double now) {
    side_ = side;
    visible_ = true;
    page_ = Page::Main;
    focus_ = kHeadphones;
    focusVisible_ = false;
    armed_ = kNone;
    pressed_ = kNone;
    request_ = Request::None;
    onAnim_.clear();
    // Only the cheap part now; the full poll runs once the panel stops moving.
    sys_.RefreshClock(st_);
    lastRefresh_ = now - kRefreshEvery;
    now_ = now;
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
    dirty_ = true;
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

    // Status IPC takes a few milliseconds: never while the panel moves.
    if (now_ - lastRefresh_ >= kRefreshEvery && !animating_ && drag_ == Drag::None) {
        sys_.Refresh(st_);
        lastRefresh_ = now_;
        dirty_ = true;
    }
    if (armed_ != kNone && now_ - armedAt_ > 3) {
        armed_ = kNone;
        dirty_ = true;
    }

    bool closing = animating_ && animTo_ == 0;
    if (!closing) {
        HandleKeys(in);
        HandleTouch(in);
    }
    Layout();

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

void Ui::HandleKeys(const Input& in) {
    uint64_t d = in.down;
    if (!d) return;
    dirty_ = true;
    if (d & key::B) {
        if (page_ != Page::Main) SetPage(Page::Main);
        else Close(now_);
        return;
    }
    uint64_t dirs = key::AnyUp | key::AnyDown | key::AnyLeft | key::AnyRight;
    if (d & dirs) {
        if (!focusVisible_) {
            focusVisible_ = true;
            if (!Find(focus_) && !items_.empty()) focus_ = items_[0].id;
            return;
        }
        if (focus_ == kSlider && (d & (key::AnyLeft | key::AnyRight)) && st_.lblReady && !st_.docked) {
            float v = st_.brightness + ((d & key::AnyRight) ? 0.05f : -0.05f);
            sys_.SetBrightness(st_, std::clamp(v, 0.f, 1.f), true);
            return;
        }
        MoveFocus(d);
        return;
    }
    if (d & key::A) {
        if (!focusVisible_) {
            focusVisible_ = true;
            if (!Find(focus_) && !items_.empty()) focus_ = items_[0].id;
            return;
        }
        Activate(focus_, now_);
    } else if (d & key::X) {
        focusVisible_ = true;
        Secondary(focus_);
    }
}

void Ui::HandleTouch(const Input& in) {
    const Touch& t = in.touch;
    bool left = side_ == Side::Left;
    // A released touch reports no position: use where the finger last was.
    float x = t.down ? t.x : lastX_, y = t.down ? t.y : lastY_;
    float lx = x - LayerX() - PanelX();
    float ly = y - kMargin;

    auto sliderValue = [&](float px) {
        const Item* s = Find(kSlider);
        if (!s) return st_.brightness;
        return std::clamp((px - s->x - kSliderH / 2) / (s->w - kSliderH), 0.f, 1.f);
    };

    if (t.down && !wasDown_) {
        startX_ = lastX_ = t.x;
        startY_ = lastY_ = t.y;
        startT_ = lastT_ = now_;
        vx_ = 0;
        longFired_ = false;
        outside_ = lx < 0 || lx > kPW || ly < 0 || ly > kPH;
        pressed_ = outside_ ? kNone : HitTest(lx, ly);
        focusVisible_ = false;
        if (pressed_ == kSlider && st_.lblReady && !st_.docked) {
            drag_ = Drag::Slider;
            sys_.SetBrightness(st_, sliderValue(lx), false);
        } else {
            drag_ = Drag::Pending;
        }
        dirty_ = true;
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
            case Drag::Slider:
                sys_.SetBrightness(st_, sliderValue(lx), false);
                dirty_ = true;
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
                    dirty_ = true;
                } else if ((pressed_ == kHeadphones || pressed_ == kOverlays) && !longFired_ &&
                           now_ - startT_ > 0.45) {
                    longFired_ = true;
                    int id = pressed_;
                    pressed_ = kNone;
                    Secondary(id);
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
            case Drag::Slider:
                sys_.SetBrightness(st_, st_.brightness, true);
                break;
            case Drag::Pending:
                if (outside_) {
                    Close(now_);
                } else if (!longFired_) {
                    int id = HitTest(lx, ly);
                    if (id != kNone && id == pressed_) Activate(id, now_);
                }
                break;
            case Drag::None:
                break;
        }
        drag_ = Drag::None;
        pressed_ = kNone;
        dirty_ = true;
    }
    wasDown_ = t.down;
}

// ------------------------------------------------------------ actions

void Ui::Activate(int id, double now) {
    dirty_ = true;
    lastRefresh_ = now - kRefreshEvery + 0.15;  // show the real result soon
    if (id != kReboot && id != kPower) armed_ = kNone;
    switch (id) {
        case kHeadphones:
        case kHpToggle:
            if (st_.btReady && st_.radioOn) sys_.SetHeadphones(st_, !(st_.hpConnected || st_.hpConnecting));
            return;
        case kWifi:
            if (st_.wifiReady) sys_.SetWifi(st_, !st_.wifiOn);
            return;
        case kAirplane:
            if (st_.wifiReady && st_.btReady && !st_.docked) sys_.SetAirplane(st_, !st_.airplane);
            return;
        case kBoost:
            if (st_.sysclkReady) sys_.SetBoost(st_, !st_.boost);
            return;
        case kCharge:
            if (st_.chargeCtlReady) sys_.SetChargingEnabled(st_, !st_.chargingEnabled);
            return;
        case kOverlays:
            SetPage(Page::Overlays);
            return;
        case kAuto:
            if (st_.lblReady) sys_.SetAutoBrightness(st_, !st_.autoBrightness);
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
        if (i < st_.paired.size()) sys_.ToggleDevice(st_, st_.paired[i]);
    }
}

void Ui::Secondary(int id) {
    if (id == kHeadphones) SetPage(Page::Headphones);
    else if (id == kOverlays) SetPage(Page::Overlays);
}

void Ui::SetPage(Page p) {
    Page from = page_;
    page_ = p;
    if (p == Page::Headphones) sys_.ListDevices(st_);
    if (p == Page::Overlays) sys_.ListOverlays(st_);
    Layout();
    if (p == Page::Main) focus_ = from == Page::Overlays ? kOverlays : kHeadphones;
    else focus_ = items_.size() > 1 ? items_[1].id : kBack;
    pressed_ = kNone;
    armed_ = kNone;
    dirty_ = true;
}

void Ui::MoveFocus(uint64_t d) {
    const Item* cur = Find(focus_);
    if (!cur) {
        if (!items_.empty()) focus_ = items_[0].id;
        return;
    }
    float cx = cur->x + cur->w / 2, cy = cur->y + cur->h / 2;
    float best = 1e9f;
    int bestId = kNone;
    for (const Item& it : items_) {
        if (it.id == cur->id) continue;
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
            items_.push_back({kSlider, kPad, 108, kCW - kSliderH - 10, kSliderH});
            items_.push_back({kAuto, kPad + kCW - kSliderH, 108, kSliderH, kSliderH});
            const int ids[6] = {kHeadphones, kWifi, kAirplane, kBoost, kCharge, kOverlays};
            for (int i = 0; i < 6; i++)
                items_.push_back({ids[i], kPad + (i % 2) * (kTileW + kGap), 180 + (i / 2) * (kTileH + kGap), kTileW,
                                  kTileH});
            items_.push_back({kReboot, kPad, kPowerY, kTileW, kPowerH});
            items_.push_back({kPower, kPad + kTileW + kGap, kPowerY, kTileW, kPowerH});
            break;
        }
        case Page::Headphones: {
            items_.push_back({kBack, 12, 14, 48, 48});
            items_.push_back({kHpToggle, kPad, 80, kCW, 72});
            int n = std::min<int>(int(st_.paired.size()), kMaxRows - 1);
            for (int i = 0; i < n; i++) items_.push_back({kDevice0 + i, kPad, 196 + i * kRowStep, kCW, kRowH});
            break;
        }
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

float Ui::OnAmount(int id, bool on) {
    float target = on ? 1.f : 0.f;
    auto it = onAnim_.find(id);
    if (it == onAnim_.end()) {
        onAnim_[id] = target;
        return target;
    }
    float& v = it->second;
    float step = float(std::max(0.0, now_ - lastRender_) / 0.16);
    if (v < target) v = std::min(target, v + step);
    else if (v > target) v = std::max(target, v - step);
    if (v != target) tileAnim_ = true;
    return v;
}

// ------------------------------------------------------------ drawing

bool Ui::NeedsPresent() const {
    if (!visible_) return shownVisible_;
    return dirty_ || tileAnim_ || !shownVisible_ || reveal_ != shownReveal_;
}

void Ui::Present(Canvas& screen) {
    if (!visible_ || reveal_ <= 0.001f) {
        screen.Clear();
    } else {
        if (dirty_ || tileAnim_) Render();
        screen.Blit(buf_.data(), int(std::lround(PanelX() - kMargin)));
    }
    shownVisible_ = visible_;
    shownReveal_ = reveal_;
}

void Ui::Render() {
    tileAnim_ = false;
    dirty_ = false;
    cv_.ResetClip();
    cv_.Clear();
    ox_ = kMargin;  // drawn fully open; Present shifts it
    oy_ = kMargin;
    cv_.FillRoundRect(ox_, oy_, kPW, kPH, 30, kPanel);
    cv_.StrokeRoundRect(ox_, oy_, kPW, kPH, 30, 1.5f, kBorder);
    switch (page_) {
        case Page::Main: DrawMain(); break;
        case Page::Headphones: DrawHeadphones(); break;
        case Page::Overlays: DrawOverlays(); break;
    }
    lastRender_ = now_;
}

void Ui::DrawPressed(const Item& it, float r) {
    if (pressed_ == it.id && drag_ == Drag::Pending)
        cv_.FillRoundRect(ox_ + it.x, oy_ + it.y, it.w, it.h, r, kPressed);
}

void Ui::DrawFocus(const Item& it, float r) {
    if (focusVisible_ && focus_ == it.id)
        cv_.StrokeRoundRect(ox_ + it.x - 4, oy_ + it.y - 4, it.w + 8, it.h + 8, r + 4, 3, kFocus);
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

void Ui::DrawTile(const Item& it, uint32_t ic, const char* title, const std::string& sub, bool on, bool enabled,
                  bool chevron) {
    float t = OnAmount(it.id, on && enabled);
    Color bg = Mix(kSurface, kPrimary, t);
    Color fg = Mix(kOnSurface, kOnPrimary, t);
    Color sc = Mix(kMuted, kOnPrimaryMuted, t);
    if (!enabled) {
        bg = kSurfaceLow;
        fg = Fade(kOnSurface, 0.4f);
        sc = Fade(kMuted, 0.6f);
    }
    float x = ox_ + it.x, y = oy_ + it.y;
    cv_.FillRoundRect(x, y, it.w, it.h, kTileR, bg);
    DrawPressed(it, kTileR);
    cv_.Icon(ic, x + 30, y + it.h / 2, 26, fg);
    float textX = x + 54, maxW = it.w - 54 - (chevron ? 30 : 14);
    cv_.Text(textX, y + 33, cv_.Fit(title, Font::Medium, 16.5f, maxW), Font::Medium, 16.5f, fg);
    cv_.Text(textX, y + 55, cv_.Fit(sub, Font::Regular, 14, maxW), Font::Regular, 14, sc);
    if (chevron) cv_.Icon(icon::Chevron, x + it.w - 20, y + it.h / 2, 22, Fade(fg, 0.8f));
    DrawFocus(it, kTileR);
}

void Ui::DrawRow(const Item& it, uint32_t ic, const std::string& title, const std::string& sub, bool on) {
    float t = OnAmount(it.id, on);
    Color bg = Mix(kSurface, kPrimary, t);
    Color fg = Mix(kOnSurface, kOnPrimary, t);
    Color sc = Mix(kMuted, kOnPrimaryMuted, t);
    float x = ox_ + it.x, y = oy_ + it.y;
    cv_.FillRoundRect(x, y, it.w, it.h, 20, bg);
    DrawPressed(it, 20);
    cv_.Icon(ic, x + 30, y + it.h / 2, 24, fg);
    float maxW = it.w - 70;
    if (sub.empty()) {
        cv_.Text(x + 56, y + it.h / 2 + 6, cv_.Fit(title, Font::Medium, 16, maxW), Font::Medium, 16, fg);
    } else {
        cv_.Text(x + 56, y + 27, cv_.Fit(title, Font::Medium, 16, maxW), Font::Medium, 16, fg);
        cv_.Text(x + 56, y + 46, cv_.Fit(sub, Font::Regular, 13.5f, maxW), Font::Regular, 13.5f, sc);
    }
    DrawFocus(it, 20);
}

void Ui::DrawPageHeader(const char* title) {
    if (const Item* b = Find(kBack)) {
        float cx = ox_ + b->x + b->w / 2, cy = oy_ + b->y + b->h / 2;
        cv_.FillCircle(cx, cy, b->w / 2, kSurface);
        if (pressed_ == kBack && drag_ == Drag::Pending) cv_.FillCircle(cx, cy, b->w / 2, kPressed);
        cv_.Icon(icon::Back, cx, cy, 24, kOnSurface);
        DrawFocus(*b, b->w / 2);
    }
    cv_.Text(ox_ + 70, oy_ + 46, title, Font::Medium, 22, kOnSurface);
}

void Ui::DrawMain() {
    char buf[96];
    const float x0 = ox_ + kPad, top = oy_;

    // Header: clock, date, battery.
    snprintf(buf, sizeof buf, "%02d:%02d", st_.hour, st_.minute);
    cv_.Text(x0, top + 62, buf, Font::Medium, 44, kOnSurface);
    const char* wd = T->weekdays[std::clamp(st_.weekday, 0, 6)];
    const char* mo = T->months[std::clamp(st_.month - 1, 0, 11)];
    if (T->dayFirst) snprintf(buf, sizeof buf, "%s, %d %s", wd, st_.day, mo);
    else snprintf(buf, sizeof buf, "%s, %s %d", wd, mo, st_.day);
    cv_.Text(x0 + 1, top + 88, buf, Font::Regular, 16, kMuted);
    DrawBattery(ox_ + kPW - kPad, top + 42);

    // Brightness.
    bool brOk = st_.lblReady && !st_.docked;
    if (const Item* s = Find(kSlider)) {
        float x = ox_ + s->x, y = top + s->y;
        float fillW = kSliderH + (s->w - kSliderH) * std::clamp(st_.brightness, 0.f, 1.f);
        cv_.FillRoundRect(x, y, s->w, s->h, kSliderH / 2, brOk ? kSurface : kSurfaceLow);
        cv_.FillRoundRect(x, y, fillW, s->h, kSliderH / 2, brOk ? kPrimary : Fade(kPrimary, 0.4f));
        cv_.Icon(icon::Light, x + kSliderH / 2, y + s->h / 2, 24, kOnPrimary);
        DrawFocus(*s, kSliderH / 2);
    }
    if (const Item* a = Find(kAuto)) {
        float t = OnAmount(kAuto, st_.autoBrightness);
        float cx = ox_ + a->x + a->w / 2, cy = top + a->y + a->h / 2;
        cv_.FillCircle(cx, cy, a->w / 2, st_.lblReady ? Mix(kSurface, kPrimary, t) : kSurfaceLow);
        if (pressed_ == kAuto && drag_ == Drag::Pending) cv_.FillCircle(cx, cy, a->w / 2, kPressed);
        cv_.Icon(icon::Auto, cx, cy, 24, st_.lblReady ? Mix(kOnSurface, kOnPrimary, t) : Fade(kOnSurface, 0.4f));
        DrawFocus(*a, a->w / 2);
    }

    // Tiles.
    for (const Item& it : items_) {
        switch (it.id) {
            case kHeadphones: {
                bool usable = st_.btReady && st_.radioOn;
                std::string sub;
                if (!st_.btReady) sub = T->noBt;
                else if (!st_.radioOn) sub = T->btOff;
                else if (st_.hpConnecting) sub = T->connecting;
                else if (st_.hpConnected) sub = st_.hpName;
                else sub = T->off;
                bool on = st_.hpConnected || st_.hpConnecting;
                DrawTile(it, on ? icon::Headphones : icon::HeadsetOff, T->headphones, sub, on, usable, true);
                break;
            }
            case kWifi: {
                std::string sub = !st_.wifiOn ? T->off : (st_.wifiOnline && !st_.ssid.empty() ? st_.ssid : T->noNetwork);
                DrawTile(it, st_.wifiOn ? icon::Wifi : icon::WifiOff, T->wifi, sub, st_.wifiOn, st_.wifiReady, false);
                break;
            }
            case kAirplane: {
                bool usable = st_.wifiReady && st_.btReady && !st_.docked;
                const char* sub = st_.docked ? T->handheldOnly : (st_.airplane ? T->on : T->off);
                DrawTile(it, icon::Flight, T->airplane, sub, st_.airplane, usable, false);
                break;
            }
            case kBoost: {
                std::string sub;
                if (!st_.sysclkReady) sub = T->needSysclk;
                else if (st_.boost) {
                    snprintf(buf, sizeof buf, "%u · %u %s", st_.cpuMHz, st_.gpuMHz, T->mhz);
                    sub = buf;
                } else sub = T->off;
                DrawTile(it, icon::Speed, T->boost, sub, st_.boost, st_.sysclkReady, false);
                break;
            }
            case kCharge: {
                bool paused = !st_.chargingEnabled;
                std::string sub = paused ? T->notCharging
                                         : (st_.charging ? T->charging : (st_.pluggedIn ? T->pluggedIn : T->onBattery));
                DrawTile(it, icon::Charging, T->chargePause, sub, paused, st_.chargeCtlReady, false);
                break;
            }
            case kOverlays: {
                std::string sub = T->none;
                if (!st_.overlays.empty()) {
                    snprintf(buf, sizeof buf, T->count, int(st_.overlays.size()));
                    sub = buf;
                }
                DrawTile(it, icon::Layers, T->overlays, sub, false, true, true);
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
            float sx = x0 + 18 + (i % 3) * colW, sy = cy + 40 + (i / 3) * 56;
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
        float hb = top + kPowerY - 18;
        DrawKeyHint(hx, hb, "A", T->select);
        DrawKeyHint(hx, hb, "X", T->more);
        DrawKeyHint(hx, hb, "B", T->close);
    }

    // Power buttons.
    for (int id : {kReboot, kPower}) {
        const Item* it = Find(id);
        if (!it) continue;
        bool armed = armed_ == id;
        float x = ox_ + it->x, y = top + it->y;
        cv_.FillRoundRect(x, y, it->w, it->h, it->h / 2, armed ? kDanger : kSurface);
        DrawPressed(*it, it->h / 2);
        Color fg = armed ? kOnDanger : kOnSurface;
        cv_.Icon(id == kReboot ? icon::Restart : icon::Power, x + 28, y + it->h / 2, 22, fg);
        const char* label = armed ? T->again : (id == kReboot ? T->reboot : T->powerOff);
        cv_.Text(x + 48, y + it->h / 2 + 5.5f, cv_.Fit(label, Font::Medium, 15, it->w - 60), Font::Medium, 15, fg);
        DrawFocus(*it, it->h / 2);
    }
}

void Ui::DrawHeadphones() {
    DrawPageHeader(T->headphones);
    for (const Item& it : items_) {
        if (it.id == kHpToggle) {
            bool on = st_.hpConnected || st_.hpConnecting;
            std::string sub = st_.hpConnecting ? T->connecting
                              : st_.hpConnected ? st_.hpName
                              : (st_.hpName.empty() ? T->off : st_.hpName + " · " + T->off);
            if (!st_.btReady) sub = T->noBt;
            else if (!st_.radioOn) sub = T->btOff;
            float t = OnAmount(it.id, on);
            float x = ox_ + it.x, y = oy_ + it.y;
            cv_.FillRoundRect(x, y, it.w, it.h, kTileR, Mix(kSurface, kPrimary, t));
            DrawPressed(it, kTileR);
            Color fg = Mix(kOnSurface, kOnPrimary, t);
            cv_.Icon(on ? icon::Headphones : icon::HeadsetOff, x + 32, y + it.h / 2, 28, fg);
            cv_.Text(x + 60, y + 31, T->headphones, Font::Medium, 18, fg);
            cv_.Text(x + 60, y + 53, cv_.Fit(sub, Font::Regular, 14, it.w - 130), Font::Regular, 14,
                     Mix(kMuted, kOnPrimaryMuted, t));
            // Switch knob.
            float sw = 52, sh = 32, sx = x + it.w - sw - 16, sy = y + (it.h - sh) / 2;
            cv_.FillRoundRect(sx, sy, sw, sh, sh / 2, Mix(Hex(0xE3CCC7), kOnPrimary, t));
            float kx = sx + sh / 2 + t * (sw - sh);
            cv_.FillCircle(kx, sy + sh / 2, 10 + 2 * t, Mix(Hex(0xFFFFFF), kPrimary, t));
            DrawFocus(it, kTileR);
        } else if (it.id >= kDevice0 && it.id < kOverlay0) {
            const AudioDevice& d = st_.paired[size_t(it.id - kDevice0)];
            bool connecting = st_.hpConnecting && !d.connected && d.name == st_.hpName;
            DrawRow(it, icon::Headphones, d.name, d.connected ? T->connected : (connecting ? T->connecting : ""),
                    d.connected);
        }
    }
    cv_.Text(ox_ + kPad + 4, oy_ + 184, st_.paired.empty() ? T->noDevices : T->devices, Font::Medium, 14, kMuted);
    if (st_.autoconnect)
        cv_.Text(ox_ + kPad + 4, oy_ + kPH - 26, st_.hpPaused ? T->autoPaused : T->autoOn, Font::Regular, 14, kMuted);
}

void Ui::DrawOverlays() {
    DrawPageHeader(T->overlays);
    for (const Item& it : items_) {
        if (it.id < kOverlay0) continue;
        const OverlayEntry& o = st_.overlays[size_t(it.id - kOverlay0)];
        DrawRow(it, icon::Layers, o.name, o.version, false);
    }
    if (st_.overlays.empty()) cv_.Text(ox_ + kPad + 4, oy_ + 104, T->noOverlays, Font::Regular, 15, kMuted);
}

}  // namespace qs
