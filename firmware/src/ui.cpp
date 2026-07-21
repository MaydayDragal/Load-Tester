// LVGL UI for the EL15 controller - v2 "Focus" (368x448 portrait).
//
// No persistent tab bar: one task fills the screen; a full-screen grid Menu
// (button top-right of the status strip) reaches any destination in <=2 taps,
// and a one-tap back arrow returns to Monitor. See README_v2_focus handoff for
// the spec. Fonts use bundled Montserrat as the Inter/JetBrains-Mono stand-in
// (48 px is the largest built-in, standing in for the 64 px hero); icons use the
// nearest LVGL built-in symbols (Phosphor glyph embed is a later pass).

#include "ui.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

namespace ui {

// ---- Palette ---------------------------------------------------------------
#define COL_BLACK   lv_color_hex(0x000000)
#define COL_CHROME  lv_color_hex(0x0A0E13)
#define COL_CARD    lv_color_hex(0x121A23)
#define COL_READOUT lv_color_hex(0x0B1016)
#define COL_INSET   lv_color_hex(0x0D141B)
#define COL_BORDER  lv_color_hex(0x2A3441)
#define COL_BORDER2 lv_color_hex(0x1C2530)
#define COL_BORDER3 lv_color_hex(0x14181F)
#define COL_INK     lv_color_hex(0xE6EDF3)
#define COL_MUTED   lv_color_hex(0x8B98A5)
#define COL_FAINT   lv_color_hex(0x5C6672)
#define COL_ACCENT  lv_color_hex(0x9184D9)
#define COL_ACCENT2 lv_color_hex(0xC9C2F2)
#define COL_GREEN   lv_color_hex(0x4CAF50)
#define COL_AMBER   lv_color_hex(0xFFB300)
#define COL_RED     lv_color_hex(0xEF5350)
#define COL_VHERO_BG lv_color_hex(0x0B120B)
#define COL_VHERO_BD lv_color_hex(0x14361A)
#define COL_IHERO_BG lv_color_hex(0x141005)
#define COL_IHERO_BD lv_color_hex(0x3A2C0A)
#define COL_IHERO_BG_ON lv_color_hex(0x180A0B)
#define COL_IHERO_BD_ON lv_color_hex(0x5A1E20)
#define COL_DARKINK lv_color_hex(0x12101F)

#define F12 &lv_font_montserrat_12
#define F14 &lv_font_montserrat_14
#define F16 &lv_font_montserrat_16
#define F20 &lv_font_montserrat_20
#define F24 &lv_font_montserrat_24
#define F28 &lv_font_montserrat_28
#define F34 &lv_font_montserrat_34
#define F40 &lv_font_montserrat_40
#define F44 &lv_font_montserrat_44
#define F48 &lv_font_montserrat_48

static UiActions A;

// ---- State -----------------------------------------------------------------
enum Screen { SCR_MON, SCR_ADJ, SCR_GRAPH, SCR_RTEST, SCR_CONNECT };
enum Overlay { OV_NONE, OV_MENU, OV_KEYPAD, OV_PICKER };
enum RtPhase { RT_IDLE, RT_RUN, RT_RESULT };

static const int MODE_RT = 0xF0;   // UI-only pseudo-mode (drives the R-Test engine)

static Screen  curScreen = SCR_MON;
static Overlay curOverlay = OV_NONE;
static int     curMode = el15::MODE_CC;   // el15 mode or MODE_RT
static bool    connected = false;
static bool    lastLoadOn = false;
static el15::Status lastStatus;

static float   setpoint = 0;
static float   stepSize = 0.1f;
static int     kpTarget = 0;               // 1 setpoint, 2 fuse
static std::string kpBuf;

static float   fuseRating = 0;
static int     rtSteps = 8;
static RtPhase rtPhase = RT_IDLE;
static ResistanceTest::Result lastResult;
static bool    rtSaved = false;
static int     rtSeq = 1;

static const int HIST_N = 60;
static float vHist[HIST_N], iHist[HIST_N];
static int   histCount = 0;

static const float FUSE_RATINGS[] = {1, 2, 3, 5, 7.5f, 10, 15, 20, 25, 30, 40};
static const int   FUSE_N = 11;

// modes incl. RT
static const int   MODE_IDS[7]  = {el15::MODE_CC, el15::MODE_CV, el15::MODE_CR,
                                   el15::MODE_CP, el15::MODE_CAP, el15::MODE_DCR, MODE_RT};
static const char *MODE_ABBR[7] = {"CC", "CV", "CR", "CP", "CAP", "DCR", "RT"};
static const char *MODE_NAME[7] = {"Constant Current", "Constant Voltage", "Constant Resistance",
                                   "Constant Power", "Capacity", "DC Resistance", "Resistance Test"};

// ---- Per-unit config (min,max,dp, 3 step sizes + default idx, 4 presets) ----
struct UnitCfg { float lo, hi; int dp; float step[3]; int defStep; float preset[4]; };
static UnitCfg unitCfg(const char *u) {
  if (strcmp(u, "V") == 0)    return {0, 150, 1, {0.1f, 1, 10}, 1, {3.7f, 5, 12, 24}};
  if (strcmp(u, "ohm") == 0)  return {0.05f, 9999, 1, {0.1f, 1, 10}, 1, {1, 5, 10, 50}};
  if (strcmp(u, "W") == 0)    return {0, 400, 0, {1, 10, 50}, 1, {10, 25, 50, 100}};
  return {0, 40, 2, {0.01f, 0.1f, 1}, 1, {0.5f, 1, 2, 5}};  // A (default)
}
static bool isRT() { return curMode == MODE_RT; }
static const char *modeUnit() {
  if (isRT()) return "A";
  return el15::setpointInfo(curMode).unit;   // "A"/"V"/"ohm"/"W"
}
static const char *modeAbbr() {
  for (int i = 0; i < 7; i++) if (MODE_IDS[i] == curMode) return MODE_ABBR[i];
  return "?";
}
static const char *modeName() {
  for (int i = 0; i < 7; i++) if (MODE_IDS[i] == curMode) return MODE_NAME[i];
  return "";
}

// ---- Widget handles --------------------------------------------------------
static lv_obj_t *scrRoot, *contentStack;
static lv_obj_t *monScreen, *adjScreen, *graphScreen, *rtestScreen, *connectScreen;
static lv_obj_t *menuOverlay, *kpOverlay, *pickerOverlay;

static lv_obj_t *stDot, *stConnLabel, *stConnGroup, *stBack, *stBackLabel, *stTempChip, *stTempLabel, *stMenuBtn;
static lv_obj_t *infoBar, *ibPower, *ibFan, *ibRuntime, *ibExtra;
static lv_obj_t *faultBanner, *faultTitle, *faultMsg;

static lv_obj_t *modeAbbrLbl, *modeNameLbl, *setLabelLbl, *setValLbl, *setUnitLbl;
static lv_obj_t *vHeroVal, *iHeroBlock, *iHeroLabelRow, *iHeroSink, *iHeroVal;
static lv_obj_t *loadBar, *loadBtn, *loadIcon, *loadTitle, *loadSub;

static lv_obj_t *adjCaption, *adjVal, *adjUnit, *stepChip[3], *stepChipLbl[3];
static lv_obj_t *graphVNum, *graphINum, *chart, *gVRange, *gIRange, *gWin;
static lv_chart_series_t *serV, *serI;

static lv_obj_t *rtIdleBox, *rtRunBox, *rtResultBox;
static lv_obj_t *fuseVal, *stepsVal, *startBtn, *startBtnLbl;
static lv_obj_t *runStepLbl, *runBar, *runVLbl, *runILbl;
static lv_obj_t *resistVal, *lowConfBox, *resultList, *saveBtn, *saveBtnLbl, *rtStatusLbl;

static lv_obj_t *connDot2, *connLabel2, *connDisc, *scanBtn, *deviceList;
static lv_obj_t *kpTitle, *kpValue, *kpUnit, *kpPreset[4], *kpPresetBtn[4];
static lv_obj_t *modeTile[7];

// ---- Forward declarations --------------------------------------------------
static void showScreen(Screen s);
static void showOverlay(Overlay o);
static void openKeypad(int target);
static void refreshMonitor();
static void refreshAdjust();
static void refreshRtest();
static void refreshChart();
static void refreshPicker();
static void addDeviceRow(const char *sym, const char *name, const char *sub, const char *addr);

// ---- Small builders --------------------------------------------------------
static lv_obj_t *cont(lv_obj_t *p) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_pad_gap(o, 0, 0);   // kill the theme's default flex row/col gap
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
static lv_obj_t *lbl(lv_obj_t *p, const char *t, lv_color_t c, const lv_font_t *f) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_color(l, c, 0);
  if (f) lv_obj_set_style_text_font(l, f, 0);
  return l;
}
static void styleCard(lv_obj_t *o, lv_color_t bg, lv_color_t border, int radius, int pad) {
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(o, border, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, pad, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t *flatBtn(lv_obj_t *p) {
  lv_obj_t *b = lv_btn_create(p);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_pad_gap(b, 0, 0);
  lv_obj_set_style_opa(b, LV_OPA_60, LV_STATE_PRESSED);  // instant press feedback
  return b;
}
static void fmtVal(char *b, int n, float v, int dp) { snprintf(b, n, "%.*f", dp, v); }

// ---- Navigation ------------------------------------------------------------
static void showOverlay(Overlay o) {
  curOverlay = o;
  lv_obj_t *ovs[3] = {menuOverlay, kpOverlay, pickerOverlay};
  Overlay ids[3] = {OV_MENU, OV_KEYPAD, OV_PICKER};
  for (int i = 0; i < 3; i++) {
    if (ids[i] == o) { lv_obj_clear_flag(ovs[i], LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(ovs[i]); }
    else lv_obj_add_flag(ovs[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void showScreen(Screen s) {
  curScreen = s;
  showOverlay(OV_NONE);
  lv_obj_t *all[5] = {monScreen, adjScreen, graphScreen, rtestScreen, connectScreen};
  Screen ids[5] = {SCR_MON, SCR_ADJ, SCR_GRAPH, SCR_RTEST, SCR_CONNECT};
  for (int i = 0; i < 5; i++) {
    if (ids[i] == s) lv_obj_clear_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
  }

  bool body = (s == SCR_MON || s == SCR_ADJ || s == SCR_GRAPH);
  if (body && connected) lv_obj_clear_flag(infoBar, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(infoBar, LV_OBJ_FLAG_HIDDEN);
  if (body) lv_obj_clear_flag(loadBar, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(loadBar, LV_OBJ_FLAG_HIDDEN);

  // status strip left: connection group on Monitor, else a back-to-Monitor pill.
  if (s == SCR_MON) {
    lv_obj_clear_flag(stConnGroup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(stBack, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(stConnGroup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(stBack, LV_OBJ_FLAG_HIDDEN);
    const char *title = s == SCR_ADJ ? LV_SYMBOL_LEFT " Adjust" : s == SCR_GRAPH ? LV_SYMBOL_LEFT " Graph"
                        : s == SCR_RTEST ? LV_SYMBOL_LEFT " R-Test" : LV_SYMBOL_LEFT " Connect";
    lv_label_set_text(stBackLabel, title);
  }
  if (s == SCR_MON) refreshMonitor();
  if (s == SCR_ADJ) refreshAdjust();
  if (s == SCR_GRAPH) refreshChart();
}

// ---- Status strip ----------------------------------------------------------
static void buildStatusStrip() {
  lv_obj_t *strip = cont(scrRoot);
  lv_obj_set_size(strip, LV_PCT(100), 42);
  lv_obj_set_style_bg_color(strip, COL_CHROME, 0);
  lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(strip, COL_BORDER2, 0);
  lv_obj_set_style_border_width(strip, 1, 0);
  lv_obj_set_style_border_side(strip, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_pad_hor(strip, 11, 0);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(strip, 7, 0);

  // conn group (Monitor)
  stConnGroup = cont(strip);
  lv_obj_set_size(stConnGroup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(stConnGroup, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stConnGroup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(stConnGroup, 8, 0);
  lv_obj_add_flag(stConnGroup, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(stConnGroup, [](lv_event_t *) { showScreen(SCR_CONNECT); }, LV_EVENT_CLICKED, nullptr);
  stDot = lv_obj_create(stConnGroup);
  lv_obj_set_size(stDot, 11, 11);
  lv_obj_set_style_radius(stDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(stDot, 0, 0);
  lv_obj_set_style_bg_color(stDot, COL_MUTED, 0);
  stConnLabel = lbl(stConnGroup, "Disconnected", COL_INK, F16);

  // back pill (non-Monitor)
  stBack = flatBtn(strip);
  lv_obj_set_size(stBack, LV_SIZE_CONTENT, 34);
  styleCard(stBack, COL_INSET, COL_BORDER, 9, 0);
  lv_obj_set_style_pad_hor(stBack, 10, 0);
  stBackLabel = lbl(stBack, LV_SYMBOL_LEFT " Back", COL_ACCENT2, F16);
  lv_obj_center(stBackLabel);
  lv_obj_add_event_cb(stBack, [](lv_event_t *) { showScreen(SCR_MON); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(stBack, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *sp = cont(strip); lv_obj_set_flex_grow(sp, 1); lv_obj_set_height(sp, 1);

  stTempChip = cont(strip);
  lv_obj_set_size(stTempChip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  stTempLabel = lbl(stTempChip, "--", COL_INK, F14);
  lv_obj_add_flag(stTempChip, LV_OBJ_FLAG_HIDDEN);

  stMenuBtn = flatBtn(strip);
  lv_obj_set_size(stMenuBtn, 38, 34);
  styleCard(stMenuBtn, COL_INSET, COL_BORDER, 9, 0);
  lv_obj_t *ml = lbl(stMenuBtn, LV_SYMBOL_LIST, COL_ACCENT, F20); lv_obj_center(ml);
  lv_obj_add_event_cb(stMenuBtn, [](lv_event_t *) { showOverlay(OV_MENU); }, LV_EVENT_CLICKED, nullptr);
}

// ---- Info bar --------------------------------------------------------------
static void buildInfoBar() {
  infoBar = cont(scrRoot);
  lv_obj_set_size(infoBar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(infoBar, COL_CHROME, 0);
  lv_obj_set_style_bg_opa(infoBar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(infoBar, COL_BORDER3, 0);
  lv_obj_set_style_border_width(infoBar, 1, 0);
  lv_obj_set_style_border_side(infoBar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_pad_hor(infoBar, 11, 0);
  lv_obj_set_style_pad_ver(infoBar, 5, 0);
  lv_obj_set_flex_flow(infoBar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(infoBar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  ibPower = lbl(infoBar, "0.0 W", COL_INK, F14);
  ibFan = lbl(infoBar, LV_SYMBOL_REFRESH " 0/5", COL_MUTED, F14);
  ibRuntime = lbl(infoBar, LV_SYMBOL_LOOP " 00:00", COL_MUTED, F14);
  ibExtra = lbl(infoBar, "", COL_MUTED, F14);
  lv_obj_add_flag(infoBar, LV_OBJ_FLAG_HIDDEN);
}

// ---- Fault banner ----------------------------------------------------------
static void buildFaultBanner() {
  faultBanner = cont(scrRoot);
  lv_obj_set_size(faultBanner, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(faultBanner, COL_RED, 0);
  lv_obj_set_style_bg_opa(faultBanner, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(faultBanner, 9, 0);
  lv_obj_set_flex_flow(faultBanner, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(faultBanner, [](lv_event_t *) { lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, nullptr);
  faultTitle = lbl(faultBanner, LV_SYMBOL_WARNING "  -- - PROTECTION", lv_color_hex(0x1a0606), F16);
  faultMsg = lbl(faultBanner, "", lv_color_hex(0x1a0606), F12);
  lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
}

// ---- Monitor ---------------------------------------------------------------
static void buildMonitor() {
  monScreen = cont(contentStack);
  lv_obj_set_size(monScreen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(monScreen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(monScreen, 11, 0);
  lv_obj_set_style_pad_row(monScreen, 7, 0);

  // Mode | Set bar
  lv_obj_t *bar = cont(monScreen);
  lv_obj_set_size(bar, LV_PCT(100), 70);
  styleCard(bar, COL_CARD, COL_BORDER, 14, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_t *ml = flatBtn(bar);
  lv_obj_set_size(ml, LV_PCT(50), LV_PCT(100));
  lv_obj_set_style_pad_all(ml, 10, 0);
  lv_obj_set_flex_flow(ml, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ml, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  modeAbbrLbl = lbl(ml, "CC", COL_ACCENT, F28);
  modeNameLbl = lbl(ml, "Constant Current", COL_MUTED, F12);
  lv_obj_add_event_cb(ml, [](lv_event_t *) { refreshPicker(); showOverlay(OV_PICKER); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *sr = flatBtn(bar);
  lv_obj_set_size(sr, LV_PCT(50), LV_PCT(100));
  lv_obj_set_style_pad_all(sr, 10, 0);
  lv_obj_set_style_border_color(sr, COL_BORDER, 0);
  lv_obj_set_style_border_width(sr, 1, 0);
  lv_obj_set_style_border_side(sr, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_radius(sr, 0, 0);
  lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  setLabelLbl = lbl(sr, "SET", COL_MUTED, F12);
  lv_obj_t *svrow = cont(sr);
  lv_obj_set_size(svrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(svrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(svrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(svrow, 4, 0);
  setValLbl = lbl(svrow, "0.00", COL_INK, F28);
  setUnitLbl = lbl(svrow, "A", COL_ACCENT, F16);
  lv_obj_add_event_cb(sr, [](lv_event_t *) {
    if (isRT()) {
      int idx = -1; for (int i = 0; i < FUSE_N; i++) if (FUSE_RATINGS[i] == fuseRating) idx = i;
      fuseRating = FUSE_RATINGS[(idx + 1) % FUSE_N]; refreshMonitor();
    } else showScreen(SCR_ADJ);
  }, LV_EVENT_CLICKED, nullptr);

  // Voltage hero
  lv_obj_t *vh = cont(monScreen);
  lv_obj_set_width(vh, LV_PCT(100));
  lv_obj_set_flex_grow(vh, 1);
  styleCard(vh, COL_VHERO_BG, COL_VHERO_BD, 14, 12);
  lv_obj_set_flex_flow(vh, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(vh, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lbl(vh, "VOLTAGE", COL_GREEN, F12);
  lv_obj_t *vrow = cont(vh);
  lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(vrow, 5, 0);
  vHeroVal = lbl(vrow, "--", COL_GREEN, F48);
  lbl(vrow, "V", COL_GREEN, F24);

  // Current hero
  iHeroBlock = cont(monScreen);
  lv_obj_set_width(iHeroBlock, LV_PCT(100));
  lv_obj_set_flex_grow(iHeroBlock, 1);
  styleCard(iHeroBlock, COL_IHERO_BG, COL_IHERO_BD, 14, 12);
  lv_obj_set_flex_flow(iHeroBlock, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(iHeroBlock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  iHeroLabelRow = cont(iHeroBlock);
  lv_obj_set_size(iHeroLabelRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(iHeroLabelRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(iHeroLabelRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(iHeroLabelRow, 8, 0);
  lbl(iHeroLabelRow, "CURRENT", COL_AMBER, F12);
  iHeroSink = lbl(iHeroLabelRow, LV_SYMBOL_CHARGE " SINKING", COL_RED, F12);
  lv_obj_add_flag(iHeroSink, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *irow = cont(iHeroBlock);
  lv_obj_set_size(irow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(irow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(irow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(irow, 5, 0);
  iHeroVal = lbl(irow, "--", COL_AMBER, F48);
  lbl(irow, "A", COL_AMBER, F24);
}

// ---- Load / RUN TEST bar ---------------------------------------------------
static void buildLoadBar() {
  loadBar = cont(scrRoot);
  lv_obj_set_size(loadBar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(loadBar, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(loadBar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(loadBar, 9, 0);
  loadBtn = flatBtn(loadBar);
  lv_obj_set_size(loadBtn, LV_PCT(100), 92);
  lv_obj_set_style_radius(loadBtn, 16, 0);
  lv_obj_set_flex_flow(loadBtn, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(loadBtn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(loadBtn, 13, 0);
  loadIcon = lbl(loadBtn, LV_SYMBOL_POWER, COL_GREEN, F34);
  lv_obj_t *tc = cont(loadBtn);
  lv_obj_set_size(tc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
  loadTitle = lbl(tc, "LOAD OFF", COL_GREEN, F28);
  loadSub = lbl(tc, "Tap to start sinking current", COL_GREEN, F12);
  lv_obj_add_event_cb(loadBtn, [](lv_event_t *) {
    if (isRT()) {
      if (!fuseRating) return;
      if (!connected) { showScreen(SCR_CONNECT); return; }
      if (A.startRTest) A.startRTest(fuseRating, rtSteps);
      return;
    }
    if (lastStatus.warning[0]) return;
    if (!connected) { showScreen(SCR_CONNECT); return; }
    if (A.setLoad) A.setLoad(!lastLoadOn);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(loadBar, LV_OBJ_FLAG_HIDDEN);
}

// ---- Adjust (dial stepper) -------------------------------------------------
static void stepApply(int dir) {
  UnitCfg c = unitCfg(modeUnit());
  float v = setpoint + dir * stepSize;
  if (v < c.lo) v = c.lo;
  if (v > c.hi) v = c.hi;
  // round to unit decimals
  float scale = 1; for (int i = 0; i < c.dp; i++) scale *= 10;
  v = roundf(v * scale) / scale;
  setpoint = v;
  if (A.setSetpoint) A.setSetpoint(v);
  refreshAdjust();
}
static void onStepChip(lv_event_t *e) {
  stepSize = *(float *)lv_event_get_user_data(e);
  refreshAdjust();
}
static float g_stepVals[3];  // backing storage for chip user_data

static void buildAdjust() {
  adjScreen = cont(contentStack);
  lv_obj_set_size(adjScreen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(adjScreen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(adjScreen, 11, 0);
  lv_obj_set_style_pad_row(adjScreen, 8, 0);

  lv_obj_t *vc = cont(adjScreen);
  lv_obj_set_width(vc, LV_PCT(100));
  styleCard(vc, COL_READOUT, COL_BORDER, 14, 12);
  lv_obj_set_flex_flow(vc, LV_FLEX_FLOW_COLUMN);
  adjCaption = lbl(vc, "", COL_MUTED, F12);
  lv_obj_t *avr = cont(vc);
  lv_obj_set_size(avr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(avr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(avr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(avr, 5, 0);
  adjVal = lbl(avr, "0.00", COL_INK, F48);
  adjUnit = lbl(avr, "A", COL_ACCENT, F24);

  lv_obj_t *chips = cont(adjScreen);
  lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(chips, 7, 0);
  for (int i = 0; i < 3; i++) {
    stepChip[i] = flatBtn(chips);
    lv_obj_set_flex_grow(stepChip[i], 1); lv_obj_set_height(stepChip[i], 42);
    styleCard(stepChip[i], COL_INSET, COL_BORDER, 11, 0);
    stepChipLbl[i] = lbl(stepChip[i], "-", COL_MUTED, F16); lv_obj_center(stepChipLbl[i]);
    lv_obj_add_event_cb(stepChip[i], onStepChip, LV_EVENT_CLICKED, &g_stepVals[i]);
  }

  lv_obj_t *pads = cont(adjScreen);
  lv_obj_set_width(pads, LV_PCT(100));
  lv_obj_set_flex_grow(pads, 1);
  lv_obj_set_flex_flow(pads, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(pads, 8, 0);
  lv_obj_t *minus = flatBtn(pads);
  lv_obj_set_flex_grow(minus, 1); lv_obj_set_height(minus, LV_PCT(100));
  styleCard(minus, COL_CARD, COL_BORDER, 15, 0);
  lv_obj_t *mn = lbl(minus, LV_SYMBOL_MINUS, COL_INK, F40); lv_obj_center(mn);
  lv_obj_add_event_cb(minus, [](lv_event_t *) { stepApply(-1); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *plus = flatBtn(pads);
  lv_obj_set_flex_grow(plus, 1); lv_obj_set_height(plus, LV_PCT(100));
  styleCard(plus, COL_CARD, COL_ACCENT, 15, 0);
  lv_obj_set_style_bg_color(plus, lv_color_hex(0x171630), 0);
  lv_obj_t *pl = lbl(plus, LV_SYMBOL_PLUS, COL_ACCENT2, F40); lv_obj_center(pl);
  lv_obj_add_event_cb(plus, [](lv_event_t *) { stepApply(+1); }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *typ = flatBtn(adjScreen);
  lv_obj_set_size(typ, LV_PCT(100), 44);
  styleCard(typ, COL_BLACK, COL_BORDER, 11, 0);
  lv_obj_set_style_bg_opa(typ, LV_OPA_TRANSP, 0);
  lv_obj_t *tl = lbl(typ, LV_SYMBOL_KEYBOARD "  Type exact value", COL_ACCENT2, F16); lv_obj_center(tl);
  lv_obj_add_event_cb(typ, [](lv_event_t *) { openKeypad(1); }, LV_EVENT_CLICKED, nullptr);
}

static void refreshAdjust() {
  UnitCfg c = unitCfg(modeUnit());
  char b[48];
  snprintf(b, sizeof(b), "%s - range %g-%g %s", modeName(), c.lo, c.hi, modeUnit());
  lv_label_set_text(adjCaption, b);
  fmtVal(b, sizeof(b), setpoint, c.dp); lv_label_set_text(adjVal, b);
  lv_label_set_text(adjUnit, modeUnit());
  for (int i = 0; i < 3; i++) {
    g_stepVals[i] = c.step[i];
    char s[16]; snprintf(s, sizeof(s), "%g", c.step[i]);
    char t[20]; snprintf(t, sizeof(t), "+/-%s", s);
    lv_label_set_text(stepChipLbl[i], t);
    bool on = fabsf(stepSize - c.step[i]) < 1e-6f;
    lv_obj_set_style_bg_color(stepChip[i], on ? lv_color_hex(0x1d1b33) : COL_INSET, 0);
    lv_obj_set_style_border_color(stepChip[i], on ? COL_ACCENT : COL_BORDER, 0);
    lv_obj_set_style_text_color(stepChipLbl[i], on ? COL_ACCENT2 : COL_MUTED, 0);
  }
}

// ---- Graph -----------------------------------------------------------------
static void buildGraph() {
  graphScreen = cont(contentStack);
  lv_obj_set_size(graphScreen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(graphScreen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(graphScreen, 11, 0);
  lv_obj_set_style_pad_row(graphScreen, 8, 0);

  lv_obj_t *nums = cont(graphScreen);
  lv_obj_set_size(nums, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(nums, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(nums, 18, 0);
  graphVNum = lbl(nums, "-- V", COL_GREEN, F34);
  graphINum = lbl(nums, "-- A", COL_AMBER, F34);

  lv_obj_t *card = cont(graphScreen);
  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_flex_grow(card, 1);
  styleCard(card, COL_READOUT, COL_BORDER, 14, 10);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 6, 0);
  chart = lv_chart_create(card);
  lv_obj_set_width(chart, LV_PCT(100));
  lv_obj_set_flex_grow(chart, 1);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(chart, COL_BORDER2, LV_PART_MAIN);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, HIST_N);
  lv_chart_set_div_line_count(chart, 2, 0);
  serV = lv_chart_add_series(chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
  serI = lv_chart_add_series(chart, COL_AMBER, LV_CHART_AXIS_SECONDARY_Y);
  lv_obj_t *rng = cont(card);
  lv_obj_set_size(rng, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rng, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rng, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  gVRange = lbl(rng, "-- V", COL_GREEN, F12);
  gWin = lbl(rng, "0s", COL_MUTED, F12);
  gIRange = lbl(rng, "-- A", COL_AMBER, F12);
}

static void pushHistory(float v, float i) {
  if (histCount < HIST_N) { vHist[histCount] = v; iHist[histCount] = i; histCount++; }
  else { for (int k = 1; k < HIST_N; k++) { vHist[k - 1] = vHist[k]; iHist[k - 1] = iHist[k]; }
         vHist[HIST_N - 1] = v; iHist[HIST_N - 1] = i; }
}
static void refreshChart() {
  if (!chart || histCount == 0) return;
  float vlo = vHist[0], vhi = vHist[0], ilo = iHist[0], ihi = iHist[0];
  for (int k = 0; k < histCount; k++) {
    vlo = LV_MIN(vlo, vHist[k]); vhi = LV_MAX(vhi, vHist[k]);
    ilo = LV_MIN(ilo, iHist[k]); ihi = LV_MAX(ihi, iHist[k]);
  }
  auto pad = [](float &lo, float &hi) {
    if (hi - lo < 0.05f) { float c = (hi + lo) / 2; lo = c - 0.5f; hi = c + 0.5f; }
    float p = (hi - lo) * 0.12f; lo -= p; hi += p;
  };
  pad(vlo, vhi); pad(ilo, ihi);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, (int)(vlo * 100), (int)(vhi * 100));
  lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, (int)(ilo * 100), (int)(ihi * 100));
  for (int k = 0; k < HIST_N; k++) {
    if (k < histCount) {
      lv_chart_set_value_by_id(chart, serV, k, (int)(vHist[k] * 100));
      lv_chart_set_value_by_id(chart, serI, k, (int)(iHist[k] * 100));
    } else {
      lv_chart_set_value_by_id(chart, serV, k, LV_CHART_POINT_NONE);
      lv_chart_set_value_by_id(chart, serI, k, LV_CHART_POINT_NONE);
    }
  }
  char b[24];
  snprintf(b, sizeof(b), "%.1f-%.1f V", vlo, vhi); lv_label_set_text(gVRange, b);
  snprintf(b, sizeof(b), "%.1f-%.1f A", ilo, ihi); lv_label_set_text(gIRange, b);
  snprintf(b, sizeof(b), "~%ds", histCount / 2); lv_label_set_text(gWin, b);
}

// ---- R-Test ----------------------------------------------------------------
static void addResultRow(const char *k, const char *v, lv_color_t vc, bool last) {
  lv_obj_t *row = cont(resultList);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_ver(row, 8, 0);
  if (!last) {
    lv_obj_set_style_border_color(row, lv_color_hex(0x161d26), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  }
  lbl(row, k, COL_MUTED, F14);
  lbl(row, v, vc, F14);
}

static void buildRtest() {
  rtestScreen = cont(contentStack);
  lv_obj_set_size(rtestScreen, LV_PCT(100), LV_PCT(100));
  lv_obj_add_flag(rtestScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(rtestScreen, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(rtestScreen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(rtestScreen, 12, 0);
  lv_obj_set_style_pad_row(rtestScreen, 12, 0);

  lv_obj_t *title = cont(rtestScreen);
  lv_obj_set_size(title, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(title, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(title, 8, 0);
  lbl(title, LV_SYMBOL_LOOP, COL_ACCENT, F20);
  lbl(title, "Circuit-Resistance", COL_INK, F20);

  // idle
  rtIdleBox = cont(rtestScreen);
  lv_obj_set_size(rtIdleBox, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rtIdleBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rtIdleBox, 12, 0);
  lv_obj_t *expl = lbl(rtIdleBox, "Sweeps current in steps and fits a line to measure series resistance.", COL_MUTED, F12);
  lv_label_set_long_mode(expl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(expl, LV_PCT(100));
  lv_obj_t *fuseTile = flatBtn(rtIdleBox);
  lv_obj_set_size(fuseTile, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(fuseTile, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(fuseTile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(fuseTile, 5, 0);
  lbl(fuseTile, "FUSE RATING - required - tap to cycle", COL_MUTED, F12);
  lv_obj_t *frow = cont(fuseTile);
  lv_obj_set_size(frow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(frow, 6, 0);
  fuseVal = lbl(frow, "--", COL_FAINT, F34);
  lbl(frow, "A", COL_ACCENT, F20);
  lv_obj_add_event_cb(fuseTile, [](lv_event_t *) {
    int idx = -1; for (int i = 0; i < FUSE_N; i++) if (FUSE_RATINGS[i] == fuseRating) idx = i;
    fuseRating = FUSE_RATINGS[(idx + 1) % FUSE_N]; refreshRtest();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *stepsRow = cont(rtIdleBox);
  lv_obj_set_size(stepsRow, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(stepsRow, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(stepsRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stepsRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(stepsRow, 10, 0);
  lv_obj_t *scol = cont(stepsRow); lv_obj_set_flex_grow(scol, 1); lv_obj_set_height(scol, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(scol, LV_FLEX_FLOW_COLUMN);
  lbl(scol, "STEPS", COL_MUTED, F12);
  stepsVal = lbl(scol, "8", COL_INK, F28);
  lv_obj_t *sm = flatBtn(stepsRow); lv_obj_set_size(sm, 58, 58);
  styleCard(sm, COL_INSET, COL_BORDER, 12, 0);
  lv_obj_t *sml = lbl(sm, LV_SYMBOL_MINUS, COL_INK, F20); lv_obj_center(sml);
  lv_obj_add_event_cb(sm, [](lv_event_t *) { rtSteps = rtSteps > 3 ? rtSteps - 1 : 3; refreshRtest(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *sp2 = flatBtn(stepsRow); lv_obj_set_size(sp2, 58, 58);
  styleCard(sp2, COL_INSET, COL_BORDER, 12, 0);
  lv_obj_t *spl = lbl(sp2, LV_SYMBOL_PLUS, COL_INK, F20); lv_obj_center(spl);
  lv_obj_add_event_cb(sp2, [](lv_event_t *) { rtSteps = rtSteps < 20 ? rtSteps + 1 : 20; refreshRtest(); }, LV_EVENT_CLICKED, nullptr);
  startBtn = flatBtn(rtIdleBox);
  lv_obj_set_size(startBtn, LV_PCT(100), 62);
  lv_obj_set_style_radius(startBtn, 14, 0);
  startBtnLbl = lbl(startBtn, LV_SYMBOL_PLAY "  Start sweep", COL_DARKINK, F20);
  lv_obj_center(startBtnLbl);
  lv_obj_add_event_cb(startBtn, [](lv_event_t *) {
    if (!fuseRating) return;
    if (!connected) { showScreen(SCR_CONNECT); return; }
    if (A.startRTest) A.startRTest(fuseRating, rtSteps);
  }, LV_EVENT_CLICKED, nullptr);

  // running
  rtRunBox = cont(rtestScreen);
  lv_obj_set_size(rtRunBox, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rtRunBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rtRunBox, 12, 0);
  lv_obj_add_flag(rtRunBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *runCard = cont(rtRunBox);
  lv_obj_set_size(runCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(runCard, COL_READOUT, lv_color_hex(0x3A3568), 14, 14);
  lv_obj_set_flex_flow(runCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(runCard, 12, 0);
  lv_obj_t *rr = cont(runCard); lv_obj_set_size(rr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lbl(rr, LV_SYMBOL_REFRESH " RUNNING", COL_ACCENT, F14);
  runStepLbl = lbl(rr, "Step 0/0", COL_INK, F16);
  runBar = lv_bar_create(runCard);
  lv_obj_set_size(runBar, LV_PCT(100), 12);
  lv_obj_set_style_bg_color(runBar, lv_color_hex(0x161d26), LV_PART_MAIN);
  lv_obj_set_style_bg_color(runBar, COL_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_radius(runBar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(runBar, 6, LV_PART_INDICATOR);
  lv_bar_set_range(runBar, 0, 100);
  lv_obj_t *rv = cont(runCard); lv_obj_set_size(rv, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rv, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(rv, 20, 0);
  runVLbl = lbl(rv, "-- V", COL_GREEN, F28);
  runILbl = lbl(rv, "-- A", COL_AMBER, F28);
  lv_obj_t *stopBtn = flatBtn(rtRunBox);
  lv_obj_set_size(stopBtn, LV_PCT(100), 66);
  styleCard(stopBtn, lv_color_hex(0x2a1416), COL_RED, 14, 0);
  lv_obj_set_style_border_width(stopBtn, 2, 0);
  lv_obj_t *stl = lbl(stopBtn, LV_SYMBOL_STOP "  STOP", COL_RED, F20); lv_obj_center(stl);
  lv_obj_add_event_cb(stopBtn, [](lv_event_t *) { if (A.stopRTest) A.stopRTest(); }, LV_EVENT_CLICKED, nullptr);

  // result
  rtResultBox = cont(rtestScreen);
  lv_obj_set_size(rtResultBox, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rtResultBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rtResultBox, 12, 0);
  lv_obj_add_flag(rtResultBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *resCard = cont(rtResultBox);
  lv_obj_set_size(resCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(resCard, COL_READOUT, COL_BORDER, 14, 14);
  lv_obj_set_flex_flow(resCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(resCard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(resCard, 3, 0);
  lbl(resCard, "SERIES RESISTANCE", COL_MUTED, F12);
  resistVal = lbl(resCard, "-- ohm", COL_GREEN, F44);
  lowConfBox = lbl(rtResultBox, LV_SYMBOL_WARNING " Low confidence - check connections", COL_AMBER, F12);
  lv_obj_add_flag(lowConfBox, LV_OBJ_FLAG_HIDDEN);
  resultList = cont(rtResultBox);
  lv_obj_set_size(resultList, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(resultList, COL_INSET, COL_BORDER2, 12, 4);
  lv_obj_set_style_pad_hor(resultList, 13, 0);
  lv_obj_set_flex_flow(resultList, LV_FLEX_FLOW_COLUMN);
  saveBtn = flatBtn(rtResultBox);
  lv_obj_set_size(saveBtn, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(saveBtn, COL_ACCENT, 0);
  lv_obj_set_style_bg_opa(saveBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(saveBtn, 13, 0);
  saveBtnLbl = lbl(saveBtn, LV_SYMBOL_SAVE "  Save to SD card", COL_DARKINK, F16);
  lv_obj_center(saveBtnLbl);
  lv_obj_add_event_cb(saveBtn, [](lv_event_t *) {
    if (rtSaved) return;
    if (A.saveRTest) A.saveRTest();
    rtSaved = true; rtSeq++; refreshRtest();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *newBtn = flatBtn(rtResultBox);
  lv_obj_set_size(newBtn, LV_PCT(100), 52);
  styleCard(newBtn, COL_BLACK, COL_BORDER, 13, 0);
  lv_obj_set_style_bg_opa(newBtn, LV_OPA_TRANSP, 0);
  lv_obj_t *nbl = lbl(newBtn, "New test", COL_ACCENT2, F16); lv_obj_center(nbl);
  lv_obj_add_event_cb(newBtn, [](lv_event_t *) { rtPhase = RT_IDLE; rtSaved = false; refreshRtest(); }, LV_EVENT_CLICKED, nullptr);

  rtStatusLbl = lbl(rtestScreen, "", COL_AMBER, F12);
  lv_obj_add_flag(rtStatusLbl, LV_OBJ_FLAG_HIDDEN);
}

static void refreshRtest() {
  lv_obj_add_flag(rtIdleBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(rtRunBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(rtResultBox, LV_OBJ_FLAG_HIDDEN);
  if (rtPhase == RT_IDLE) {
    lv_obj_clear_flag(rtIdleBox, LV_OBJ_FLAG_HIDDEN);
    char b[16];
    if (fuseRating) { snprintf(b, sizeof(b), "%g", fuseRating); lv_label_set_text(fuseVal, b); lv_obj_set_style_text_color(fuseVal, COL_INK, 0); }
    else { lv_label_set_text(fuseVal, "--"); lv_obj_set_style_text_color(fuseVal, COL_FAINT, 0); }
    snprintf(b, sizeof(b), "%d", rtSteps); lv_label_set_text(stepsVal, b);
    lv_obj_set_style_bg_color(startBtn, fuseRating ? COL_ACCENT : lv_color_hex(0x161d26), 0);
    lv_obj_set_style_bg_opa(startBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(startBtnLbl, fuseRating ? COL_DARKINK : COL_FAINT, 0);
  } else if (rtPhase == RT_RUN) {
    lv_obj_clear_flag(rtRunBox, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(rtResultBox, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---- Connect ---------------------------------------------------------------
static std::vector<std::string *> g_addrPool;
static void onDeviceRow(lv_event_t *e) {
  const char *addr = (const char *)lv_event_get_user_data(e);
  if (addr && strcmp(addr, "__demo__") == 0) { if (A.startDemo) A.startDemo(); }
  else if (addr && A.connect) A.connect(addr);
}
static void addDeviceRow(const char *sym, const char *name, const char *sub, const char *addr) {
  lv_obj_t *row = flatBtn(deviceList);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(row, COL_CARD, COL_BORDER, 12, 13);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 12, 0);
  lbl(row, sym, COL_ACCENT, F24);
  lv_obj_t *col = cont(row); lv_obj_set_flex_grow(col, 1); lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lbl(col, name, COL_INK, F16);
  lbl(col, sub, COL_MUTED, F12);
  lv_obj_add_event_cb(row, onDeviceRow, LV_EVENT_CLICKED, (void *)addr);
}
static void buildConnect() {
  connectScreen = cont(contentStack);
  lv_obj_set_size(connectScreen, LV_PCT(100), LV_PCT(100));
  lv_obj_add_flag(connectScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(connectScreen, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(connectScreen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(connectScreen, 12, 0);
  lv_obj_set_style_pad_row(connectScreen, 12, 0);
  lv_obj_t *st = cont(connectScreen);
  lv_obj_set_size(st, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(st, COL_INSET, COL_INSET, 12, 11);
  lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(st, 10, 0);
  connDot2 = lv_obj_create(st);
  lv_obj_set_size(connDot2, 10, 10);
  lv_obj_set_style_radius(connDot2, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(connDot2, 0, 0);
  lv_obj_set_style_bg_color(connDot2, COL_MUTED, 0);
  connLabel2 = lbl(st, "Disconnected", COL_INK, F16);
  lv_obj_set_flex_grow(connLabel2, 1);
  connDisc = flatBtn(st);
  lv_obj_set_size(connDisc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  styleCard(connDisc, COL_BLACK, COL_RED, 8, 7);
  lv_obj_set_style_bg_opa(connDisc, LV_OPA_TRANSP, 0);
  lbl(connDisc, "Disconnect", COL_RED, F14);
  lv_obj_add_event_cb(connDisc, [](lv_event_t *) { if (A.disconnect) A.disconnect(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(connDisc, LV_OBJ_FLAG_HIDDEN);
  scanBtn = flatBtn(connectScreen);
  lv_obj_set_size(scanBtn, LV_PCT(100), 56);
  styleCard(scanBtn, COL_BLACK, COL_ACCENT, 12, 0);
  lv_obj_set_style_bg_opa(scanBtn, LV_OPA_TRANSP, 0);
  lv_obj_t *sbl = lbl(scanBtn, LV_SYMBOL_REFRESH "  Scan for devices", COL_ACCENT, F16); lv_obj_center(sbl);
  lv_obj_add_event_cb(scanBtn, [](lv_event_t *) { clearDevices(); if (A.scan) A.scan(); }, LV_EVENT_CLICKED, nullptr);
  lbl(connectScreen, "DEVICES", COL_MUTED, F12);
  deviceList = cont(connectScreen);
  lv_obj_set_size(deviceList, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(deviceList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(deviceList, 8, 0);
  addDeviceRow(LV_SYMBOL_EDIT, "Demo Simulator", "built-in - no hardware", "__demo__");
}

// ---- Menu overlay ----------------------------------------------------------
static void buildMenu() {
  menuOverlay = cont(lv_layer_top());
  lv_obj_set_size(menuOverlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(menuOverlay, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(menuOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(menuOverlay, 12, 0);
  lv_obj_set_flex_flow(menuOverlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(menuOverlay, 10, 0);
  lv_obj_add_flag(menuOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *hdr = cont(menuOverlay);
  lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 8, 0);
  lv_obj_t *x = flatBtn(hdr); lv_obj_set_size(x, 40, 40);
  styleCard(x, COL_CARD, COL_BORDER, 10, 0);
  lv_obj_t *xl = lbl(x, LV_SYMBOL_CLOSE, COL_MUTED, F16); lv_obj_center(xl);
  lv_obj_add_event_cb(x, [](lv_event_t *) { showOverlay(OV_NONE); }, LV_EVENT_CLICKED, nullptr);
  lbl(hdr, "Menu", COL_INK, F20);
  lv_obj_t *grid = cont(menuOverlay);
  lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(grid, 1);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 8, 0);
  struct MItem { const char *sym; const char *name; const char *note; int act; };  // act: 0..5
  static const MItem items[6] = {
    {LV_SYMBOL_EYE_OPEN, "Monitor", "Live readout", 0},
    {LV_SYMBOL_SETTINGS, "Adjust", "Set setpoint", 1},
    {LV_SYMBOL_SHUFFLE, "Mode", "CC/CV/CR/...", 2},
    {LV_SYMBOL_UP, "Graph", "Live trend", 3},
    {LV_SYMBOL_LOOP, "R-Test", "Sweep resistance", 4},
    {LV_SYMBOL_BLUETOOTH, "Connect", "Manage device", 5},
  };
  for (int i = 0; i < 6; i++) {
    lv_obj_t *t = flatBtn(grid);
    lv_obj_set_size(t, 164, 150);
    styleCard(t, COL_CARD, COL_BORDER, 14, 13);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lbl(t, items[i].sym, COL_ACCENT, F28);
    lv_obj_t *tc = cont(t); lv_obj_set_size(tc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    lbl(tc, items[i].name, COL_INK, F16);
    lbl(tc, items[i].note, COL_MUTED, F12);
    lv_obj_add_event_cb(t, [](lv_event_t *e) {
      int act = (int)(intptr_t)lv_event_get_user_data(e);
      switch (act) {
        case 0: showScreen(SCR_MON); break;
        case 1: showScreen(SCR_ADJ); break;
        case 2: refreshPicker(); showOverlay(OV_PICKER); break;
        case 3: showScreen(SCR_GRAPH); break;
        case 4: showScreen(SCR_RTEST); break;
        case 5: showScreen(SCR_CONNECT); break;
      }
    }, LV_EVENT_CLICKED, (void *)(intptr_t)items[i].act);
  }
}

// ---- Keypad overlay --------------------------------------------------------
static void kpRefresh() {
  lv_label_set_text(kpValue, kpBuf.empty() ? "0" : kpBuf.c_str());
  const char *unit = kpTarget == 2 ? "A" : modeUnit();
  lv_label_set_text(kpUnit, unit);
  lv_label_set_text(kpTitle, kpTarget == 2 ? "Fuse rating" : modeName());
  UnitCfg c = unitCfg(unit);
  for (int i = 0; i < 4; i++) {
    char b[12]; snprintf(b, sizeof(b), "%g", c.preset[i]);
    lv_label_set_text(kpPreset[i], b);
    lv_obj_set_user_data(kpPresetBtn[i], (void *)(intptr_t)(int)(c.preset[i] * 100));
  }
}
static void openKeypad(int target) {
  kpTarget = target;
  if (target == 1) { char b[16]; snprintf(b, sizeof(b), "%g", setpoint); kpBuf = b; }
  else kpBuf = fuseRating ? std::to_string((int)fuseRating) : "";
  kpRefresh();
  showOverlay(OV_KEYPAD);
}
static void kpPress(const char *c) {
  if (strcmp(c, "del") == 0) { if (!kpBuf.empty()) kpBuf.pop_back(); }
  else if (strcmp(c, ".") == 0) { if (kpBuf.find('.') == std::string::npos) kpBuf = (kpBuf.empty() ? "0" : kpBuf) + "."; }
  else { if (kpBuf == "0") kpBuf = c; else kpBuf += c;
         std::string d = kpBuf; d.erase(std::remove(d.begin(), d.end(), '.'), d.end());
         if (d.size() > 6) kpBuf.pop_back(); }
  kpRefresh();
}
static void kpSet() {
  float v = atof(kpBuf.c_str());
  if (kpTarget == 1) { setpoint = v; if (A.setSetpoint) A.setSetpoint(v); refreshAdjust(); refreshMonitor(); }
  else { fuseRating = v; refreshRtest(); refreshMonitor(); }
  showOverlay(OV_NONE);
}
static void onKey(lv_event_t *e) { kpPress((const char *)lv_event_get_user_data(e)); }
static void onPreset(lv_event_t *e) {
  int cents = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  char b[16]; snprintf(b, sizeof(b), "%g", cents / 100.0f); kpBuf = b; kpRefresh();
}
static void buildKeypad() {
  kpOverlay = cont(lv_layer_top());
  lv_obj_set_size(kpOverlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(kpOverlay, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(kpOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(kpOverlay, 12, 0);
  lv_obj_set_flex_flow(kpOverlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(kpOverlay, 8, 0);
  lv_obj_add_flag(kpOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *hdr = cont(kpOverlay);
  lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 8, 0);
  lv_obj_t *x = flatBtn(hdr); lv_obj_set_size(x, 40, 40);
  styleCard(x, COL_CARD, COL_BORDER, 10, 0);
  lv_obj_t *xl = lbl(x, LV_SYMBOL_CLOSE, COL_MUTED, F16); lv_obj_center(xl);
  lv_obj_add_event_cb(x, [](lv_event_t *) { showOverlay(OV_NONE); }, LV_EVENT_CLICKED, nullptr);
  kpTitle = lbl(hdr, "Setpoint", COL_INK, F16);
  lv_obj_t *disp = cont(kpOverlay);
  lv_obj_set_size(disp, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(disp, COL_READOUT, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(disp, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(disp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(disp, 8, 0);
  kpValue = lbl(disp, "0", COL_INK, F48);
  kpUnit = lbl(disp, "A", COL_ACCENT, F20);
  lv_obj_t *pr = cont(kpOverlay);
  lv_obj_set_size(pr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(pr, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(pr, 6, 0);
  for (int i = 0; i < 4; i++) {
    lv_obj_t *b = flatBtn(pr);
    lv_obj_set_flex_grow(b, 1); lv_obj_set_height(b, 44);
    styleCard(b, COL_CARD, COL_BORDER, 9, 0);
    kpPresetBtn[i] = b;
    kpPreset[i] = lbl(b, "-", COL_ACCENT, F16); lv_obj_center(kpPreset[i]);
    lv_obj_add_event_cb(b, onPreset, LV_EVENT_CLICKED, nullptr);
  }
  lv_obj_t *pad = cont(kpOverlay);
  lv_obj_set_size(pad, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(pad, 1);
  lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(pad, 6, 0);
  static const char *keys[12] = {"7", "8", "9", "4", "5", "6", "1", "2", "3", ".", "0", "del"};
  for (int i = 0; i < 12; i++) {
    lv_obj_t *k = flatBtn(pad);
    lv_obj_set_size(k, 108, 62);
    styleCard(k, COL_CARD, COL_BORDER, 11, 0);
    const char *face = strcmp(keys[i], "del") == 0 ? LV_SYMBOL_BACKSPACE : keys[i];
    lv_obj_t *kl = lbl(k, face, COL_INK, F28); lv_obj_center(kl);
    lv_obj_add_event_cb(k, onKey, LV_EVENT_CLICKED, (void *)keys[i]);
  }
  lv_obj_t *set = flatBtn(kpOverlay);
  lv_obj_set_size(set, LV_PCT(100), 58);
  lv_obj_set_style_bg_color(set, COL_ACCENT, 0);
  lv_obj_set_style_bg_opa(set, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(set, 12, 0);
  lv_obj_t *sl = lbl(set, "SET", COL_DARKINK, F20); lv_obj_center(sl);
  lv_obj_add_event_cb(set, [](lv_event_t *) { kpSet(); }, LV_EVENT_CLICKED, nullptr);
}

// ---- Mode picker overlay ---------------------------------------------------
static void onModePick(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  int m = MODE_IDS[idx];
  if (m == MODE_RT) { curMode = MODE_RT; }
  else { curMode = m; if (A.setMode) A.setMode(m); UnitCfg c = unitCfg(modeUnit()); stepSize = c.step[c.defStep]; }
  showOverlay(OV_NONE);
  showScreen(SCR_MON);
}
static void buildPicker() {
  pickerOverlay = cont(lv_layer_top());
  lv_obj_set_size(pickerOverlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(pickerOverlay, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(pickerOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(pickerOverlay, 12, 0);
  lv_obj_set_flex_flow(pickerOverlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(pickerOverlay, 10, 0);
  lv_obj_add_flag(pickerOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *hdr = cont(pickerOverlay);
  lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 8, 0);
  lv_obj_t *x = flatBtn(hdr); lv_obj_set_size(x, 40, 40);
  styleCard(x, COL_CARD, COL_BORDER, 10, 0);
  lv_obj_t *xl = lbl(x, LV_SYMBOL_CLOSE, COL_MUTED, F16); lv_obj_center(xl);
  lv_obj_add_event_cb(x, [](lv_event_t *) { showOverlay(OV_NONE); }, LV_EVENT_CLICKED, nullptr);
  lbl(hdr, "Select mode", COL_INK, F16);
  lv_obj_t *grid = cont(pickerOverlay);
  lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(grid, 1);
  lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 8, 0);
  static const char *MU[7] = {"A", "V", "ohm", "W", "A", "A", "ohm"};
  for (int i = 0; i < 7; i++) {
    lv_obj_t *t = flatBtn(grid);
    lv_obj_set_size(t, 164, 74);
    bool rt = (MODE_IDS[i] == MODE_RT);
    styleCard(t, COL_CARD, rt ? COL_AMBER : COL_BORDER, 14, 4);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lbl(t, MODE_ABBR[i], rt ? COL_AMBER : COL_INK, F28);
    lbl(t, MODE_NAME[i], COL_MUTED, F12);
    lbl(t, MU[i], COL_ACCENT, F12);
    lv_obj_add_event_cb(t, onModePick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    modeTile[i] = t;
  }
}
static void refreshPicker() {
  for (int i = 0; i < 7; i++) {
    bool sel = MODE_IDS[i] == curMode;
    bool rt = MODE_IDS[i] == MODE_RT;
    lv_obj_set_style_border_color(modeTile[i], sel ? COL_ACCENT : (rt ? COL_AMBER : COL_BORDER), 0);
    lv_obj_set_style_border_width(modeTile[i], sel ? 2 : 1, 0);
  }
}

// ---- Live updates ----------------------------------------------------------
static void hhmmss(int t, char *out, int n) {
  int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
  if (h) snprintf(out, n, "%d:%02d:%02d", h, m, s); else snprintf(out, n, "%02d:%02d", m, s);
}

// Update the Monitor mode|set bar + load bar for the current mode/state.
static void refreshMonitor() {
  lv_label_set_text(modeAbbrLbl, modeAbbr());
  lv_label_set_text(modeNameLbl, modeName());
  char b[24];
  if (isRT()) {
    lv_label_set_text(setLabelLbl, "FUSE");
    if (fuseRating) snprintf(b, sizeof(b), "%g", fuseRating); else strcpy(b, "--");
    lv_label_set_text(setValLbl, b);
    lv_label_set_text(setUnitLbl, "A");
  } else {
    lv_label_set_text(setLabelLbl, "SET");
    UnitCfg c = unitCfg(modeUnit());
    fmtVal(b, sizeof(b), setpoint, c.dp); lv_label_set_text(setValLbl, b);
    lv_label_set_text(setUnitLbl, modeUnit());
  }
  // load / run-test button
  if (isRT()) {
    lv_obj_set_style_bg_color(loadBtn, COL_ACCENT, 0); lv_obj_set_style_bg_opa(loadBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(loadBtn, 0, 0);
    lv_label_set_text(loadIcon, LV_SYMBOL_LOOP); lv_obj_set_style_text_color(loadIcon, COL_DARKINK, 0);
    lv_label_set_text(loadTitle, "RUN TEST"); lv_obj_set_style_text_color(loadTitle, COL_DARKINK, 0);
    lv_label_set_text(loadSub, "Sweep & measure resistance"); lv_obj_set_style_text_color(loadSub, COL_DARKINK, 0);
  } else if (lastLoadOn) {
    lv_obj_set_style_bg_color(loadBtn, COL_RED, 0); lv_obj_set_style_bg_opa(loadBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(loadBtn, 0, 0);
    lv_label_set_text(loadIcon, LV_SYMBOL_POWER); lv_obj_set_style_text_color(loadIcon, lv_color_hex(0x1a0606), 0);
    lv_label_set_text(loadTitle, "LOAD ON"); lv_obj_set_style_text_color(loadTitle, lv_color_hex(0x1a0606), 0);
    lv_label_set_text(loadSub, "Sinking current - tap to stop"); lv_obj_set_style_text_color(loadSub, lv_color_hex(0x1a0606), 0);
  } else {
    lv_obj_set_style_bg_opa(loadBtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(loadBtn, COL_GREEN, 0); lv_obj_set_style_border_width(loadBtn, 2, 0);
    lv_label_set_text(loadIcon, LV_SYMBOL_POWER); lv_obj_set_style_text_color(loadIcon, COL_GREEN, 0);
    lv_label_set_text(loadTitle, "LOAD OFF"); lv_obj_set_style_text_color(loadTitle, COL_GREEN, 0);
    lv_label_set_text(loadSub, "Tap to start sinking current"); lv_obj_set_style_text_color(loadSub, COL_GREEN, 0);
  }
}

void onStatus(const el15::Status &s) {
  if (!s.valid) return;
  lastStatus = s;
  lastLoadOn = s.loadOn;
  if (!isRT()) curMode = s.mode;
  char b[48];

  // status strip temp chip
  lv_color_t tc = s.temperature > 50 ? COL_RED : s.temperature > 42 ? COL_AMBER : COL_INK;
  snprintf(b, sizeof(b), "%.1fC", s.temperature);
  lv_label_set_text(stTempLabel, b); lv_obj_set_style_text_color(stTempLabel, tc, 0);

  // info bar
  snprintf(b, sizeof(b), "%.1f W", s.power); lv_label_set_text(ibPower, b);
  snprintf(b, sizeof(b), LV_SYMBOL_REFRESH " %d/5", s.fanSpeed); lv_label_set_text(ibFan, b);
  char rt[16]; hhmmss(s.runtime, rt, sizeof(rt));
  snprintf(b, sizeof(b), LV_SYMBOL_LOOP " %s", rt); lv_label_set_text(ibRuntime, b);
  if (s.mode == el15::MODE_CAP) { snprintf(b, sizeof(b), "%.3f Ah", s.capacityAh); lv_label_set_text(ibExtra, b); }
  else if (s.mode == el15::MODE_DCR) { snprintf(b, sizeof(b), "%.1f mohm", s.dcrMilliOhm); lv_label_set_text(ibExtra, b); }
  else lv_label_set_text(ibExtra, "");

  // hero blocks
  float shownI = (s.mode == el15::MODE_DCR) ? s.dcrI1 : s.current;
  snprintf(b, sizeof(b), "%.2f", s.voltage); lv_label_set_text(vHeroVal, b);
  snprintf(b, sizeof(b), "%.3f", shownI); lv_label_set_text(iHeroVal, b);
  snprintf(b, sizeof(b), "%.2f V", s.voltage); lv_label_set_text(graphVNum, b);
  snprintf(b, sizeof(b), "%.3f A", shownI); lv_label_set_text(graphINum, b);
  if (s.loadOn) {
    lv_obj_set_style_bg_color(iHeroBlock, COL_IHERO_BG_ON, 0);
    lv_obj_set_style_border_color(iHeroBlock, COL_IHERO_BD_ON, 0);
    lv_obj_set_style_text_color(iHeroVal, COL_RED, 0);
    lv_obj_clear_flag(iHeroSink, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_set_style_bg_color(iHeroBlock, COL_IHERO_BG, 0);
    lv_obj_set_style_border_color(iHeroBlock, COL_IHERO_BD, 0);
    lv_obj_set_style_text_color(iHeroVal, COL_AMBER, 0);
    lv_obj_add_flag(iHeroSink, LV_OBJ_FLAG_HIDDEN);
  }

  // setpoint sync (not while editing)
  if (curScreen != SCR_ADJ && curOverlay != OV_KEYPAD && !isRT() && s.setpointInPacket)
    setpoint = s.setpoint;

  // graph history
  pushHistory(s.voltage, shownI);
  if (curScreen == SCR_GRAPH) refreshChart();

  // fault banner
  if (s.warning[0]) {
    snprintf(b, sizeof(b), LV_SYMBOL_WARNING "  %s - PROTECTION", s.warning); lv_label_set_text(faultTitle, b);
    lv_label_set_text(faultMsg, "Load protection tripped - check the setup");
    lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);

  // running r-test live values
  if (rtPhase == RT_RUN) {
    snprintf(b, sizeof(b), "%.2f V", s.voltage); lv_label_set_text(runVLbl, b);
    snprintf(b, sizeof(b), "%.3f A", s.current); lv_label_set_text(runILbl, b);
  }

  refreshMonitor();
}

void onConnState(int state, const char *info) {
  connected = (state == 3);
  const char *label = info ? info : "";
  lv_color_t dot = connected ? COL_GREEN : (state == 1 || state == 2) ? COL_AMBER : COL_MUTED;
  lv_label_set_text(stConnLabel, label);
  lv_obj_set_style_bg_color(stDot, dot, 0);
  lv_label_set_text(connLabel2, label);
  lv_obj_set_style_bg_color(connDot2, dot, 0);
  if (connected) {
    lv_obj_clear_flag(stTempChip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(connDisc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scanBtn, LV_OBJ_FLAG_HIDDEN);
    if (curScreen == SCR_CONNECT) showScreen(SCR_MON);
  } else {
    lv_obj_add_flag(stTempChip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(connDisc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(scanBtn, LV_OBJ_FLAG_HIDDEN);
    histCount = 0;
  }
  // info bar visibility depends on connection
  if (connected && (curScreen == SCR_MON || curScreen == SCR_ADJ || curScreen == SCR_GRAPH))
    lv_obj_clear_flag(infoBar, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(infoBar, LV_OBJ_FLAG_HIDDEN);
}

void onDeviceFound(const char *address, const char *name) {
  std::string *addr = new std::string(address);
  g_addrPool.push_back(addr);
  addDeviceRow(LV_SYMBOL_BLUETOOTH, name, address, addr->c_str());
}
void clearDevices() {
  uint32_t cnt = lv_obj_get_child_cnt(deviceList);
  for (uint32_t i = cnt; i > 1; i--) lv_obj_del(lv_obj_get_child(deviceList, i - 1));
  for (auto *p : g_addrPool) delete p;
  g_addrPool.clear();
}

void onTestProgress(int step, int total, float target, float v, float i) {
  rtPhase = RT_RUN; refreshRtest();
  if (curScreen != SCR_RTEST) showScreen(SCR_RTEST);
  char b[40];
  snprintf(b, sizeof(b), "Step %d/%d", step, total); lv_label_set_text(runStepLbl, b);
  lv_bar_set_value(runBar, total > 0 ? step * 100 / total : 0, LV_ANIM_OFF);
  snprintf(b, sizeof(b), "%.2f V", v); lv_label_set_text(runVLbl, b);
  snprintf(b, sizeof(b), "%.3f A", i); lv_label_set_text(runILbl, b);
}
void onTestComplete(const ResistanceTest::Result &r) {
  lastResult = r; rtPhase = RT_RESULT; rtSaved = false;
  char b[32];
  if (r.resistanceOhm < 1.0f) snprintf(b, sizeof(b), "%.4f ohm", r.resistanceOhm);
  else snprintf(b, sizeof(b), "%.3f ohm", r.resistanceOhm);
  lv_label_set_text(resistVal, b);
  if (r.reliable) lv_obj_add_flag(lowConfBox, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(lowConfBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(resultList);
  char v1[24];
  snprintf(v1, sizeof(v1), "%.2f V", r.openCircuitVoltage); addResultRow("Open-circuit voltage", v1, COL_INK, false);
  snprintf(v1, sizeof(v1), "%.4f", r.rSquared); addResultRow("Fit quality (R2)", v1, r.reliable ? COL_GREEN : COL_AMBER, false);
  float lo = r.samples.empty() ? 0 : r.maxTestCurrent / r.samples.size();
  snprintf(v1, sizeof(v1), "%.2f-%.2f A", lo, r.maxTestCurrent); addResultRow("Current sweep", v1, COL_INK, false);
  snprintf(v1, sizeof(v1), "%d", (int)r.samples.size()); addResultRow("Steps / samples", v1, COL_INK, false);
  snprintf(v1, sizeof(v1), "%.1f A", r.fuseRating); addResultRow("Fuse limit", v1, COL_INK, false);
  snprintf(v1, sizeof(v1), "%.1f C", lastStatus.temperature); addResultRow("Load temp", v1, COL_INK, true);
  lv_label_set_text(saveBtnLbl, LV_SYMBOL_SAVE "  Save to SD card");
  lv_obj_set_style_bg_color(saveBtn, COL_ACCENT, 0);
  refreshRtest();
  showScreen(SCR_RTEST);
}
void onTestError(const char *msg) {
  rtPhase = RT_IDLE; refreshRtest();
  lv_label_set_text(rtStatusLbl, msg);
  lv_obj_clear_flag(rtStatusLbl, LV_OBJ_FLAG_HIDDEN);
}

// ---- Entry -----------------------------------------------------------------
void begin(const UiActions &actions) {
  A = actions;
  scrRoot = lv_scr_act();
  lv_obj_set_style_bg_color(scrRoot, COL_BLACK, 0);
  lv_obj_set_style_pad_all(scrRoot, 0, 0);
  lv_obj_set_style_pad_gap(scrRoot, 0, 0);
  lv_obj_clear_flag(scrRoot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(scrRoot, LV_FLEX_FLOW_COLUMN);

  buildStatusStrip();
  buildInfoBar();
  buildFaultBanner();

  contentStack = cont(scrRoot);
  lv_obj_set_width(contentStack, LV_PCT(100));
  lv_obj_set_flex_grow(contentStack, 1);
  lv_obj_set_flex_flow(contentStack, LV_FLEX_FLOW_COLUMN);

  buildMonitor();
  buildAdjust();
  buildGraph();
  buildRtest();
  buildConnect();
  buildLoadBar();
  buildMenu();
  buildKeypad();
  buildPicker();

  UnitCfg c = unitCfg("A");
  stepSize = c.step[c.defStep];
  showScreen(SCR_MON);
  refreshRtest();
}

}  // namespace ui
