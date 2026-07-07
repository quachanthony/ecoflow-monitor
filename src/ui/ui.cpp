#include "ui.h"

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include "../config.h"

namespace ui {

static TFT_eSPI tft;
static SPIClass touch_spi(VSPI);
static XPT2046_Touchscreen ts(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

// 320x240 landscape layout
static constexpr int W = 320;
static constexpr int H = 240;

static constexpr uint16_t COL_BG = TFT_BLACK;
static constexpr uint16_t COL_HEADER = 0x18E3;   // dark grey
static constexpr uint16_t COL_TEXT = TFT_WHITE;
static constexpr uint16_t COL_DIM = 0x7BEF;      // mid grey
static constexpr uint16_t COL_OK = 0x0640;       // green
static constexpr uint16_t COL_WARN = 0xFD20;     // amber
static constexpr uint16_t COL_ERR = 0xE8E4;      // red
static constexpr uint16_t COL_BTN_ON = 0x0560;   // green fill
static constexpr uint16_t COL_BTN_OFF = 0x39E7;  // grey fill
static constexpr uint16_t COL_BTN_DISABLED = 0x2124;

// AC/DC output toggle buttons: big targets for a resistive panel in a
// moving van (Phase 5: >=100x80 px)
struct Button {
  int x, y, w, h;
  const char *label;

  Button(int x_, int y_, int w_, int h_, const char *label_)
      : x(x_), y(y_), w(w_), h(h_), label(label_) {}

  enum Visual { V_DISABLED, V_OFF, V_ON, V_PENDING, V_FAILED };  // V_ prefix: OFF/DISABLED are gpio macros

  bool pending = false;
  bool pending_target = false;
  uint32_t pending_since_ms = 0;
  bool failed = false;
  Visual drawn = (Visual)-1;  // force first draw

  bool contains(int px, int py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

static Button ac_btn(10, 130, 145, 86, "AC");
static Button dc_btn(165, 130, 145, 86, "12V");

// change-detection caches for partial redraws
static float drawn_pct = -2.0f;
static int drawn_in = -2, drawn_out = -2;
static int32_t drawn_remain = -2;
static String drawn_status;
static String drawn_detail;
static uint32_t last_refresh_ms = 0;

void begin() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  touch_spi.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  ts.begin(touch_spi);
  ts.setRotation(1);

  tft.fillRect(0, 0, W, 24, COL_HEADER);
  tft.setTextColor(COL_DIM, COL_HEADER);
  tft.setTextDatum(TL_DATUM);

  // static labels
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("IN", 200, 34, 2);
  tft.drawString("OUT", 200, 78, 2);
}

// --- drawing helpers ---

static void drawStatus(const String &text, uint16_t color) {
  if (text == drawn_status) return;
  drawn_status = text;
  tft.fillRect(0, 0, 230, 24, COL_HEADER);
  tft.fillCircle(12, 12, 5, color);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_TEXT, COL_HEADER);
  tft.setTextPadding(0);
  tft.drawString(text, 24, 13, 2);
}

static void drawSn(const String &sn) {
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(COL_DIM, COL_HEADER);
  tft.setTextPadding(0);
  tft.drawString(sn.length() ? sn : "", 312, 13, 2);
}

static void drawBattery(float pct) {
  if (pct == drawn_pct) return;
  drawn_pct = pct;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(pct >= 0 && pct <= 20 ? COL_WARN : COL_TEXT, COL_BG);
  tft.setTextPadding(170);
  String s = pct < 0 ? "--" : String(pct, pct < 99.95f ? 1 : 0);
  tft.drawString(s, 10, 36, 7);
  tft.setTextPadding(0);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("%", 10 + tft.textWidth(s, 7) + 4, 60, 4);
}

static void drawWatts(int in_w, int out_w) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(110);
  if (in_w != drawn_in) {
    drawn_in = in_w;
    tft.setTextColor(in_w > 0 ? COL_OK : COL_TEXT, COL_BG);
    tft.drawString(in_w < 0 ? "--" : String(in_w) + "W", 200, 50, 4);
  }
  if (out_w != drawn_out) {
    drawn_out = out_w;
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(out_w < 0 ? "--" : String(out_w) + "W", 200, 94, 4);
  }
  tft.setTextPadding(0);
}

static void drawRemaining(int32_t minutes, bool charging) {
  if (minutes == drawn_remain) return;
  drawn_remain = minutes;
  String s;
  if (minutes < 0) {
    s = "";
  } else {
    s = String(minutes / 60) + "h" + String(minutes % 60) + "m " +
        (charging ? "to full" : "left");
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextPadding(180);
  tft.drawString(s, 12, 94, 2);
  tft.setTextPadding(0);
}

static void drawDetail(const String &s) {
  if (s == drawn_detail) return;
  drawn_detail = s;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextPadding(300);
  tft.drawString(s, 12, 222, 2);
  tft.setTextPadding(0);
}

static void drawButton(Button &b, Button::Visual v) {
  if (v == b.drawn) return;
  b.drawn = v;

  uint16_t fill;
  String text;
  uint16_t text_col = COL_TEXT;
  switch (v) {
    case Button::V_ON:       fill = COL_BTN_ON;  text = String(b.label) + " ON"; break;
    case Button::V_OFF:      fill = COL_BTN_OFF; text = String(b.label) + " OFF"; break;
    case Button::V_PENDING:  fill = COL_BTN_OFF; text = String(b.label) + " ..."; text_col = COL_DIM; break;
    case Button::V_FAILED:   fill = COL_ERR;     text = "RETRY"; break;
    default:               fill = COL_BTN_DISABLED; text = String(b.label) + " --"; text_col = COL_DIM; break;
  }

  tft.fillRoundRect(b.x, b.y, b.w, b.h, 10, fill);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 10, COL_DIM);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(text_col, fill);
  tft.setTextPadding(0);
  tft.drawString(text, b.x + b.w / 2, b.y + b.h / 2, 4);
}

// --- touch ---

// Returns true once per accepted (debounced) press and fills px/py.
static bool readTouch(uint32_t now_ms, int &px, int &py) {
  static uint32_t last_sample_ms = 0;
  static uint32_t last_accept_ms = 0;
  static int touched_streak = 0;

  if (now_ms - last_sample_ms < 25) return false;
  last_sample_ms = now_ms;

  if (!ts.tirqTouched() || !ts.touched()) {
    touched_streak = 0;
    return false;
  }

  TS_Point p = ts.getPoint();
  touched_streak++;
  // require two consecutive samples (noise filter), rate-limit accepts
  if (touched_streak != 2 || now_ms - last_accept_ms < TOUCH_DEBOUNCE_MS) {
    return false;
  }
  last_accept_ms = now_ms;

  px = map(p.x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, W);
  py = map(p.y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, H);
  px = constrain(px, 0, W - 1);
  py = constrain(py, 0, H - 1);
  Serial.printf("[touch] raw=(%d,%d) -> (%d,%d)\n", p.x, p.y, px, py);
  return true;
}

// --- button state machine (Phase 6: trust telemetry, not the command) ---

static Button::Visual buttonVisual(const Button &b, int port_on, bool usable) {
  if (!usable || port_on < 0) return Button::V_DISABLED;
  if (b.failed) return Button::V_FAILED;
  if (b.pending) return Button::V_PENDING;
  return port_on == 1 ? Button::V_ON : Button::V_OFF;
}

static void resolvePending(Button &b, int port_on, uint32_t port_update_ms,
                           uint32_t now_ms) {
  if (!b.pending) return;
  // confirmed: telemetry newer than the command reports the desired state
  if (port_update_ms >= b.pending_since_ms && port_on >= 0 &&
      (port_on == 1) == b.pending_target) {
    b.pending = false;
    b.failed = false;
    return;
  }
  if (now_ms - b.pending_since_ms > TOGGLE_CONFIRM_TIMEOUT_MS) {
    b.pending = false;
    b.failed = true;  // shown as "RETRY" until tapped or state refreshes
  }
}

static void handleButtonTap(Button &b, int port_on, bool sent_ok,
                            uint32_t now_ms) {
  if (sent_ok) {
    b.pending = true;
    b.failed = false;
    b.pending_since_ms = now_ms;
  } else {
    b.failed = true;
  }
}

void tick(ef::Connection &conn, uint32_t now_ms) {
  const ef::river2::DeviceState &ds = conn.deviceState();
  bool authed = conn.state() == ef::ConnState::AUTHENTICATED;
  bool stale = authed && conn.telemetryAge(now_ms) > STALE_TELEMETRY_MS;
  bool usable = authed && !stale;

  // pending toggles resolve/expire from telemetry regardless of redraw cadence
  resolvePending(ac_btn, ds.ac_on, ds.last_port_state_ms, now_ms);
  resolvePending(dc_btn, ds.dc_on, ds.last_port_state_ms, now_ms);
  // a fresh port-state report clears a stale FAILED marker
  if (ac_btn.failed && ds.last_port_state_ms > ac_btn.pending_since_ms &&
      !ac_btn.pending) {
    ac_btn.failed = false;
  }
  if (dc_btn.failed && ds.last_port_state_ms > dc_btn.pending_since_ms &&
      !dc_btn.pending) {
    dc_btn.failed = false;
  }

  // touch first — snappier than the 1 Hz numeric refresh
  int px, py;
  if (readTouch(now_ms, px, py) && usable) {
    if (ac_btn.contains(px, py) && !ac_btn.pending && ds.ac_on >= 0) {
      bool target = ds.ac_on != 1;
      ac_btn.pending_target = target;
      handleButtonTap(ac_btn, ds.ac_on, conn.sendAcToggle(target), now_ms);
    } else if (dc_btn.contains(px, py) && !dc_btn.pending && ds.dc_on >= 0) {
      bool target = ds.dc_on != 1;
      dc_btn.pending_target = target;
      handleButtonTap(dc_btn, ds.dc_on, conn.sendDcToggle(target), now_ms);
    }
  }

  drawButton(ac_btn, buttonVisual(ac_btn, ds.ac_on, usable));
  drawButton(dc_btn, buttonVisual(dc_btn, ds.dc_on, usable));

  if (now_ms - last_refresh_ms < UI_REFRESH_MS) return;
  last_refresh_ms = now_ms;

  // header status
  if (stale) {
    drawStatus("No signal", COL_WARN);
  } else {
    uint16_t col;
    switch (conn.state()) {
      case ef::ConnState::AUTHENTICATED: col = COL_OK; break;
      case ef::ConnState::AUTH_REJECTED: col = COL_ERR; break;
      case ef::ConnState::BACKOFF:
      case ef::ConnState::SCANNING: col = COL_WARN; break;
      default: col = COL_WARN; break;
    }
    drawStatus(connStateName(conn.state()), col);
  }
  drawSn(conn.deviceSn().c_str());

  drawBattery(usable ? ds.battery_pct : -1.0f);
  drawWatts(usable ? ds.watts_in : -1, usable ? ds.watts_out : -1);

  bool charging = ds.watts_in > 0;
  int32_t remain = charging ? ds.chg_remain_min : ds.dsg_remain_min;
  drawRemaining(usable ? remain : -1, charging);

  if (usable) {
    String detail = "AC " + (ds.ac_out_w < 0 ? "--" : String(ds.ac_out_w)) +
                    "W   12V " +
                    (ds.dc12v_out_w < 0 ? "--" : String(ds.dc12v_out_w)) + "W";
    drawDetail(detail);
  } else if (conn.state() == ef::ConnState::AUTH_REJECTED) {
    drawDetail("Check ECOFLOW_USER_ID / re-pair in app");
  } else if (conn.state() == ef::ConnState::SCANNING) {
    drawDetail("Close the EcoFlow app if it is open");
  } else {
    drawDetail("");
  }
}

}  // namespace ui
