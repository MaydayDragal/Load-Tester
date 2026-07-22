#include "ui.h"

#include <lvgl.h>
#include <stdio.h>
#include <vector>
#include <string>

// Blueprint-ish palette matching the Android app's instrument look.
#define COL_BG      lv_color_hex(0x0B1016)
#define COL_SURFACE lv_color_hex(0x121A23)
#define COL_INK     lv_color_hex(0xE6EDF3)
#define COL_MUTED   lv_color_hex(0x8B98A5)
#define COL_STEEL   lv_color_hex(0x7BA1C9)
#define COL_GREEN   lv_color_hex(0x4CAF50)
#define COL_AMBER   lv_color_hex(0xFFB300)
#define COL_RED     lv_color_hex(0xEF5350)

namespace ui {

static UiActions A;

// ---- Widgets we update live ------------------------------------------------
static lv_obj_t *scrMain;
static lv_obj_t *dotConn, *lblConn, *lblTop;      // top bar
static lv_obj_t *lblV, *lblI, *lblP, *lblMode, *lblExtra, *lblWarn;  // monitor
static lv_obj_t *btnLoad, *lblLoad, *taSetpoint, *lblSetHint;        // control
static lv_obj_t *modeBtns[6];
static lv_obj_t *taFuse, *taSteps, *btnStart, *lblStart, *barTest, *lblTestStatus;  // r-test
static lv_obj_t *listDevices, *dlgConnect;
static lv_obj_t *kb;             // shared numeric on-screen keyboard
static bool lastLoadOn = false;  // live load state, for the toggle button

static const int MODE_IDS[6] = {el15::MODE_CC, el15::MODE_CV, el15::MODE_CR,
                                el15::MODE_CP, el15::MODE_CAP, el15::MODE_DCR};
static const char *MODE_LBL[6] = {"CC", "CV", "CR", "CP", "CAP", "DCR"};

static bool connected = false, testRunning = false, userEditingSetpoint = false;

// ---- Helpers ---------------------------------------------------------------
static lv_obj_t *card(lv_obj_t *parent) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(c, COL_SURFACE, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(0x2A3441), 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_radius(c, 6, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  return c;
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, lv_color_t color, const lv_font_t *font) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, color, 0);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  return l;
}

// ---- On-screen keyboard ----------------------------------------------------
// The board is touch-only, so numeric text fields (setpoint, fuse rating,
// steps) need an on-screen keyboard — without one they can't be filled in and
// the resistance test can't be started. One shared NUMBER-mode keyboard is
// attached to whichever field is focused, and hidden when editing ends.
static void showKeyboard(lv_obj_t *ta) {
  if (!kb) return;
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(kb);
}

static void hideKeyboard() {
  if (!kb) return;
  lv_keyboard_set_textarea(kb, nullptr);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

// Shared field handler: pop the keyboard up while a field is focused and pause
// live setpoint refresh so what the user types isn't overwritten by a poll.
static void taFocusCb(lv_event_t *e) {
  lv_obj_t *ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
    if (ta == taSetpoint) userEditingSetpoint = true;
    showKeyboard(ta);
  } else if (code == LV_EVENT_DEFOCUSED) {
    if (ta == taSetpoint) userEditingSetpoint = false;
    hideKeyboard();
  } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    if (ta == taSetpoint) userEditingSetpoint = false;
    hideKeyboard();
  }
}

// The keyboard's own OK/close (checkmark / close) sends READY/CANCEL to the
// keyboard object — dismiss it and resume live refresh.
static void kbEventCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    hideKeyboard();
    userEditingSetpoint = false;
  }
}

// ---- Connect dialog --------------------------------------------------------
static void deviceClickedCb(lv_event_t *e) {
  const char *addr = (const char *)lv_event_get_user_data(e);
  if (addr && A.connect) A.connect(addr);
  if (dlgConnect) { lv_obj_del(dlgConnect); dlgConnect = nullptr; listDevices = nullptr; }
}

static void openConnectDialog() {
  dlgConnect = lv_obj_create(scrMain);
  lv_obj_set_size(dlgConnect, LV_PCT(92), LV_PCT(80));
  lv_obj_center(dlgConnect);
  lv_obj_set_style_bg_color(dlgConnect, COL_SURFACE, 0);
  lv_obj_set_flex_flow(dlgConnect, LV_FLEX_FLOW_COLUMN);

  label(dlgConnect, "Select device", COL_INK, &lv_font_montserrat_16);
  listDevices = lv_list_create(dlgConnect);
  lv_obj_set_width(listDevices, LV_PCT(100));
  lv_obj_set_flex_grow(listDevices, 1);
  lv_obj_set_style_bg_color(listDevices, COL_BG, 0);

  lv_obj_t *close = lv_btn_create(dlgConnect);
  label(close, "Cancel", COL_INK, nullptr);
  lv_obj_add_event_cb(close, [](lv_event_t *) {
    if (dlgConnect) { lv_obj_del(dlgConnect); dlgConnect = nullptr; listDevices = nullptr; }
  }, LV_EVENT_CLICKED, nullptr);

  if (A.scan) A.scan();
}

// Persist addresses referenced by list buttons for the lifetime of the dialog.
static std::vector<std::string *> g_addrPool;

void onDeviceFound(const char *address, const char *name) {
  if (!listDevices) return;
  std::string *addr = new std::string(address);
  g_addrPool.push_back(addr);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s\n%s", name, address);
  lv_obj_t *b = lv_list_add_btn(listDevices, LV_SYMBOL_BLUETOOTH, buf);
  lv_obj_add_event_cb(b, deviceClickedCb, LV_EVENT_CLICKED, (void *)addr->c_str());
}

void clearDevices() {
  for (auto *p : g_addrPool) delete p;
  g_addrPool.clear();
}

// ---- Control callbacks -----------------------------------------------------
static void modeCb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (A.setMode) A.setMode(MODE_IDS[idx]);
}

static void setpointCb(lv_event_t *) {
  const char *txt = lv_textarea_get_text(taSetpoint);
  float v = atof(txt);
  if (A.setSetpoint) A.setSetpoint(v);
}

static void loadCb(lv_event_t *) {
  if (!connected) { openConnectDialog(); return; }
  if (A.setLoad) A.setLoad(!lastLoadOn);  // toggle against the last live state
}

// ---- Resistance test callbacks --------------------------------------------
static void startTestCb(lv_event_t *) {
  if (testRunning) { if (A.stopRTest) A.stopRTest(); return; }
  if (!connected) { openConnectDialog(); return; }
  float fuse = atof(lv_textarea_get_text(taFuse));
  int steps = atoi(lv_textarea_get_text(taSteps));
  if (fuse <= 0) { lv_label_set_text(lblTestStatus, "Enter the fuse rating (A)"); return; }
  if (steps < 2) steps = 8;
  if (A.startRTest) A.startRTest(fuse, steps);
}

// ---- Build the UI ----------------------------------------------------------
static lv_obj_t *buildMonitorTab(lv_obj_t *tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_t *c = card(tab);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lblV = label(c, "-- V", COL_GREEN, &lv_font_montserrat_28);
  lblI = label(c, "-- A", COL_AMBER, &lv_font_montserrat_28);
  lblP = label(c, "-- W", COL_STEEL, &lv_font_montserrat_20);
  lblMode = label(c, "Mode: --", COL_INK, &lv_font_montserrat_16);
  lblExtra = label(c, "", COL_MUTED, &lv_font_montserrat_12);
  lblWarn = label(c, "", COL_RED, &lv_font_montserrat_16);
  return tab;
}

static lv_obj_t *buildControlTab(lv_obj_t *tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *modeCard = card(tab);
  lv_obj_set_flex_flow(modeCard, LV_FLEX_FLOW_ROW_WRAP);
  for (int i = 0; i < 6; i++) {
    modeBtns[i] = lv_btn_create(modeCard);
    lv_obj_set_width(modeBtns[i], 64);
    label(modeBtns[i], MODE_LBL[i], COL_INK, nullptr);
    lv_obj_add_event_cb(modeBtns[i], modeCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }

  lv_obj_t *spCard = card(tab);
  lv_obj_set_flex_flow(spCard, LV_FLEX_FLOW_COLUMN);
  lblSetHint = label(spCard, "Setpoint", COL_MUTED, &lv_font_montserrat_12);
  taSetpoint = lv_textarea_create(spCard);
  lv_textarea_set_one_line(taSetpoint, true);
  lv_textarea_set_accepted_chars(taSetpoint, "0123456789.");
  lv_obj_set_width(taSetpoint, LV_PCT(100));
  lv_obj_add_event_cb(taSetpoint, taFocusCb, LV_EVENT_ALL, nullptr);
  lv_obj_t *setBtn = lv_btn_create(spCard);
  label(setBtn, "Set", COL_INK, nullptr);
  lv_obj_add_event_cb(setBtn, setpointCb, LV_EVENT_CLICKED, nullptr);

  btnLoad = lv_btn_create(tab);
  lv_obj_set_width(btnLoad, LV_PCT(100));
  lv_obj_set_style_bg_color(btnLoad, COL_GREEN, 0);
  lblLoad = label(btnLoad, "Turn Load ON", COL_INK, &lv_font_montserrat_16);
  lv_obj_add_event_cb(btnLoad, loadCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *lockBtn = lv_btn_create(tab);
  lv_obj_set_width(lockBtn, LV_PCT(100));
  label(lockBtn, "Lock keypad", COL_INK, nullptr);
  lv_obj_add_event_cb(lockBtn, [](lv_event_t *) { if (A.lock) A.lock(); }, LV_EVENT_CLICKED, nullptr);
  return tab;
}

static lv_obj_t *makeField(lv_obj_t *parent, const char *hint, const char *def, const char *accept) {
  lv_obj_t *wrap = lv_obj_create(parent);
  lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(wrap, LV_OPA_0, 0);
  lv_obj_set_style_border_width(wrap, 0, 0);
  lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
  label(wrap, hint, COL_MUTED, &lv_font_montserrat_12);
  lv_obj_t *ta = lv_textarea_create(wrap);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_accepted_chars(ta, accept);
  lv_textarea_set_text(ta, def);
  lv_obj_set_width(ta, LV_PCT(100));
  lv_obj_add_event_cb(ta, taFocusCb, LV_EVENT_ALL, nullptr);
  return ta;
}

static lv_obj_t *buildRTestTab(lv_obj_t *tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_t *c = card(tab);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  label(c, "Circuit resistance test", COL_INK, &lv_font_montserrat_16);
  taFuse = makeField(c, "Fuse rating (A)", "", "0123456789.");
  taSteps = makeField(c, "Steps", "8", "0123456789");

  btnStart = lv_btn_create(c);
  lv_obj_set_width(btnStart, LV_PCT(100));
  lblStart = label(btnStart, "Start test", COL_INK, &lv_font_montserrat_16);
  lv_obj_add_event_cb(btnStart, startTestCb, LV_EVENT_CLICKED, nullptr);

  barTest = lv_bar_create(c);
  lv_obj_set_width(barTest, LV_PCT(100));
  lv_bar_set_range(barTest, 0, 100);
  lv_obj_add_flag(barTest, LV_OBJ_FLAG_HIDDEN);

  lblTestStatus = label(c, "", COL_STEEL, &lv_font_montserrat_12);
  return tab;
}

void begin(const UiActions &actions) {
  A = actions;
  scrMain = lv_scr_act();
  lv_obj_set_style_bg_color(scrMain, COL_BG, 0);
  lv_obj_set_flex_flow(scrMain, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(scrMain, 6, 0);

  // Top bar: connection state + a Connect/Disconnect button.
  lv_obj_t *bar = lv_obj_create(scrMain);
  lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(bar, COL_SURFACE, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  dotConn = lv_obj_create(bar);
  lv_obj_set_size(dotConn, 12, 12);
  lv_obj_set_style_radius(dotConn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dotConn, COL_RED, 0);
  lblConn = label(bar, "Disconnected", COL_INK, &lv_font_montserrat_14);
  lv_obj_set_flex_grow(lblConn, 1);
  lv_obj_t *btnConn = lv_btn_create(bar);
  lblTop = label(btnConn, "Connect", COL_INK, nullptr);
  lv_obj_add_event_cb(btnConn, [](lv_event_t *) {
    if (connected) { if (A.disconnect) A.disconnect(); }
    else openConnectDialog();
  }, LV_EVENT_CLICKED, nullptr);

  // Tabs.
  lv_obj_t *tv = lv_tabview_create(scrMain, LV_DIR_BOTTOM, 40);
  lv_obj_set_flex_grow(tv, 1);
  lv_obj_set_style_bg_color(tv, COL_BG, 0);
  buildMonitorTab(lv_tabview_add_tab(tv, "Monitor"));
  buildControlTab(lv_tabview_add_tab(tv, "Control"));
  buildRTestTab(lv_tabview_add_tab(tv, "R-Test"));

  // Shared numeric keyboard, floating over the bottom half and hidden until a
  // text field is focused (see taFocusCb / kbEventCb).
  kb = lv_keyboard_create(scrMain);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING);   // keep it out of the flex layout
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(kb, kbEventCb, LV_EVENT_ALL, nullptr);
}

// ---- Live data in ----------------------------------------------------------
void onStatus(const el15::Status &s) {
  if (!s.valid) return;
  char b[32];
  snprintf(b, sizeof(b), "%.2f V", s.voltage); lv_label_set_text(lblV, b);
  float curr = (s.mode == el15::MODE_DCR) ? s.dcrI1 : s.current;
  snprintf(b, sizeof(b), "%.3f A", curr); lv_label_set_text(lblI, b);
  snprintf(b, sizeof(b), "%.1f W", s.power); lv_label_set_text(lblP, b);
  snprintf(b, sizeof(b), "Mode: %s", s.modeStr); lv_label_set_text(lblMode, b);

  char extra[96];
  snprintf(extra, sizeof(extra), "Temp %.1f C   Fan %d/%d   %s",
           s.temperature, s.fanSpeed, el15::FAN_SPEED_MAX, s.ready ? "Ready" : "Idle");
  lv_label_set_text(lblExtra, extra);
  lv_label_set_text(lblWarn, s.warning[0] ? s.warning : "");

  // Load button reflects live state.
  lastLoadOn = s.loadOn;
  if (s.loadOn) {
    lv_label_set_text(lblLoad, "Turn Load OFF");
    lv_obj_set_style_bg_color(btnLoad, COL_RED, 0);
  } else {
    lv_label_set_text(lblLoad, "Turn Load ON");
    lv_obj_set_style_bg_color(btnLoad, COL_GREEN, 0);
  }

  if (lblSetHint) {
    snprintf(b, sizeof(b), "%s (%s)", s.setpointLabel, s.setpointUnit);
    lv_label_set_text(lblSetHint, b);
  }
  if (!userEditingSetpoint && s.setpointInPacket) {
    char fmt[16]; snprintf(fmt, sizeof(fmt), "%%.%df", s.setpointDecimals);
    char val[24]; snprintf(val, sizeof(val), fmt, s.setpoint);
    lv_textarea_set_text(taSetpoint, val);
  }
}

void onConnState(int state, const char *info) {
  connected = (state == 3 /* CONNECTED */);
  lv_label_set_text(lblConn, info ? info : "");
  lv_obj_set_style_bg_color(dotConn, connected ? COL_GREEN : COL_RED, 0);
  lv_label_set_text(lblTop, connected ? "Disconnect" : "Connect");
}

void onTestProgress(int step, int total, float target, float v, float i) {
  testRunning = true;
  lv_obj_clear_flag(barTest, LV_OBJ_FLAG_HIDDEN);
  lv_bar_set_value(barTest, total > 0 ? step * 100 / total : 0, LV_ANIM_OFF);
  char b[80];
  snprintf(b, sizeof(b), "Step %d/%d  %.3f A set  %.3f V @ %.3f A", step, total, target, v, i);
  lv_label_set_text(lblTestStatus, b);
  lv_label_set_text(lblStart, "Stop");
}

void onTestComplete(const ResistanceTest::Result &r) {
  testRunning = false;
  lv_obj_add_flag(barTest, LV_OBJ_FLAG_HIDDEN);
  char b[128];
  const char *unit = r.resistanceOhm < 1.0f ? "mOhm" : "Ohm";
  float val = r.resistanceOhm < 1.0f ? r.resistanceOhm * 1000.0f : r.resistanceOhm;
  snprintf(b, sizeof(b), "R = %.3f %s   Voc %.2f V   R2 %.4f%s",
           val, unit, r.openCircuitVoltage, r.rSquared, r.reliable ? "" : "  (low confidence)");
  lv_label_set_text(lblTestStatus, b);
  lv_label_set_text(lblStart, "Start test");
}

void onTestError(const char *msg) {
  testRunning = false;
  lv_obj_add_flag(barTest, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(lblTestStatus, msg);
  lv_label_set_text(lblStart, "Start test");
}

}  // namespace ui
