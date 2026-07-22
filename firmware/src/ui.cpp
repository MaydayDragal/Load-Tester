// LVGL UI for the EL15 controller - v2 "Focus" (368x448 portrait).
//
// No persistent tab bar: one task fills the screen; a full-screen grid Menu
// (button top-right of the status strip) reaches any destination in <=2 taps,
// and a one-tap back arrow returns to Monitor. See README_v2_focus handoff for
// the spec. Fonts use bundled Montserrat as the Inter/JetBrains-Mono stand-in
// (48 px is the largest built-in, standing in for the 64 px hero); icons use the
// nearest LVGL built-in symbols (Phosphor glyph embed is a later pass).

#include "ui.h"

#include <Arduino.h>
#include <lvgl.h>

#include "audio.h"
#include "display.h"
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
enum Screen { SCR_MON, SCR_ADJ, SCR_GRAPH, SCR_RTEST, SCR_CONNECT, SCR_SET, SCR_BATT };
enum Overlay { OV_NONE, OV_MENU, OV_KEYPAD, OV_PICKER };
enum RtPhase { RT_IDLE, RT_RUN, RT_RESULT };

static const int MODE_RT = 0xF0;    // UI-only pseudo-mode (drives the R-Test engine)
static const int MODE_BATT = 0xF1;  // UI-only pseudo-mode (drives the capacity test)

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

// ---- Circuit-estimate inputs (optional, R-Test setup) ------------------------
static float estWireMm2 = 0;   // conductor cross-section; 0 = not specified
static float estWireLen = 0;   // total conductor length in meters
static int   estConns   = 0;   // mated contact pairs (male+female) in the path
static int   estFuseType = 0;  // index into FUSE_TYPE_NAMES

// Automotive conductor sizes, 0.13 mm2 (26 AWG signal wire) through 10 mm2.
static const float WIRE_SIZES[] = {0, 0.13f, 0.22f, 0.35f, 0.5f, 0.75f, 1.0f, 1.5f, 2.5f, 4, 6, 10};
static const int   WIRE_SIZES_N = 12;
static const char *FUSE_TYPE_NAMES[] = {"None", "Standard", "Mini", "Maxi"};

// Typical COLD resistance of a standard ATO/ATC blade fuse per rating (ohm),
// consistent with Littelfuse 287-series data. MINI (297) elements run ~15%
// higher at the same rating; MAXI (299) heavy elements ~35% lower.
static float fuseTypeR() {
  static const float base[FUSE_N] = {0.120f, 0.058f, 0.038f, 0.018f, 0.0122f,
                                     0.0089f, 0.0055f, 0.0041f, 0.0033f, 0.0028f, 0.0020f};
  static const float factor[4] = {0, 1.0f, 1.15f, 0.65f};  // None/Standard/Mini/Maxi
  if (!estFuseType || fuseRating <= 0) return 0;
  float b = 0.120f / fuseRating;  // inverse-fit fallback for a custom rating
  for (int i = 0; i < FUSE_N; i++) if (FUSE_RATINGS[i] == fuseRating) { b = base[i]; break; }
  return b * factor[estFuseType];
}

// Predicted circuit build-out resistance: copper wire (0.0175 ohm*mm2/m at
// 20 C) + contacts (4 mohm per mated male/female pair incl. crimps — typical
// for healthy tin-plated automotive terminals; corroded ones run 10x+) + fuse.
static float estimateBuildR(float &wireR, float &connR, float &fuseR) {
  wireR = (estWireMm2 > 0 && estWireLen > 0) ? 0.0175f * estWireLen / estWireMm2 : 0;
  connR = estConns * 0.004f;
  fuseR = fuseTypeR();
  return wireR + connR + fuseR;
}

static void fmtOhm(char *b, int n, float x) {
  float ax = fabsf(x);
  if (ax < 0.9995f) snprintf(b, n, "%s%.1f mohm", x < 0 ? "-" : "", ax * 1000);
  else snprintf(b, n, "%s%.3f ohm", x < 0 ? "-" : "", ax);
}

// modes incl. the RT / BATT pseudo-modes
static const int   MODE_N = 8;
static const int   MODE_IDS[MODE_N]  = {el15::MODE_CC, el15::MODE_CV, el15::MODE_CR,
                                        el15::MODE_CP, el15::MODE_CAP, el15::MODE_DCR, MODE_RT, MODE_BATT};
static const char *MODE_ABBR[MODE_N] = {"CC", "CV", "CR", "CP", "CAP", "DCR", "RT", "BATT"};
static const char *MODE_NAME[MODE_N] = {"Constant Current", "Constant Voltage", "Constant Resistance",
                                        "Constant Power", "Capacity", "DC Resistance", "Resistance Test",
                                        "Battery Capacity"};

// ---- Per-unit config (min,max,dp, 3 step sizes + default idx, 4 presets) ----
struct UnitCfg { float lo, hi; int dp; float step[3]; int defStep; float preset[4]; };
static UnitCfg unitCfg(const char *u) {
  if (strcmp(u, "V") == 0)    return {0, 150, 1, {0.1f, 1, 10}, 1, {3.7f, 5, 12, 24}};
  if (strcmp(u, "ohm") == 0)  return {0.05f, 9999, 1, {0.1f, 1, 10}, 1, {1, 5, 10, 50}};
  if (strcmp(u, "W") == 0)    return {0, 400, 0, {1, 10, 50}, 1, {10, 25, 50, 100}};
  if (strcmp(u, "m") == 0)    return {0, 100, 1, {0.1f, 1, 5}, 1, {1, 2, 5, 10}};  // wire length
  return {0, 40, 2, {0.01f, 0.1f, 1}, 1, {0.5f, 1, 2, 5}};  // A (default)
}
static bool isRT() { return curMode == MODE_RT; }
static bool isBatt() { return curMode == MODE_BATT; }
static const char *modeUnit() {
  if (isRT()) return "A";
  if (isBatt()) return "V";
  // CR's protocol unit is the UTF-8 ohm glyph, which Montserrat can't render
  // (tofu) and which fails unitCfg()'s "ohm" match — CR would silently get the
  // Amps range/steps/presets. Map it to the ASCII name the UI uses everywhere.
  if (curMode == el15::MODE_CR) return "ohm";
  return el15::setpointInfo(curMode).unit;   // "A"/"V"/"W"
}
static const char *modeAbbr() {
  for (int i = 0; i < MODE_N; i++) if (MODE_IDS[i] == curMode) return MODE_ABBR[i];
  return "?";
}
static const char *modeName() {
  for (int i = 0; i < MODE_N; i++) if (MODE_IDS[i] == curMode) return MODE_NAME[i];
  return "";
}

// ---- Widget handles --------------------------------------------------------
static lv_obj_t *scrRoot, *contentStack;
static lv_obj_t *monScreen, *adjScreen, *graphScreen, *rtestScreen, *connectScreen, *setScreen, *battScreen;
static lv_obj_t *menuOverlay, *kpOverlay, *pickerOverlay;

static lv_obj_t *stDot, *stConnLabel, *stConnGroup, *stBack, *stBackLabel, *stMenuBtn;
static lv_obj_t *infoBar, *ibPower, *ibFan, *ibTemp, *ibRuntime, *ibExtra;
static lv_obj_t *faultBanner, *faultTitle, *faultMsg;
// The fault banner doubles as the emergency-stop acknowledgement. When shown
// for an e-stop it must stay up (a clean status packet would otherwise hide it
// on the next poll) until the user taps it or a real protection trip supersedes.
static bool faultIsEmergency = false;

static lv_obj_t *modeAbbrLbl, *modeNameLbl, *setLabelLbl, *setValLbl, *setUnitLbl;
static lv_obj_t *vHeroBlock, *vHeroVal, *iHeroBlock, *iHeroLabelRow, *iHeroSink, *iHeroVal, *iHeroUnit;
static lv_obj_t *rtSetupGroup, *battSetupGroup;  // reparented onto Monitor in RT/BATT mode
static lv_obj_t *loadBar, *loadBtn, *loadIcon, *loadTitle, *loadSub;

static lv_obj_t *adjCaption, *adjVal, *adjUnit, *stepChip[3], *stepChipLbl[3];
static lv_obj_t *graphVNum, *graphINum, *chart, *gVRange, *gIRange, *gWin;
static lv_chart_series_t *serV, *serI;

static lv_obj_t *rtIdleBox, *rtRunBox, *rtResultBox;
static lv_obj_t *fuseVal, *stepsVal, *startBtn, *startBtnLbl;
static lv_obj_t *estWireVal, *estLenVal, *estConnVal, *estTypeVal, *estTotalVal;
static lv_obj_t *runStepLbl, *runBar, *runVLbl, *runILbl;
static lv_obj_t *resistVal, *lowConfBox, *resultList, *saveBtn, *saveBtnLbl, *rtStatusLbl;
static lv_obj_t *rtChart, *rcXRange, *rcYRange;
static lv_chart_series_t *rtSerMeas, *rtSerFit;
// Result rows are built ONCE and text-updated per test. Rebuilding them per
// completion (lv_obj_clean + ~45 allocations) interleaved frees with the chart
// buffer reallocs and corrupted the heap -> Load access fault in the next
// layout pass (see the 2026-07-21 panic capture).
static const int RR_N = 16;
enum { RR_VOC, RR_TOL, RR_R2, RR_PSC, RR_SAG, RR_PKW, RR_TEMP, RR_FAN, RR_SWEEP,
       RR_STEPS, RR_FUSELIM, RR_WIRE, RR_CONN, RR_FUSEEST, RR_ESTTOT, RR_RESID };
static lv_obj_t *rrRow[RR_N], *rrKey[RR_N], *rrVal[RR_N];
static const int RT_CHART_PTS = 20;  // fixed capacity = UI max steps; no reallocs
static lv_obj_t *setBriVal, *setBattVal, *setBattState, *setRtcVal, *setHeapVal, *setMinHeapVal, *setUptimeVal;
static lv_obj_t *setVolVal, *setMuteBtn, *setMuteLbl;
// ---- Battery capacity test state ---------------------------------------------
struct BattChem { const char *name; float nom, full, cut; int maxCells; };
static const BattChem BATT_CHEMS[5] = {
    {"Li-ion", 3.7f, 4.2f, 3.0f, 14},
    {"LiFePO4", 3.2f, 3.65f, 2.5f, 16},
    {"Lead-acid", 2.0f, 2.13f, 1.75f, 24},   // per 2 V cell; 24 = 48 V bank
    {"NiMH", 1.2f, 1.4f, 1.0f, 40},
    {"Custom", 0, 0, 0, 0},
};
static int battChem = 0, battCells = 3;
static float battCutoff = 9.0f;          // = cells x per-cell cutoff, or custom
static bool battCutoffCustom = false;
static float battAmps = 1.0f;
enum BattPhase { BT_IDLE, BT_RUN, BT_REST, BT_RESULT };
static BattPhase btPhase = BT_IDLE;
static CapacityTest::Result lastBatt;

// True while either test engine is actively driving the load. Manual controls
// (load toggle, setpoint) and the other engine's start are blocked while busy,
// so two things can never fight over the load — the user stops the running test
// first (STOP on its screen, or the BOOT emergency-stop button).
static bool engineBusy() { return rtPhase == RT_RUN || btPhase == BT_RUN || btPhase == BT_REST; }
static bool battSaved = false;
// Downsampled V-vs-time curve: fixed buffer whose sample stride doubles when
// full, so it always spans the whole test in bounded memory.
// Discharge-curve storage: a dense voltage reservoir (halved when it fills, to
// stay bounded) that is RESAMPLED onto the chart every refresh. Drawing always
// stretches the whole reservoir across the full chart width against a live
// [0, elapsed] axis, so the time axis grows continuously in real time instead
// of jumping when the buffer decimates. The reservoir is much larger than the
// chart so its (rare) halving never drops the drawn resolution below full.
static const int BATT_RES_N = 480;    // raw voltage reservoir
static const int BATT_CHART_N = 120;  // points actually drawn on the chart
static float btHistV[BATT_RES_N];
static int btHistN = 0, btHistStride = 1, btHistAcc = 0;
static uint32_t btLastElapsed = 0;

static lv_obj_t *btIdleBox, *btRunBox, *btResultBox, *btChartCard;
static lv_obj_t *btChemVal, *btCellsVal, *btVocLbl, *btCutoffVal, *btAmpsVal, *btStartBtn, *btStartLbl, *btStatusLbl;
static lv_obj_t *btPhaseLbl, *btElapsedLbl, *btVLbl, *btCutSub, *btILbl, *btAhLbl, *btWhLbl, *btTempLbl;
static lv_obj_t *btChart, *btChartYLbl, *btChartXLbl;
static lv_chart_series_t *btSer;
static lv_obj_t *btAhBig, *btWhSub, *btSaveLbl;
static const int BR_N = 10;
static lv_obj_t *brVal[BR_N];

static int pollMs = 500;  // status sampling interval, mirrored to BLE + R-test
static const int RATE_MS[4] = {100, 250, 500, 1000};
static const char *RATE_NAMES[4] = {"10 Hz", "4 Hz", "2 Hz", "1 Hz"};
static lv_obj_t *rateChip[4], *rateChipLbl[4];

static lv_obj_t *connDot2, *connLabel2, *connDisc, *scanBtn, *deviceList;
static lv_obj_t *kpTitle, *kpValue, *kpUnit, *kpPreset[4], *kpPresetBtn[4];
static lv_obj_t *modeTile[MODE_N];

// ---- Forward declarations --------------------------------------------------
static void showScreen(Screen s);
static void showOverlay(Overlay o);
static void openKeypad(int target);
static void refreshMonitor();
static void refreshAdjust();
static void refreshRtest();
static void refreshChart();
static void refreshPicker();
static void enterRtRun();
static void settingsTick();
static void hhmmss(int t, char *out, int n);
static void refreshBatt();
static void battChartRefresh();
static void enterBattRun();
static void syncMonitorExtras();
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
  // Layout containers must never swallow input: lv_obj is CLICKABLE by default,
  // so an inert cont sitting inside a button (the load button's text stack, a
  // device row's text column, ...) would eat the tap and the button never fires.
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
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
  // A soft tick on every button press — central here so all controls click.
  lv_obj_add_event_cb(b, [](lv_event_t *) { audio::click(); }, LV_EVENT_PRESSED, nullptr);
  return b;
}
static void fmtVal(char *b, int n, float v, int dp) { snprintf(b, n, "%.*f", dp, v); }

// Set a label's text only when it actually changed. lv_label_set_text
// invalidates (and so repaints) even for identical text; the 2 Hz status
// updates rewriting the whole chrome blocked touch polling for 60-75 ms per
// update, which is long enough to swallow quick taps.
static void setTextIf(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

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
  lv_obj_t *all[7] = {monScreen, adjScreen, graphScreen, rtestScreen, connectScreen, setScreen, battScreen};
  Screen ids[7] = {SCR_MON, SCR_ADJ, SCR_GRAPH, SCR_RTEST, SCR_CONNECT, SCR_SET, SCR_BATT};
  for (int i = 0; i < 7; i++) {
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
                        : s == SCR_RTEST ? LV_SYMBOL_LEFT " R-Test" : s == SCR_SET ? LV_SYMBOL_LEFT " Settings"
                        : s == SCR_BATT ? LV_SYMBOL_LEFT " Battery"
                        : LV_SYMBOL_LEFT " Connect";
    lv_label_set_text(stBackLabel, title);
  }
  syncMonitorExtras();
  if (s == SCR_MON) refreshMonitor();
  if (s == SCR_ADJ) refreshAdjust();
  if (s == SCR_GRAPH) refreshChart();
  if (s == SCR_SET) settingsTick();
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
  // 22/26 px side insets keep the dot and Menu button clear of the panel's
  // physical rounded corners, which swallow ~15-25 px at the strip's height.
  lv_obj_set_style_pad_hor(strip, 22, 0);
  lv_obj_set_style_pad_right(strip, 26, 0);
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
  lv_obj_set_ext_click_area(stConnGroup, 14);
  lv_obj_add_event_cb(stConnGroup, [](lv_event_t *) { showScreen(SCR_CONNECT); }, LV_EVENT_CLICKED, nullptr);
  stDot = lv_obj_create(stConnGroup);
  lv_obj_set_size(stDot, 11, 11);
  lv_obj_clear_flag(stDot, LV_OBJ_FLAG_CLICKABLE);  // don't let the dot eat the group's tap
  lv_obj_set_style_radius(stDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(stDot, 0, 0);
  lv_obj_set_style_bg_color(stDot, COL_MUTED, 0);
  stConnLabel = lbl(stConnGroup, "Offline", COL_INK, F16);

  // back pill (non-Monitor)
  stBack = flatBtn(strip);
  lv_obj_set_size(stBack, LV_SIZE_CONTENT, 34);
  lv_obj_set_ext_click_area(stBack, 14);
  styleCard(stBack, COL_INSET, COL_BORDER, 9, 0);
  lv_obj_set_style_pad_hor(stBack, 10, 0);
  stBackLabel = lbl(stBack, LV_SYMBOL_LEFT " Back", COL_ACCENT2, F16);
  lv_obj_center(stBackLabel);
  lv_obj_add_event_cb(stBack, [](lv_event_t *) { showScreen(SCR_MON); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(stBack, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *sp = cont(strip); lv_obj_set_flex_grow(sp, 1); lv_obj_set_height(sp, 1);

  stMenuBtn = flatBtn(strip);
  // Modest drawn size (a 38 px-tall button rode 2 px under the glass curve);
  // the 12 px ext click area below keeps the effective tap target large.
  lv_obj_set_size(stMenuBtn, 42, 32);
  styleCard(stMenuBtn, COL_INSET, COL_BORDER, 9, 0);
  lv_obj_t *ml = lbl(stMenuBtn, LV_SYMBOL_LIST, COL_ACCENT, F20); lv_obj_center(ml);
  // The 42 px strip caps how big the button can draw, so also extend the touch
  // hit-box past the drawn border — taps anywhere in the top-right corner land.
  lv_obj_set_ext_click_area(stMenuBtn, 12);
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
  lv_obj_set_style_pad_hor(infoBar, 22, 0);  // align the row with the strip content
  lv_obj_set_style_pad_ver(infoBar, 6, 0);
  lv_obj_set_flex_flow(infoBar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(infoBar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  // One telemetry row, one font: power - fan - temp - runtime (- Ah/mohm).
  ibPower = lbl(infoBar, "0.0 W", COL_INK, F14);
  ibFan = lbl(infoBar, LV_SYMBOL_REFRESH " 0/5", COL_MUTED, F14);
  ibTemp = lbl(infoBar, "--", COL_INK, F14);
  ibRuntime = lbl(infoBar, LV_SYMBOL_LOOP " 00:00", COL_MUTED, F14);
  ibExtra = lbl(infoBar, "", COL_MUTED, F14);
  lv_obj_add_flag(ibExtra, LV_OBJ_FLAG_HIDDEN);  // shown only in CAP/DCR
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
  lv_obj_add_event_cb(faultBanner, [](lv_event_t *) { faultIsEmergency = false; lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, nullptr);
  faultTitle = lbl(faultBanner, LV_SYMBOL_WARNING "  -- - PROTECTION", lv_color_hex(0x1a0606), F16);
  faultMsg = lbl(faultBanner, "", lv_color_hex(0x1a0606), F12);
  lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
}

// ---- Monitor ---------------------------------------------------------------
static void buildMonitor() {
  monScreen = cont(contentStack);
  lv_obj_set_size(monScreen, LV_PCT(100), LV_PCT(100));
  // Scrollable so the RT / BATT setup groups can ride below the heroes and be
  // reached by scrolling right on the main screen (see syncMonitorExtras).
  lv_obj_add_flag(monScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(monScreen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scrollbar_mode(monScreen, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(monScreen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(monScreen, 11, 0);
  lv_obj_set_style_pad_row(monScreen, 6, 0);

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
      fuseRating = FUSE_RATINGS[(idx + 1) % FUSE_N]; refreshMonitor(); refreshRtest();
    } else if (isBatt()) openKeypad(4);
    else showScreen(SCR_ADJ);
  }, LV_EVENT_CLICKED, nullptr);

  // Voltage hero
  lv_obj_t *vh = cont(monScreen);
  vHeroBlock = vh;
  lv_obj_set_width(vh, LV_PCT(100));
  lv_obj_set_flex_grow(vh, 1);
  // Pad 6, not 12: when connected each hero gets ~82 px; 12 px padding plus the
  // F12 caption (15) and F48 digits (52) needs 91 px and clips the digit bottoms.
  styleCard(vh, COL_VHERO_BG, COL_VHERO_BD, 14, 6);
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
  styleCard(iHeroBlock, COL_IHERO_BG, COL_IHERO_BD, 14, 6);  // pad 6: see voltage hero
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
  iHeroUnit = lbl(irow, "A", COL_AMBER, F24);
}

// ---- Load / RUN TEST bar ---------------------------------------------------
static void buildLoadBar() {
  loadBar = cont(scrRoot);
  lv_obj_set_size(loadBar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(loadBar, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(loadBar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(loadBar, 9, 0);
  lv_obj_set_style_pad_hor(loadBar, 16, 0);  // clear of the glass's bottom rounded corners
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
    if (engineBusy()) return;   // a test owns the load; stop it first
    if (isRT()) {
      if (!fuseRating) return;
      if (!connected) { showScreen(SCR_CONNECT); return; }
      if (A.startRTest) { A.startRTest(fuseRating, rtSteps); enterRtRun(); }
      return;
    }
    if (isBatt()) {
      if (battCutoff <= 0.05f || battAmps <= 0.005f) return;
      if (!connected) { showScreen(SCR_CONNECT); return; }
      if (A.startBatt) { A.startBatt(battCutoff, battAmps); enterBattRun(); }
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
  if (engineBusy()) return;   // don't fight an engine that owns the setpoint
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

  // Value card doubles as the "type exact value" button (keyboard glyph in the
  // caption hints it) — dropping the separate row frees ~50 px for the +/- pads.
  lv_obj_t *vc = flatBtn(adjScreen);
  lv_obj_set_width(vc, LV_PCT(100));
  styleCard(vc, COL_READOUT, COL_BORDER, 14, 8);
  lv_obj_set_flex_flow(vc, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_event_cb(vc, [](lv_event_t *) { openKeypad(1); }, LV_EVENT_CLICKED, nullptr);
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
  lv_obj_add_event_cb(minus, [](lv_event_t *) { stepApply(-1); }, LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(minus, [](lv_event_t *) { stepApply(-1); }, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
  lv_obj_t *plus = flatBtn(pads);
  lv_obj_set_flex_grow(plus, 1); lv_obj_set_height(plus, LV_PCT(100));
  styleCard(plus, COL_CARD, COL_ACCENT, 15, 0);
  lv_obj_set_style_bg_color(plus, lv_color_hex(0x171630), 0);
  lv_obj_t *pl = lbl(plus, LV_SYMBOL_PLUS, COL_ACCENT2, F40); lv_obj_center(pl);
  lv_obj_add_event_cb(plus, [](lv_event_t *) { stepApply(+1); }, LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(plus, [](lv_event_t *) { stepApply(+1); }, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
}

static void refreshAdjust() {
  UnitCfg c = unitCfg(modeUnit());
  char b[64];
  snprintf(b, sizeof(b), LV_SYMBOL_KEYBOARD "  %s - range %g-%g %s", modeName(), c.lo, c.hi, modeUnit());
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
  snprintf(b, sizeof(b), "~%ds", histCount * pollMs / 1000); lv_label_set_text(gWin, b);
}

// ---- R-Test ----------------------------------------------------------------

static void buildRtest() {
  rtestScreen = cont(contentStack);
  lv_obj_set_size(rtestScreen, LV_PCT(100), LV_PCT(100));
  lv_obj_add_flag(rtestScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(rtestScreen, LV_OBJ_FLAG_CLICKABLE);  // so background drags scroll
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
  // Setup controls live in one group so they can be reparented onto the
  // Monitor screen when RT is the active UI mode.
  rtSetupGroup = cont(rtIdleBox);
  lv_obj_set_size(rtSetupGroup, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rtSetupGroup, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rtSetupGroup, 12, 0);
  lv_obj_t *fuseTile = flatBtn(rtSetupGroup);
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
  lv_obj_t *stepsRow = cont(rtSetupGroup);
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
  // circuit estimate (optional): wire + connections + fuse -> predicted R
  lv_obj_t *estCard = cont(rtSetupGroup);
  lv_obj_set_size(estCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(estCard, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(estCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(estCard, 2, 0);
  lbl(estCard, "CIRCUIT ESTIMATE - optional", COL_MUTED, F12);
  lv_obj_t *estNote = lbl(estCard, "Describe the wiring to predict its resistance and compare it with the measurement.", COL_FAINT, F12);
  lv_label_set_long_mode(estNote, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(estNote, LV_PCT(100));

  auto estRow = [](lv_obj_t *parent, const char *k, lv_obj_t **valOut, lv_event_cb_t cb) {
    lv_obj_t *row = flatBtn(parent);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lbl(row, k, COL_MUTED, F14);
    *valOut = lbl(row, "--", COL_ACCENT2, F16);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
  };
  estRow(estCard, "Wire size - tap to cycle", &estWireVal, [](lv_event_t *) {
    int idx = 0;
    for (int i = 0; i < WIRE_SIZES_N; i++) if (WIRE_SIZES[i] == estWireMm2) idx = i;
    estWireMm2 = WIRE_SIZES[(idx + 1) % WIRE_SIZES_N];
    refreshRtest();
  });
  estRow(estCard, "Wire length - tap to type", &estLenVal, [](lv_event_t *) { openKeypad(3); });
  lv_obj_t *crow = cont(estCard);
  lv_obj_set_size(crow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(crow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lbl(crow, "Connections", COL_MUTED, F14);
  lv_obj_t *cgrp = cont(crow);
  lv_obj_set_size(cgrp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cgrp, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cgrp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(cgrp, 10, 0);
  lv_obj_t *cm = flatBtn(cgrp);
  lv_obj_set_size(cm, 44, 38);
  styleCard(cm, COL_INSET, COL_BORDER, 10, 0);
  lv_obj_t *cml = lbl(cm, LV_SYMBOL_MINUS, COL_INK, F14); lv_obj_center(cml);
  lv_obj_add_event_cb(cm, [](lv_event_t *) { if (estConns > 0) estConns--; refreshRtest(); }, LV_EVENT_CLICKED, nullptr);
  estConnVal = lbl(cgrp, "0", COL_ACCENT2, F16);
  lv_obj_t *cp = flatBtn(cgrp);
  lv_obj_set_size(cp, 44, 38);
  styleCard(cp, COL_INSET, COL_BORDER, 10, 0);
  lv_obj_t *cpl = lbl(cp, LV_SYMBOL_PLUS, COL_INK, F14); lv_obj_center(cpl);
  lv_obj_add_event_cb(cp, [](lv_event_t *) { if (estConns < 20) estConns++; refreshRtest(); }, LV_EVENT_CLICKED, nullptr);
  estRow(estCard, "Fuse type - tap to cycle", &estTypeVal, [](lv_event_t *) {
    estFuseType = (estFuseType + 1) % 4;
    refreshRtest();
  });
  lv_obj_t *trow = cont(estCard);
  lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_ver(trow, 6, 0);
  lbl(trow, "Predicted build R", COL_MUTED, F14);
  estTotalVal = lbl(trow, "--", COL_GREEN, F16);

  startBtn = flatBtn(rtIdleBox);
  lv_obj_set_size(startBtn, LV_PCT(100), 62);
  lv_obj_set_style_radius(startBtn, 14, 0);
  startBtnLbl = lbl(startBtn, LV_SYMBOL_PLAY "  Start sweep", COL_DARKINK, F20);
  lv_obj_center(startBtnLbl);
  lv_obj_add_event_cb(startBtn, [](lv_event_t *) {
    if (engineBusy()) return;
    if (!fuseRating) return;
    if (!connected) { showScreen(SCR_CONNECT); return; }
    if (A.startRTest) { A.startRTest(fuseRating, rtSteps); enterRtRun(); }
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
  lv_obj_add_event_cb(stopBtn, [](lv_event_t *) {
    if (A.stopRTest) A.stopRTest();
    // The engine's stop() fires no callback, so flip the UI back ourselves —
    // otherwise the screen stays on "RUNNING" forever.
    rtPhase = RT_IDLE;
    refreshRtest();
  }, LV_EVENT_CLICKED, nullptr);

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

  // V-I sweep chart: measured step averages (amber) + the least-squares fit
  // line (green) from the Voc intercept down to the max test current.
  lv_obj_t *chartCard = cont(rtResultBox);
  lv_obj_set_size(chartCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(chartCard, COL_READOUT, COL_BORDER2, 12, 8);
  lv_obj_set_flex_flow(chartCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(chartCard, 4, 0);
  lbl(chartCard, "V-I SWEEP - MEASURED vs FIT", COL_MUTED, F12);
  rtChart = lv_chart_create(chartCard);
  lv_obj_set_width(rtChart, LV_PCT(100));
  lv_obj_set_height(rtChart, 128);
  lv_obj_set_style_bg_opa(rtChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rtChart, 0, 0);
  lv_obj_set_style_pad_all(rtChart, 2, 0);
  lv_obj_set_style_line_color(rtChart, COL_BORDER2, LV_PART_MAIN);
  lv_obj_set_style_width(rtChart, 5, LV_PART_INDICATOR);
  lv_obj_set_style_height(rtChart, 5, LV_PART_INDICATOR);
  // LINE, not SCATTER: the current levels are evenly spaced by construction, so
  // plotting V against step index is truthful, and LVGL 8.4's line renderer
  // skips LV_CHART_POINT_NONE tail slots cleanly — the scatter renderer draws
  // them as stray far-off points (the recurring green-streak artifact).
  lv_chart_set_type(rtChart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(rtChart, 3, 4);
  rtSerFit = lv_chart_add_series(rtChart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
  rtSerMeas = lv_chart_add_series(rtChart, COL_AMBER, LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_point_count(rtChart, RT_CHART_PTS);  // allocate once, never resize
  lv_obj_t *chartRng = cont(chartCard);
  lv_obj_set_size(chartRng, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chartRng, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chartRng, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  rcYRange = lbl(chartRng, "", COL_GREEN, F12);
  rcXRange = lbl(chartRng, "", COL_AMBER, F12);

  resultList = cont(rtResultBox);
  lv_obj_set_size(resultList, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(resultList, COL_INSET, COL_BORDER2, 12, 4);
  lv_obj_set_style_pad_hor(resultList, 13, 0);
  lv_obj_set_flex_flow(resultList, LV_FLEX_FLOW_COLUMN);
  static const char *RR_KEYS[RR_N] = {
      "Open-circuit voltage", "Uncertainty (+/-)", "Fit quality (R2)", "Est. short-circuit I",
      "Sag at max current", "Peak test power", "Load temp", "Max fan",
      "Current sweep", "Steps / samples", "Fuse limit",
      "Wire", "Contacts", "Fuse (est)", "Est. build R", "Residual vs est."};
  for (int i = 0; i < RR_N; i++) {
    lv_obj_t *row = cont(resultList);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 8, 0);
    if (i < RR_N - 1) {
      lv_obj_set_style_border_color(row, lv_color_hex(0x161d26), 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    }
    rrRow[i] = row;
    rrKey[i] = lbl(row, RR_KEYS[i], COL_MUTED, F14);
    rrVal[i] = lbl(row, "--", COL_INK, F14);
  }
  for (int i = RR_WIRE; i < RR_N; i++) lv_obj_add_flag(rrRow[i], LV_OBJ_FLAG_HIDDEN);
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
  // Each phase (Idle / Running / Result) is a different "view" sharing one
  // scrollable screen — reset to the top when the phase changes so a new view
  // never inherits the previous one's scroll position. Plain value refreshes
  // (fuse cycling, estimator edits) keep the user's scroll untouched.
  static RtPhase lastShownPhase = RT_IDLE;
  if (rtPhase != lastShownPhase) {
    lastShownPhase = rtPhase;
    lv_obj_scroll_to_y(rtestScreen, 0, LV_ANIM_OFF);
  }
  lv_obj_add_flag(rtIdleBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(rtRunBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(rtResultBox, LV_OBJ_FLAG_HIDDEN);
  if (rtPhase == RT_IDLE) {
    lv_obj_clear_flag(rtIdleBox, LV_OBJ_FLAG_HIDDEN);
    char b[24];
    if (fuseRating) { snprintf(b, sizeof(b), "%g", fuseRating); lv_label_set_text(fuseVal, b); lv_obj_set_style_text_color(fuseVal, COL_INK, 0); }
    else { lv_label_set_text(fuseVal, "--"); lv_obj_set_style_text_color(fuseVal, COL_FAINT, 0); }
    snprintf(b, sizeof(b), "%d", rtSteps); lv_label_set_text(stepsVal, b);
    if (estWireMm2 > 0) { snprintf(b, sizeof(b), "%g mm2", estWireMm2); lv_label_set_text(estWireVal, b); }
    else lv_label_set_text(estWireVal, "--");
    if (estWireLen > 0) { snprintf(b, sizeof(b), "%g m", estWireLen); lv_label_set_text(estLenVal, b); }
    else lv_label_set_text(estLenVal, "--");
    snprintf(b, sizeof(b), "%d", estConns); lv_label_set_text(estConnVal, b);
    lv_label_set_text(estTypeVal, FUSE_TYPE_NAMES[estFuseType]);
    float wR, cR, fR;
    float tR = estimateBuildR(wR, cR, fR);
    if (tR > 0.0001f) { char ob[20]; fmtOhm(ob, sizeof(ob), tR); lv_label_set_text(estTotalVal, ob); }
    else lv_label_set_text(estTotalVal, "--");
    lv_obj_set_style_bg_color(startBtn, fuseRating ? COL_ACCENT : lv_color_hex(0x161d26), 0);
    lv_obj_set_style_bg_opa(startBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(startBtnLbl, fuseRating ? COL_DARKINK : COL_FAINT, 0);
  } else if (rtPhase == RT_RUN) {
    lv_obj_clear_flag(rtRunBox, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(rtResultBox, LV_OBJ_FLAG_HIDDEN);
  }
}

// Immediate feedback when a sweep starts: the engine's first progress callback
// only fires after prime + settle + collect (~3-4 s), which used to leave the
// UI sitting on the previous screen looking dead after RUN TEST / Start sweep.
static void enterRtRun() {
  rtPhase = RT_RUN;
  char b[24];
  snprintf(b, sizeof(b), "Step 0/%d", rtSteps);
  lv_label_set_text(runStepLbl, b);
  lv_bar_set_value(runBar, 0, LV_ANIM_OFF);
  lv_label_set_text(runVLbl, "-- V");
  lv_label_set_text(runILbl, "-- A");
  lv_obj_add_flag(rtStatusLbl, LV_OBJ_FLAG_HIDDEN);  // clear a stale error
  refreshRtest();
  showScreen(SCR_RTEST);
}

// ---- Connect ---------------------------------------------------------------
static std::vector<std::string *> g_addrPool;
static void onDeviceRow(lv_event_t *e) {
  const char *addr = (const char *)lv_event_get_user_data(e);
  if (addr && A.connect) A.connect(addr);
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
  lv_obj_add_flag(connectScreen, LV_OBJ_FLAG_CLICKABLE);  // so background drags scroll
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
  lv_obj_clear_flag(connDot2, LV_OBJ_FLAG_CLICKABLE);
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
  lbl(deviceList, "Scan to find EL15 devices.", COL_FAINT, F12);
}

// ---- Settings ----------------------------------------------------------------
static lv_obj_t *kvRow(lv_obj_t *parent, const char *k) {
  lv_obj_t *row = cont(parent);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_ver(row, 6, 0);
  lbl(row, k, COL_MUTED, F14);
  return lbl(row, "--", COL_INK, F14);
}

static lv_obj_t *settingsCard(const char *caption) {
  lv_obj_t *c = cont(setScreen);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(c, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 2, 0);
  lbl(c, caption, COL_MUTED, F12);
  return c;
}

static void refreshRateChips() {
  for (int i = 0; i < 4; i++) {
    bool on = RATE_MS[i] == pollMs;
    lv_obj_set_style_bg_color(rateChip[i], on ? lv_color_hex(0x1d1b33) : COL_INSET, 0);
    lv_obj_set_style_border_color(rateChip[i], on ? COL_ACCENT : COL_BORDER, 0);
    lv_obj_set_style_text_color(rateChipLbl[i], on ? COL_ACCENT2 : COL_MUTED, 0);
  }
}

static void onRateChip(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  pollMs = RATE_MS[i];
  if (A.setPollRate) A.setPollRate(pollMs);
  refreshRateChips();
}

static void onBrightness(lv_event_t *e) {
  int v = lv_slider_get_value(lv_event_get_target(e));
  display::setBrightness((uint8_t)v);
  char b[12];
  snprintf(b, sizeof(b), "%d%%", v * 100 / 255);
  lv_label_set_text(setBriVal, b);
}

static void refreshMute() {
  bool m = audio::muted();
  lv_label_set_text(setMuteLbl, m ? "Muted" : "Sound on");
  lv_obj_set_style_border_color(setMuteBtn, m ? COL_RED : COL_GREEN, 0);
  lv_obj_set_style_text_color(setMuteLbl, m ? COL_RED : COL_GREEN, 0);
}
static void onVolume(lv_event_t *e) {
  int v = lv_slider_get_value(lv_event_get_target(e));
  audio::setVolume((uint8_t)v);
  char b[12];
  snprintf(b, sizeof(b), "%d%%", v);
  lv_label_set_text(setVolVal, b);
}

static void buildSettings() {
  setScreen = cont(contentStack);
  lv_obj_set_size(setScreen, LV_PCT(100), LV_PCT(100));
  lv_obj_add_flag(setScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(setScreen, LV_OBJ_FLAG_CLICKABLE);  // so background drags scroll
  lv_obj_set_scrollbar_mode(setScreen, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(setScreen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(setScreen, 12, 0);
  lv_obj_set_style_pad_row(setScreen, 10, 0);

  lv_obj_t *title = cont(setScreen);
  lv_obj_set_size(title, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(title, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(title, 8, 0);
  lbl(title, LV_SYMBOL_SETTINGS, COL_ACCENT, F20);
  lbl(title, "Settings", COL_INK, F20);

  // brightness
  lv_obj_t *bc = cont(setScreen);
  lv_obj_set_size(bc, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(bc, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(bc, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(bc, 12, 0);
  lv_obj_t *brow = cont(bc);
  lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lbl(brow, "BRIGHTNESS", COL_MUTED, F12);
  setBriVal = lbl(brow, "--", COL_INK, F14);
  char bb[12];
  snprintf(bb, sizeof(bb), "%d%%", display::getBrightness() * 100 / 255);
  lv_label_set_text(setBriVal, bb);
  lv_obj_t *sl = lv_slider_create(bc);
  lv_obj_set_width(sl, LV_PCT(96));
  lv_obj_set_height(sl, 14);
  lv_slider_set_range(sl, 10, 255);  // floor of 10 so the screen can't go black
  lv_slider_set_value(sl, display::getBrightness(), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, lv_color_hex(0x161d26), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, COL_ACCENT2, LV_PART_KNOB);
  lv_obj_add_event_cb(sl, onBrightness, LV_EVENT_VALUE_CHANGED, nullptr);

  // audio: volume + mute
  lv_obj_t *ac = cont(setScreen);
  lv_obj_set_size(ac, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(ac, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(ac, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(ac, 12, 0);
  lv_obj_t *arow = cont(ac);
  lv_obj_set_size(arow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(arow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lbl(arow, "VOLUME", COL_MUTED, F12);
  setVolVal = lbl(arow, "--", COL_INK, F14);
  char vb[12];
  snprintf(vb, sizeof(vb), "%d%%", audio::getVolume());
  lv_label_set_text(setVolVal, vb);
  lv_obj_t *vsl = lv_slider_create(ac);
  lv_obj_set_width(vsl, LV_PCT(96));
  lv_obj_set_height(vsl, 14);
  lv_slider_set_range(vsl, 0, 100);
  lv_slider_set_value(vsl, audio::getVolume(), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(vsl, lv_color_hex(0x161d26), LV_PART_MAIN);
  lv_obj_set_style_bg_color(vsl, COL_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(vsl, COL_ACCENT2, LV_PART_KNOB);
  lv_obj_add_event_cb(vsl, onVolume, LV_EVENT_VALUE_CHANGED, nullptr);
  setMuteBtn = flatBtn(ac);
  lv_obj_set_size(setMuteBtn, LV_PCT(100), 44);
  styleCard(setMuteBtn, COL_BLACK, COL_GREEN, 11, 0);
  lv_obj_set_style_bg_opa(setMuteBtn, LV_OPA_TRANSP, 0);
  setMuteLbl = lbl(setMuteBtn, "Sound on", COL_GREEN, F16);
  lv_obj_center(setMuteLbl);
  lv_obj_add_event_cb(setMuteBtn, [](lv_event_t *) {
    audio::setMuted(!audio::muted());
    if (!audio::muted()) audio::press();   // audible confirm when un-muting
    refreshMute();
  }, LV_EVENT_CLICKED, nullptr);
  refreshMute();

  // sample rate
  lv_obj_t *src2 = settingsCard("SAMPLE RATE");
  lv_obj_set_style_pad_row(src2, 8, 0);
  lv_obj_t *srNote = lbl(src2, "How often the load is polled - live readouts, graph and R-test sampling.", COL_FAINT, F12);
  lv_label_set_long_mode(srNote, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(srNote, LV_PCT(100));
  lv_obj_t *chips2 = cont(src2);
  lv_obj_set_size(chips2, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chips2, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(chips2, 7, 0);
  for (int i = 0; i < 4; i++) {
    rateChip[i] = flatBtn(chips2);
    lv_obj_set_flex_grow(rateChip[i], 1);
    lv_obj_set_height(rateChip[i], 42);
    styleCard(rateChip[i], COL_INSET, COL_BORDER, 11, 0);
    rateChipLbl[i] = lbl(rateChip[i], RATE_NAMES[i], COL_MUTED, F14);
    lv_obj_center(rateChipLbl[i]);
    lv_obj_add_event_cb(rateChip[i], onRateChip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  refreshRateChips();

  // battery (AXP2101)
  lv_obj_t *pc = settingsCard("BATTERY");
  setBattVal = kvRow(pc, "Level");
  setBattState = kvRow(pc, "State");

  // clock (PCF85063)
  lv_obj_t *cc2 = settingsCard("CLOCK");
  setRtcVal = kvRow(cc2, "RTC");

  // system
  lv_obj_t *sc = settingsCard("SYSTEM");
  setHeapVal = kvRow(sc, "Heap free");
  setMinHeapVal = kvRow(sc, "Heap low-water");
  setUptimeVal = kvRow(sc, "Uptime");
  lv_obj_t *cpuVal = kvRow(sc, "CPU");
  char cb[24];
  snprintf(cb, sizeof(cb), "%u MHz", (unsigned)ESP.getCpuFreqMHz());
  lv_label_set_text(cpuVal, cb);
  lv_obj_t *flashVal = kvRow(sc, "Flash");
  snprintf(cb, sizeof(cb), "%u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  lv_label_set_text(flashVal, cb);
  lv_obj_t *sdkVal = kvRow(sc, "SDK");
  lv_label_set_text(sdkVal, ESP.getSdkVersion());

  // restart
  lv_obj_t *rb = flatBtn(setScreen);
  lv_obj_set_size(rb, LV_PCT(100), 52);
  styleCard(rb, lv_color_hex(0x2a1416), COL_RED, 13, 0);
  lv_obj_t *rl = lbl(rb, LV_SYMBOL_REFRESH "  Restart controller", COL_RED, F16);
  lv_obj_center(rl);
  lv_obj_add_event_cb(rb, [](lv_event_t *) { ESP.restart(); }, LV_EVENT_CLICKED, nullptr);
}

// Refresh the live Settings values (1 Hz timer; cheap no-op off-screen).
static void settingsTick() {
  if (curScreen != SCR_SET) return;
  char b[40];
  int pct, mv, chg;
  bool present;
  if (display::batteryStats(pct, mv, chg, present)) {
    if (!present) { setTextIf(setBattVal, "No battery"); setTextIf(setBattState, "-"); }
    else {
      snprintf(b, sizeof(b), "%d%%  (%.2f V)", pct, mv / 1000.0f);
      setTextIf(setBattVal, b);
      setTextIf(setBattState, (chg >= 1 && chg <= 3) ? "Charging" : chg == 4 ? "Charged" : "Discharging");
    }
  } else { setTextIf(setBattVal, "--"); setTextIf(setBattState, "--"); }
  int y, mo, d, h, mi, s;
  if (display::rtcTime(y, mo, d, h, mi, s)) {
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    setTextIf(setRtcVal, b);
  } else setTextIf(setRtcVal, "Not set");
  snprintf(b, sizeof(b), "%u kB", (unsigned)(ESP.getFreeHeap() / 1024)); setTextIf(setHeapVal, b);
  snprintf(b, sizeof(b), "%u kB", (unsigned)(ESP.getMinFreeHeap() / 1024)); setTextIf(setMinHeapVal, b);
  char up[16];
  hhmmss((int)(millis() / 1000), up, sizeof(up));
  setTextIf(setUptimeVal, up);
}

// ---- Battery capacity test ---------------------------------------------------
static void battHistReset() { btHistN = 0; btHistStride = 1; btHistAcc = 0; btLastElapsed = 0; }

static void battHistPush(float v) {
  if (++btHistAcc < btHistStride) return;
  btHistAcc = 0;
  if (btHistN == BATT_RES_N) {           // full: halve resolution, double stride
    for (int k = 0; k < BATT_RES_N / 2; k++) btHistV[k] = btHistV[k * 2];
    btHistN = BATT_RES_N / 2;
    btHistStride *= 2;
  }
  btHistV[btHistN++] = v;
}

static void battChartRefresh() {
  if (!btChart || btHistN == 0) return;
  float lo = btHistV[0], hi = btHistV[0];
  for (int k = 0; k < btHistN; k++) { lo = LV_MIN(lo, btHistV[k]); hi = LV_MAX(hi, btHistV[k]); }
  if (hi - lo < 0.2f) { lo -= 0.1f; hi += 0.1f; }
  float p = (hi - lo) * 0.10f;
  lv_chart_set_range(btChart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)((lo - p) * 100), (lv_coord_t)((hi + p) * 100));
  // Resample the reservoir across the full chart width: chart point j maps to
  // time fraction j/(BATT_CHART_N-1) of the whole run, so the curve always
  // spans [0, elapsed] and the time axis grows smoothly every refresh.
  for (int j = 0; j < BATT_CHART_N; j++) {
    float v;
    if (btHistN == 1) {
      v = btHistV[0];
    } else {
      float sf = (float)j * (btHistN - 1) / (BATT_CHART_N - 1);
      int i0 = (int)sf;
      if (i0 >= btHistN - 1) v = btHistV[btHistN - 1];
      else { float f = sf - i0; v = btHistV[i0] * (1 - f) + btHistV[i0 + 1] * f; }
    }
    lv_chart_set_value_by_id(btChart, btSer, j, (lv_coord_t)(v * 100));
  }
  char b[24];
  snprintf(b, sizeof(b), "%.2f-%.2f V", lo, hi); lv_label_set_text(btChartYLbl, b);
  char el[16];
  hhmmss((int)btLastElapsed, el, sizeof(el));
  snprintf(b, sizeof(b), "0 - %s", el); lv_label_set_text(btChartXLbl, b);
}

static void refreshBatt() {
  static BattPhase lastShownBt = BT_IDLE;
  if (btPhase != lastShownBt) {
    lastShownBt = btPhase;
    lv_obj_scroll_to_y(battScreen, 0, LV_ANIM_OFF);
  }
  lv_obj_add_flag(btIdleBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(btRunBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(btResultBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(btChartCard, LV_OBJ_FLAG_HIDDEN);
  if (btPhase == BT_IDLE) {
    lv_obj_clear_flag(btIdleBox, LV_OBJ_FLAG_HIDDEN);
    const BattChem &c = BATT_CHEMS[battChem];
    char b[40];
    lv_label_set_text(btChemVal, c.name);
    if (c.maxCells) { snprintf(b, sizeof(b), "%dS", battCells); lv_label_set_text(btCellsVal, b); }
    else lv_label_set_text(btCellsVal, "-");
    snprintf(b, sizeof(b), "%.2f V", battCutoff); lv_label_set_text(btCutoffVal, b);
    snprintf(b, sizeof(b), "%.2f A", battAmps); lv_label_set_text(btAmpsVal, b);
    bool valid = battCutoff > 0.05f && battAmps > 0.005f;
    lv_obj_set_style_bg_color(btStartBtn, valid ? COL_ACCENT : lv_color_hex(0x161d26), 0);
    lv_obj_set_style_bg_opa(btStartBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(btStartLbl, valid ? COL_DARKINK : COL_FAINT, 0);
    if (valid) snprintf(b, sizeof(b), LV_SYMBOL_PLAY "  Discharge %.2f A to %.2f V", battAmps, battCutoff);
    else snprintf(b, sizeof(b), "Set cutoff & current");
    lv_label_set_text(btStartLbl, b);
  } else if (btPhase == BT_RUN || btPhase == BT_REST) {
    lv_obj_clear_flag(btRunBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(btChartCard, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(btResultBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(btChartCard, LV_OBJ_FLAG_HIDDEN);
  }
}

// Immediate feedback on start (the engine primes for ~1.5 s before discharging).
static void enterBattRun() {
  btPhase = BT_RUN;
  battHistReset();
  lv_label_set_text(btPhaseLbl, "PRIMING");
  lv_label_set_text(btElapsedLbl, "00:00");
  lv_label_set_text(btVLbl, "--");
  lv_label_set_text(btILbl, "-- A");
  lv_label_set_text(btAhLbl, "0.000 Ah");
  lv_label_set_text(btWhLbl, "0.0 Wh");
  lv_label_set_text(btTempLbl, "--");
  char b[32];
  snprintf(b, sizeof(b), "auto-stop at %.2f V", battCutoff);
  lv_label_set_text(btCutSub, b);
  lv_obj_add_flag(btStatusLbl, LV_OBJ_FLAG_HIDDEN);
  refreshBatt();
  showScreen(SCR_BATT);
}

static void buildBatt() {
  battScreen = cont(contentStack);
  lv_obj_set_size(battScreen, LV_PCT(100), LV_PCT(100));
  lv_obj_add_flag(battScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(battScreen, LV_OBJ_FLAG_CLICKABLE);  // so background drags scroll
  lv_obj_set_scrollbar_mode(battScreen, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(battScreen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(battScreen, 12, 0);
  lv_obj_set_style_pad_row(battScreen, 12, 0);

  lv_obj_t *title = cont(battScreen);
  lv_obj_set_size(title, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(title, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(title, 8, 0);
  lbl(title, LV_SYMBOL_BATTERY_FULL, COL_ACCENT, F20);
  lbl(title, "Battery Capacity", COL_INK, F20);

  // ---- idle / setup ----
  btIdleBox = cont(battScreen);
  lv_obj_set_size(btIdleBox, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(btIdleBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(btIdleBox, 8, 0);
  lv_obj_t *expl = lbl(btIdleBox, "Discharges at constant current until the cutoff voltage, logging capacity and energy.", COL_MUTED, F12);
  lv_label_set_long_mode(expl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(expl, LV_PCT(100));

  lv_obj_t *setupCard = cont(btIdleBox);
  battSetupGroup = setupCard;  // reparented onto Monitor in BATT mode
  lv_obj_set_size(setupCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(setupCard, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(setupCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(setupCard, 2, 0);
  auto bRow = [](lv_obj_t *parent, const char *k, lv_obj_t **valOut, lv_event_cb_t cb) {
    lv_obj_t *row = flatBtn(parent);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lbl(row, k, COL_MUTED, F14);
    *valOut = lbl(row, "--", COL_ACCENT2, F16);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
  };
  bRow(setupCard, "Chemistry - tap to cycle", &btChemVal, [](lv_event_t *) {
    battChem = (battChem + 1) % 5;
    const BattChem &c = BATT_CHEMS[battChem];
    if (c.maxCells && battCells > c.maxCells) battCells = c.maxCells;
    battCutoffCustom = false;
    if (c.maxCells) battCutoff = c.cut * battCells;
    refreshBatt();
  });
  // cells -/+ row
  lv_obj_t *crow = cont(setupCard);
  lv_obj_set_size(crow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(crow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lbl(crow, "Cells in series", COL_MUTED, F14);
  lv_obj_t *cgrp = cont(crow);
  lv_obj_set_size(cgrp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cgrp, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cgrp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(cgrp, 10, 0);
  lv_obj_t *cm = flatBtn(cgrp);
  lv_obj_set_size(cm, 44, 38);
  styleCard(cm, COL_INSET, COL_BORDER, 10, 0);
  lv_obj_t *cml = lbl(cm, LV_SYMBOL_MINUS, COL_INK, F14); lv_obj_center(cml);
  lv_obj_add_event_cb(cm, [](lv_event_t *) {
    if (battCells > 1) battCells--;
    const BattChem &c = BATT_CHEMS[battChem];
    if (!battCutoffCustom && c.maxCells) battCutoff = c.cut * battCells;
    refreshBatt();
  }, LV_EVENT_CLICKED, nullptr);
  btCellsVal = lbl(cgrp, "3S", COL_ACCENT2, F16);
  lv_obj_t *cp = flatBtn(cgrp);
  lv_obj_set_size(cp, 44, 38);
  styleCard(cp, COL_INSET, COL_BORDER, 10, 0);
  lv_obj_t *cpl = lbl(cp, LV_SYMBOL_PLUS, COL_INK, F14); lv_obj_center(cpl);
  lv_obj_add_event_cb(cp, [](lv_event_t *) {
    const BattChem &c = BATT_CHEMS[battChem];
    int mx = c.maxCells ? c.maxCells : 40;
    if (battCells < mx) battCells++;
    if (!battCutoffCustom && c.maxCells) battCutoff = c.cut * battCells;
    refreshBatt();
  }, LV_EVENT_CLICKED, nullptr);
  btVocLbl = lbl(setupCard, "Voc: connect to read", COL_FAINT, F12);
  lv_label_set_long_mode(btVocLbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(btVocLbl, LV_PCT(100));
  bRow(setupCard, "Cutoff voltage - tap to type", &btCutoffVal, [](lv_event_t *) { openKeypad(4); });
  bRow(setupCard, "Discharge current - tap to type", &btAmpsVal, [](lv_event_t *) { openKeypad(5); });

  btStartBtn = flatBtn(btIdleBox);
  lv_obj_set_size(btStartBtn, LV_PCT(100), 62);
  lv_obj_set_style_radius(btStartBtn, 14, 0);
  btStartLbl = lbl(btStartBtn, LV_SYMBOL_PLAY "  Start discharge", COL_DARKINK, F16);
  lv_obj_center(btStartLbl);
  lv_obj_add_event_cb(btStartBtn, [](lv_event_t *) {
    if (engineBusy()) return;
    if (battCutoff <= 0.05f || battAmps <= 0.005f) return;
    if (!connected) { showScreen(SCR_CONNECT); return; }
    if (A.startBatt) { A.startBatt(battCutoff, battAmps); enterBattRun(); }
  }, LV_EVENT_CLICKED, nullptr);

  // ---- discharge curve (shared by running + result) ----
  btChartCard = cont(battScreen);
  lv_obj_set_size(btChartCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(btChartCard, COL_READOUT, COL_BORDER2, 12, 8);
  lv_obj_set_flex_flow(btChartCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(btChartCard, 4, 0);
  lbl(btChartCard, "DISCHARGE CURVE - V vs time", COL_MUTED, F12);
  btChart = lv_chart_create(btChartCard);
  lv_obj_set_width(btChart, LV_PCT(100));
  lv_obj_set_height(btChart, 120);
  lv_obj_set_style_bg_opa(btChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btChart, 0, 0);
  lv_obj_set_style_pad_all(btChart, 2, 0);
  lv_obj_set_style_line_color(btChart, COL_BORDER2, LV_PART_MAIN);
  lv_obj_set_style_width(btChart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(btChart, 0, LV_PART_INDICATOR);
  lv_chart_set_type(btChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(btChart, BATT_CHART_N);  // fixed capacity, no reallocs
  lv_chart_set_div_line_count(btChart, 3, 4);
  btSer = lv_chart_add_series(btChart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_t *chartRng = cont(btChartCard);
  lv_obj_set_size(chartRng, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chartRng, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chartRng, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  btChartYLbl = lbl(chartRng, "", COL_GREEN, F12);
  btChartXLbl = lbl(chartRng, "", COL_MUTED, F12);
  lv_obj_add_flag(btChartCard, LV_OBJ_FLAG_HIDDEN);

  // ---- running ----
  btRunBox = cont(battScreen);
  lv_obj_set_size(btRunBox, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(btRunBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(btRunBox, 12, 0);
  lv_obj_add_flag(btRunBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *runCard = cont(btRunBox);
  lv_obj_set_size(runCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(runCard, COL_READOUT, lv_color_hex(0x3A3568), 14, 14);
  lv_obj_set_flex_flow(runCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(runCard, 8, 0);
  lv_obj_t *rr = cont(runCard);
  lv_obj_set_size(rr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  btPhaseLbl = lbl(rr, "DISCHARGING", COL_ACCENT, F14);
  btElapsedLbl = lbl(rr, "00:00", COL_INK, F16);
  lv_obj_t *vrow = cont(runCard);
  lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(vrow, 5, 0);
  btVLbl = lbl(vrow, "--", COL_GREEN, F44);
  lbl(vrow, "V", COL_GREEN, F20);
  btCutSub = lbl(runCard, "auto-stop at -- V", COL_MUTED, F12);
  lv_obj_t *iarow = cont(runCard);
  lv_obj_set_size(iarow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(iarow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(iarow, 18, 0);
  btILbl = lbl(iarow, "-- A", COL_AMBER, F28);
  btAhLbl = lbl(iarow, "0.000 Ah", COL_INK, F28);
  lv_obj_t *werow = cont(runCard);
  lv_obj_set_size(werow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(werow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(werow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  btWhLbl = lbl(werow, "0.0 Wh", COL_MUTED, F14);
  btTempLbl = lbl(werow, "--", COL_MUTED, F14);
  lv_obj_t *stopBtn = flatBtn(btRunBox);
  lv_obj_set_size(stopBtn, LV_PCT(100), 66);
  styleCard(stopBtn, lv_color_hex(0x2a1416), COL_RED, 14, 0);
  lv_obj_set_style_border_width(stopBtn, 2, 0);
  lv_obj_t *stl = lbl(stopBtn, LV_SYMBOL_STOP "  STOP", COL_RED, F20); lv_obj_center(stl);
  lv_obj_add_event_cb(stopBtn, [](lv_event_t *) {
    // The engine always answers a stop with onBattComplete (partial data) or
    // onBattError ("Cancelled" during priming) — the callback flips the phase.
    if (A.stopBatt) A.stopBatt();
  }, LV_EVENT_CLICKED, nullptr);

  // ---- result ----
  btResultBox = cont(battScreen);
  lv_obj_set_size(btResultBox, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(btResultBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(btResultBox, 12, 0);
  lv_obj_add_flag(btResultBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *resCard = cont(btResultBox);
  lv_obj_set_size(resCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(resCard, COL_READOUT, COL_BORDER, 14, 14);
  lv_obj_set_flex_flow(resCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(resCard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(resCard, 3, 0);
  lbl(resCard, "CAPACITY", COL_MUTED, F12);
  btAhBig = lbl(resCard, "-- Ah", COL_GREEN, F44);
  btWhSub = lbl(resCard, "", COL_MUTED, F14);
  lv_obj_t *rowsCard = cont(btResultBox);
  lv_obj_set_size(rowsCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(rowsCard, COL_INSET, COL_BORDER2, 12, 6);
  lv_obj_set_style_pad_hor(rowsCard, 13, 0);
  lv_obj_set_flex_flow(rowsCard, LV_FLEX_FLOW_COLUMN);
  static const char *BR_KEYS[BR_N] = {
      "Duration", "Stop reason", "Start voltage", "End voltage (loaded)",
      "Rebound (rested)", "Average voltage", "Average current", "Temp range",
      "Cutoff", "Discharge current"};
  for (int i = 0; i < BR_N; i++) brVal[i] = kvRow(rowsCard, BR_KEYS[i]);
  lv_obj_t *saveBtn2 = flatBtn(btResultBox);
  lv_obj_set_size(saveBtn2, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(saveBtn2, COL_ACCENT, 0);
  lv_obj_set_style_bg_opa(saveBtn2, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(saveBtn2, 13, 0);
  btSaveLbl = lbl(saveBtn2, LV_SYMBOL_SAVE "  Save to SD card", COL_DARKINK, F16);
  lv_obj_center(btSaveLbl);
  lv_obj_add_event_cb(saveBtn2, [](lv_event_t *) {
    if (battSaved) return;
    if (A.saveBatt) A.saveBatt();
    battSaved = true;
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *newBtn2 = flatBtn(btResultBox);
  lv_obj_set_size(newBtn2, LV_PCT(100), 52);
  styleCard(newBtn2, COL_BLACK, COL_BORDER, 13, 0);
  lv_obj_set_style_bg_opa(newBtn2, LV_OPA_TRANSP, 0);
  lv_obj_t *nbl2 = lbl(newBtn2, "New test", COL_ACCENT2, F16); lv_obj_center(nbl2);
  lv_obj_add_event_cb(newBtn2, [](lv_event_t *) { btPhase = BT_IDLE; battSaved = false; refreshBatt(); }, LV_EVENT_CLICKED, nullptr);

  btStatusLbl = lbl(battScreen, "", COL_AMBER, F12);
  lv_label_set_long_mode(btStatusLbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(btStatusLbl, LV_PCT(100));
  lv_obj_add_flag(btStatusLbl, LV_OBJ_FLAG_HIDDEN);
}

// ---- Menu overlay ----------------------------------------------------------
static void buildMenu() {
  menuOverlay = cont(lv_layer_top());
  lv_obj_add_flag(menuOverlay, LV_OBJ_FLAG_CLICKABLE);  // modal barrier: catch stray taps
  lv_obj_set_size(menuOverlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(menuOverlay, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(menuOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(menuOverlay, 16, 0);  // keep the corner X clear of the rounded glass
  lv_obj_set_flex_flow(menuOverlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(menuOverlay, 10, 0);
  lv_obj_add_flag(menuOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *hdr = cont(menuOverlay);
  lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 8, 0);
  lv_obj_t *x = flatBtn(hdr); lv_obj_set_size(x, 40, 40);
  lv_obj_set_ext_click_area(x, 12);
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
  struct MItem { const char *sym; const char *name; const char *note; int act; };  // act: 0..7
  static const MItem items[8] = {
    {LV_SYMBOL_EYE_OPEN, "Monitor", "Live readout", 0},
    {LV_SYMBOL_EDIT, "Adjust", "Set setpoint", 1},
    {LV_SYMBOL_SHUFFLE, "Mode", "CC/CV/CR/...", 2},
    {LV_SYMBOL_UP, "Graph", "Live trend", 3},
    {LV_SYMBOL_LOOP, "R-Test", "Sweep resistance", 4},
    {LV_SYMBOL_BLUETOOTH, "Connect", "Manage device", 5},
    {LV_SYMBOL_SETTINGS, "Settings", "Brightness - system", 6},
    {LV_SYMBOL_BATTERY_FULL, "Battery", "Capacity test", 7},
  };
  for (int i = 0; i < 8; i++) {
    lv_obj_t *t = flatBtn(grid);
    // 84 px: 4 rows + 3 gaps must fit the ~366 px grid (448 - chrome, 16 px
    // overlay pad) now that Settings makes seven tiles.
    lv_obj_set_size(t, 164, 84);
    styleCard(t, COL_CARD, COL_BORDER, 14, 8);
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
        case 6: showScreen(SCR_SET); break;
        case 7: showScreen(SCR_BATT); break;
      }
    }, LV_EVENT_CLICKED, (void *)(intptr_t)items[i].act);
  }
}

// ---- Keypad overlay --------------------------------------------------------
static void kpRefresh() {
  lv_label_set_text(kpValue, kpBuf.empty() ? "0" : kpBuf.c_str());
  const char *unit = kpTarget == 2 ? "A" : kpTarget == 3 ? "m" : kpTarget == 4 ? "V"
                   : kpTarget == 5 ? "A" : modeUnit();
  lv_label_set_text(kpUnit, unit);
  lv_label_set_text(kpTitle, kpTarget == 2 ? "Fuse rating" : kpTarget == 3 ? "Wire length"
                             : kpTarget == 4 ? "Cutoff voltage" : kpTarget == 5 ? "Discharge current"
                             : modeName());
  UnitCfg c = unitCfg(unit);
  for (int i = 0; i < 4; i++) {
    char b[20]; snprintf(b, sizeof(b), "%g", c.preset[i]);
    lv_label_set_text(kpPreset[i], b);
    lv_obj_set_user_data(kpPresetBtn[i], (void *)(intptr_t)(int)(c.preset[i] * 100 + 0.5f));
  }
}
static void openKeypad(int target) {
  kpTarget = target;
  char b[16];
  if (target == 1) { snprintf(b, sizeof(b), "%g", setpoint); kpBuf = b; }
  else if (target == 3) { if (estWireLen > 0) { snprintf(b, sizeof(b), "%g", estWireLen); kpBuf = b; } else kpBuf = ""; }
  else if (target == 4) { snprintf(b, sizeof(b), "%g", battCutoff); kpBuf = b; }
  else if (target == 5) { snprintf(b, sizeof(b), "%g", battAmps); kpBuf = b; }
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
  // Target 1 pushes a live setpoint; suppress it while an engine owns the load.
  // The other targets are test configuration and are safe to edit any time.
  if (kpTarget == 1) { if (!engineBusy()) { setpoint = v; if (A.setSetpoint) A.setSetpoint(v); } refreshAdjust(); refreshMonitor(); }
  else if (kpTarget == 3) { estWireLen = v < 0 ? 0 : v > 100 ? 100 : v; refreshRtest(); }
  else if (kpTarget == 4) { battCutoff = v < 0.1f ? 0.1f : v > 60 ? 60 : v; battCutoffCustom = true; refreshBatt(); refreshMonitor(); }
  else if (kpTarget == 5) { battAmps = v < 0.01f ? 0.01f : v > 12 ? 12 : v; refreshBatt(); }
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
  lv_obj_add_flag(kpOverlay, LV_OBJ_FLAG_CLICKABLE);  // modal barrier: catch stray taps
  lv_obj_set_size(kpOverlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(kpOverlay, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(kpOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(kpOverlay, 16, 0);  // keep the corner X clear of the rounded glass
  lv_obj_set_flex_flow(kpOverlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(kpOverlay, 6, 0);
  lv_obj_add_flag(kpOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *hdr = cont(kpOverlay);
  lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 8, 0);
  lv_obj_t *x = flatBtn(hdr); lv_obj_set_size(x, 40, 40);
  lv_obj_set_ext_click_area(x, 12);
  styleCard(x, COL_CARD, COL_BORDER, 10, 0);
  lv_obj_t *xl = lbl(x, LV_SYMBOL_CLOSE, COL_MUTED, F16); lv_obj_center(xl);
  lv_obj_add_event_cb(x, [](lv_event_t *) { showOverlay(OV_NONE); }, LV_EVENT_CLICKED, nullptr);
  kpTitle = lbl(hdr, "Setpoint", COL_INK, F16);
  lv_obj_t *disp = cont(kpOverlay);
  lv_obj_set_size(disp, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(disp, COL_READOUT, COL_BORDER, 12, 6);
  lv_obj_set_flex_flow(disp, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(disp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(disp, 8, 0);
  kpValue = lbl(disp, "0", COL_INK, F40);
  kpUnit = lbl(disp, "A", COL_ACCENT, F20);
  lv_obj_t *pr = cont(kpOverlay);
  lv_obj_set_size(pr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(pr, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(pr, 6, 0);
  for (int i = 0; i < 4; i++) {
    lv_obj_t *b = flatBtn(pr);
    lv_obj_set_flex_grow(b, 1); lv_obj_set_height(b, 38);
    styleCard(b, COL_CARD, COL_BORDER, 9, 0);
    kpPresetBtn[i] = b;
    kpPreset[i] = lbl(b, "-", COL_ACCENT, F16); lv_obj_center(kpPreset[i]);
    lv_obj_add_event_cb(b, onPreset, LV_EVENT_CLICKED, nullptr);
  }
  // Key grid: 4 flex-grow rows of 3 flex-grow keys, so the keys always divide
  // whatever height remains — the previous fixed 62 px keys needed ~266 px in
  // a ~170 px slot and the ". 0 <backspace>" row was clipped off-screen.
  lv_obj_t *pad = cont(kpOverlay);
  lv_obj_set_size(pad, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(pad, 1);
  lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(pad, 6, 0);
  static const char *keys[12] = {"7", "8", "9", "4", "5", "6", "1", "2", "3", ".", "0", "del"};
  for (int r = 0; r < 4; r++) {
    lv_obj_t *rowc = cont(pad);
    lv_obj_set_width(rowc, LV_PCT(100));
    lv_obj_set_flex_grow(rowc, 1);
    lv_obj_set_flex_flow(rowc, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(rowc, 6, 0);
    for (int c = 0; c < 3; c++) {
      int i = r * 3 + c;
      lv_obj_t *k = flatBtn(rowc);
      lv_obj_set_flex_grow(k, 1);
      lv_obj_set_height(k, LV_PCT(100));
      styleCard(k, COL_CARD, COL_BORDER, 11, 0);
      const char *face = strcmp(keys[i], "del") == 0 ? LV_SYMBOL_BACKSPACE : keys[i];
      lv_obj_t *kl = lbl(k, face, COL_INK, F28); lv_obj_center(kl);
      lv_obj_add_event_cb(k, onKey, LV_EVENT_CLICKED, (void *)keys[i]);
    }
  }
  lv_obj_t *set = flatBtn(kpOverlay);
  lv_obj_set_size(set, LV_PCT(100), 50);
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
  if (m == MODE_RT || m == MODE_BATT) { curMode = m; }  // UI-only; no device mode
  else { curMode = m; if (A.setMode) A.setMode(m); UnitCfg c = unitCfg(modeUnit()); stepSize = c.step[c.defStep]; }
  showOverlay(OV_NONE);
  showScreen(SCR_MON);
}
static void buildPicker() {
  pickerOverlay = cont(lv_layer_top());
  lv_obj_add_flag(pickerOverlay, LV_OBJ_FLAG_CLICKABLE);  // modal barrier: catch stray taps
  lv_obj_set_size(pickerOverlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(pickerOverlay, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(pickerOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(pickerOverlay, 16, 0);  // keep the corner X clear of the rounded glass
  lv_obj_set_flex_flow(pickerOverlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(pickerOverlay, 10, 0);
  lv_obj_add_flag(pickerOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *hdr = cont(pickerOverlay);
  lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 8, 0);
  lv_obj_t *x = flatBtn(hdr); lv_obj_set_size(x, 40, 40);
  lv_obj_set_ext_click_area(x, 12);
  styleCard(x, COL_CARD, COL_BORDER, 10, 0);
  lv_obj_t *xl = lbl(x, LV_SYMBOL_CLOSE, COL_MUTED, F16); lv_obj_center(xl);
  lv_obj_add_event_cb(x, [](lv_event_t *) { showOverlay(OV_NONE); }, LV_EVENT_CLICKED, nullptr);
  lbl(hdr, "Select mode", COL_INK, F16);
  lv_obj_t *grid = cont(pickerOverlay);
  lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(grid, 1);
  lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(grid, LV_OBJ_FLAG_CLICKABLE);  // so background drags scroll
  lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 8, 0);
  static const char *MU[MODE_N] = {"A", "V", "ohm", "W", "A", "A", "ohm", "V"};
  for (int i = 0; i < MODE_N; i++) {
    lv_obj_t *t = flatBtn(grid);
    lv_obj_set_size(t, 164, 74);
    bool rt = (MODE_IDS[i] == MODE_RT || MODE_IDS[i] == MODE_BATT);
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
  for (int i = 0; i < MODE_N; i++) {
    bool sel = MODE_IDS[i] == curMode;
    bool rt = MODE_IDS[i] == MODE_RT || MODE_IDS[i] == MODE_BATT;
    lv_obj_set_style_border_color(modeTile[i], sel ? COL_ACCENT : (rt ? COL_AMBER : COL_BORDER), 0);
    lv_obj_set_style_border_width(modeTile[i], sel ? 2 : 1, 0);
  }
}

// ---- Live updates ----------------------------------------------------------
static void hhmmss(int t, char *out, int n) {
  int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
  if (h) snprintf(out, n, "%d:%02d:%02d", h, m, s); else snprintf(out, n, "%02d:%02d", m, s);
}

// Keep the RT / BATT setup groups on whichever screen is showing them: on the
// Monitor (below the heroes, reached by scrolling) while the matching pseudo-
// mode is active, else back home on their dedicated screens. The heroes drop
// from flex-grow to a compact fixed height while a setup group rides below.
static void syncMonitorExtras() {
  bool rtHere = isRT() && curScreen == SCR_MON;
  bool battHere = isBatt() && curScreen == SCR_MON;
  if (lv_obj_get_parent(rtSetupGroup) != (rtHere ? monScreen : rtIdleBox)) {
    lv_obj_set_parent(rtSetupGroup, rtHere ? monScreen : rtIdleBox);
    if (!rtHere) lv_obj_move_to_index(rtSetupGroup, 1);  // expl, [setup], start
  }
  if (lv_obj_get_parent(battSetupGroup) != (battHere ? monScreen : btIdleBox)) {
    lv_obj_set_parent(battSetupGroup, battHere ? monScreen : btIdleBox);
    if (!battHere) lv_obj_move_to_index(battSetupGroup, 1);
  }
  bool compact = rtHere || battHere;
  static int lastCompact = -1;
  if ((int)compact != lastCompact) {
    lastCompact = (int)compact;
    if (compact) {
      lv_obj_set_flex_grow(vHeroBlock, 0);
      lv_obj_set_flex_grow(iHeroBlock, 0);
      lv_obj_set_height(vHeroBlock, 84);
      lv_obj_set_height(iHeroBlock, 84);
    } else {
      lv_obj_set_flex_grow(vHeroBlock, 1);
      lv_obj_set_flex_grow(iHeroBlock, 1);
    }
    lv_obj_scroll_to_y(monScreen, 0, LV_ANIM_OFF);
  }
}

// Update the Monitor mode|set bar + load bar for the current mode/state.
static void refreshMonitor() {
  setTextIf(modeAbbrLbl, modeAbbr());
  setTextIf(modeNameLbl, modeName());
  char b[24];
  if (isRT()) {
    setTextIf(setLabelLbl, "FUSE");
    if (fuseRating) snprintf(b, sizeof(b), "%g", fuseRating); else strcpy(b, "--");
    setTextIf(setValLbl, b);
    setTextIf(setUnitLbl, "A");
  } else if (isBatt()) {
    setTextIf(setLabelLbl, "CUTOFF");
    snprintf(b, sizeof(b), "%.2f", battCutoff);
    setTextIf(setValLbl, b);
    setTextIf(setUnitLbl, "V");
  } else {
    setTextIf(setLabelLbl, "SET");
    UnitCfg c = unitCfg(modeUnit());
    fmtVal(b, sizeof(b), setpoint, c.dp); setTextIf(setValLbl, b);
    setTextIf(setUnitLbl, modeUnit());
  }
  // load / run-test button: restyling invalidates the whole 346x92 bar, so only
  // do it when the visual state actually flips (RT / BATT / ON / OFF).
  int barVis = isRT() ? 0 : isBatt() ? 3 : lastLoadOn ? 1 : 2;
  static int lastBarVis = -1;
  if (barVis == lastBarVis) return;
  lastBarVis = barVis;
  if (isBatt()) {
    lv_obj_set_style_bg_color(loadBtn, COL_ACCENT, 0); lv_obj_set_style_bg_opa(loadBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(loadBtn, 0, 0);
    lv_label_set_text(loadIcon, LV_SYMBOL_BATTERY_FULL); lv_obj_set_style_text_color(loadIcon, COL_DARKINK, 0);
    lv_label_set_text(loadTitle, "START DISCHARGE"); lv_obj_set_style_text_color(loadTitle, COL_DARKINK, 0);
    lv_label_set_text(loadSub, "Capacity test to the cutoff voltage"); lv_obj_set_style_text_color(loadSub, COL_DARKINK, 0);
  } else if (isRT()) {
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
  if (!isRT() && !isBatt() && curMode != s.mode) {
    curMode = s.mode;
    syncMonitorExtras();
  }
  char b[48];

  // telemetry bar: power - fan - temp - runtime (- Ah/mohm in CAP/DCR)
  snprintf(b, sizeof(b), "%.1f W", s.power); setTextIf(ibPower, b);
  int fanPct = (s.fanSpeed > el15::FAN_SPEED_MAX ? el15::FAN_SPEED_MAX : s.fanSpeed) * 100 / el15::FAN_SPEED_MAX;
  snprintf(b, sizeof(b), LV_SYMBOL_REFRESH " %d%%", fanPct); setTextIf(ibFan, b);
  snprintf(b, sizeof(b), "%.1f\xC2\xB0" "C", s.temperature);  // Montserrat does bundle the degree glyph
  setTextIf(ibTemp, b);
  int tb = s.temperature > 50 ? 2 : s.temperature > 42 ? 1 : 0;
  static int lastTb = -1;
  if (tb != lastTb) {
    lastTb = tb;
    lv_obj_set_style_text_color(ibTemp, tb == 2 ? COL_RED : tb == 1 ? COL_AMBER : COL_INK, 0);
  }
  char rt[16]; hhmmss(s.runtime, rt, sizeof(rt));
  snprintf(b, sizeof(b), LV_SYMBOL_LOOP " %s", rt); setTextIf(ibRuntime, b);
  bool hasExtra = (s.mode == el15::MODE_CAP || s.mode == el15::MODE_DCR);
  if (s.mode == el15::MODE_CAP) { snprintf(b, sizeof(b), "%.3f Ah", s.capacityAh); setTextIf(ibExtra, b); }
  else if (s.mode == el15::MODE_DCR) { snprintf(b, sizeof(b), "%.1f mohm", s.dcrMilliOhm); setTextIf(ibExtra, b); }
  if (hasExtra == lv_obj_has_flag(ibExtra, LV_OBJ_FLAG_HIDDEN)) {
    if (hasExtra) lv_obj_clear_flag(ibExtra, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ibExtra, LV_OBJ_FLAG_HIDDEN);
  }

  // hero blocks
  float shownI = (s.mode == el15::MODE_DCR) ? s.dcrI1 : s.current;
  snprintf(b, sizeof(b), "%.2f", s.voltage); setTextIf(vHeroVal, b);
  snprintf(b, sizeof(b), "%.3f", shownI); setTextIf(iHeroVal, b);
  snprintf(b, sizeof(b), "%.2f V", s.voltage); setTextIf(graphVNum, b);
  snprintf(b, sizeof(b), "%.3f A", shownI); setTextIf(graphINum, b);
  static int lastHeroOn = -1;
  if ((int)s.loadOn != lastHeroOn) {
    lastHeroOn = (int)s.loadOn;
    if (s.loadOn) {
      lv_obj_set_style_bg_color(iHeroBlock, COL_IHERO_BG_ON, 0);
      lv_obj_set_style_border_color(iHeroBlock, COL_IHERO_BD_ON, 0);
      lv_obj_set_style_text_color(iHeroVal, COL_RED, 0);
      lv_obj_set_style_text_color(iHeroUnit, COL_RED, 0);
      lv_obj_clear_flag(iHeroSink, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_set_style_bg_color(iHeroBlock, COL_IHERO_BG, 0);
      lv_obj_set_style_border_color(iHeroBlock, COL_IHERO_BD, 0);
      lv_obj_set_style_text_color(iHeroVal, COL_AMBER, 0);
      lv_obj_set_style_text_color(iHeroUnit, COL_AMBER, 0);
      lv_obj_add_flag(iHeroSink, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // setpoint sync (not while editing)
  if (curScreen != SCR_ADJ && curOverlay != OV_KEYPAD && !isRT() && !isBatt() && s.setpointInPacket)
    setpoint = s.setpoint;

  // graph history
  pushHistory(s.voltage, shownI);
  if (curScreen == SCR_GRAPH) refreshChart();

  // battery setup: live Voc + cell-count sanity/suggestion (on the Battery
  // screen, or on Monitor with the setup group scrolled in under BATT mode)
  if ((curScreen == SCR_BATT || (curScreen == SCR_MON && isBatt())) && btPhase == BT_IDLE) {
    char vb[64];
    if (BATT_CHEMS[battChem].maxCells) {
      const BattChem &c = BATT_CHEMS[battChem];
      float perCell = s.voltage / battCells;
      int sug = (int)(s.voltage / c.nom + 0.5f);
      if (sug < 1) sug = 1;
      if (sug > c.maxCells) sug = c.maxCells;
      snprintf(vb, sizeof(vb), "Voc %.2f V - %.2f V/cell (looks like %dS)", s.voltage, perCell, sug);
      setTextIf(btVocLbl, vb);
      bool odd = perCell < c.cut * 0.95f || perCell > c.full * 1.08f;
      static int lastOdd = -1;
      if ((int)odd != lastOdd) {
        lastOdd = (int)odd;
        lv_obj_set_style_text_color(btVocLbl, odd ? COL_AMBER : COL_FAINT, 0);
      }
    } else {
      snprintf(vb, sizeof(vb), "Voc %.2f V", s.voltage);
      setTextIf(btVocLbl, vb);
    }
  }

  // fault banner (flags toggled only on a visibility change)
  if (s.warning[0]) {
    faultIsEmergency = false;   // a real protection trip supersedes an e-stop ack
    snprintf(b, sizeof(b), LV_SYMBOL_WARNING "  %s - PROTECTION", s.warning); setTextIf(faultTitle, b);
    setTextIf(faultMsg, "Load protection tripped - check the setup");
    if (lv_obj_has_flag(faultBanner, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
      audio::fault();   // alarm only on the transition into a new fault
    }
  } else if (!faultIsEmergency && !lv_obj_has_flag(faultBanner, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  }

  // running r-test live values
  if (rtPhase == RT_RUN) {
    snprintf(b, sizeof(b), "%.2f V", s.voltage); setTextIf(runVLbl, b);
    snprintf(b, sizeof(b), "%.3f A", s.current); setTextIf(runILbl, b);
  }

  refreshMonitor();
}

void onEmergencyStop(bool wasTestRunning) {
  // Reuse the full-width fault banner as the acknowledgement; it stays up until
  // tapped (faultIsEmergency guards onStatus from auto-hiding it).
  faultIsEmergency = true;
  lv_label_set_text(faultTitle, LV_SYMBOL_WARNING "  EMERGENCY STOP");
  lv_label_set_text(faultMsg, wasTestRunning ? "Test aborted - load forced off. Tap to dismiss."
                                             : "Load forced off. Tap to dismiss.");
  lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(faultBanner);
  // A test that was aborted leaves its engine idle; reflect that in the UI so
  // the R-Test/Battery screens don't sit on a stale "running" view.
  if (rtPhase == RT_RUN) { rtPhase = RT_IDLE; refreshRtest(); }
  if (btPhase == BT_RUN || btPhase == BT_REST) { btPhase = BT_IDLE; refreshBatt(); }
}

void onConnState(int state, const char *info) {
  connected = (state == 3);
  const char *label = info ? info : "";
  // The strip chip gets a compact fixed word so it can never wrap or crowd the
  // stats cluster; the full detail string ("Not an EL15 (no FFF0)", ...) shows
  // on the Connect screen's status row.
  const char *chip = connected ? "Connected" : state == 2 ? "Connecting" : state == 1 ? "Scanning" : "Offline";
  lv_color_t dot = connected ? COL_GREEN : (state == 1 || state == 2) ? COL_AMBER : COL_MUTED;
  lv_label_set_text(stConnLabel, chip);
  lv_obj_set_style_bg_color(stDot, dot, 0);
  lv_label_set_text(connLabel2, label);
  lv_obj_set_style_bg_color(connDot2, dot, 0);
  if (connected) {
    lv_obj_clear_flag(connDisc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scanBtn, LV_OBJ_FLAG_HIDDEN);
    if (curScreen == SCR_CONNECT) showScreen(SCR_MON);
  } else {
    lv_obj_add_flag(connDisc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(scanBtn, LV_OBJ_FLAG_HIDDEN);
    histCount = 0;
    // A drop mid-sweep stops the engine without a callback; unstick the UI.
    if (rtPhase == RT_RUN) { rtPhase = RT_IDLE; refreshRtest(); }
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
  char v1[28];
  int n = (int)r.samples.size();
  if (n > RT_CHART_PTS) n = RT_CHART_PTS;  // UI caps steps at 20; belt & braces
  float iHi = n ? r.samples.back().current : 0;
  float vEnd = n ? r.samples.back().voltage : 0;
  float sag = r.openCircuitVoltage - vEnd;
  float pkW = 0;
  int fanMax = 0;
  for (auto &sm : r.samples) { pkW = LV_MAX(pkW, sm.voltage * sm.current); fanMax = LV_MAX(fanMax, sm.fanSpeed); }

  snprintf(v1, sizeof(v1), "%.2f V", r.openCircuitVoltage); lv_label_set_text(rrVal[RR_VOC], v1);
  // Uncertainty carries the reliable colour (green ok / amber high); R^2 is now
  // secondary and stays neutral.
  fmtOhm(v1, sizeof(v1), r.resistanceStdErr); lv_label_set_text(rrVal[RR_TOL], v1);
  lv_obj_set_style_text_color(rrVal[RR_TOL], r.reliable ? COL_GREEN : COL_AMBER, 0);
  snprintf(v1, sizeof(v1), "%.4f", r.rSquared); lv_label_set_text(rrVal[RR_R2], v1);
  lv_obj_set_style_text_color(rrVal[RR_R2], COL_INK, 0);
  if (r.resistanceOhm > 1e-4f) snprintf(v1, sizeof(v1), "%.1f A", r.openCircuitVoltage / r.resistanceOhm);
  else strcpy(v1, "--");
  lv_label_set_text(rrVal[RR_PSC], v1);
  lv_obj_set_style_text_color(rrVal[RR_PSC], COL_AMBER, 0);
  snprintf(v1, sizeof(v1), "%.2f V (%.1f%%)", sag,
           r.openCircuitVoltage > 0.01f ? sag * 100.0f / r.openCircuitVoltage : 0.0f);
  lv_label_set_text(rrVal[RR_SAG], v1);
  snprintf(v1, sizeof(v1), "%.1f W", pkW); lv_label_set_text(rrVal[RR_PKW], v1);
  if (n) snprintf(v1, sizeof(v1), "%.1f -> %.1f\xC2\xB0" "C", r.samples.front().temperature, r.samples.back().temperature);
  else strcpy(v1, "--");
  lv_label_set_text(rrVal[RR_TEMP], v1);
  int fanPct = (fanMax > el15::FAN_SPEED_MAX ? el15::FAN_SPEED_MAX : fanMax) * 100 / el15::FAN_SPEED_MAX;
  snprintf(v1, sizeof(v1), "%d%%", fanPct); lv_label_set_text(rrVal[RR_FAN], v1);
  // Sweep as measured (first/last step averages), not derived from the clamp.
  float swLo = n ? r.samples.front().current : 0;
  snprintf(v1, sizeof(v1), "%.2f-%.2f A", swLo, iHi); lv_label_set_text(rrVal[RR_SWEEP], v1);
  snprintf(v1, sizeof(v1), "%d", n); lv_label_set_text(rrVal[RR_STEPS], v1);
  snprintf(v1, sizeof(v1), "%.1f A", r.fuseRating); lv_label_set_text(rrVal[RR_FUSELIM], v1);

  // Component estimate vs measurement: a large positive residual usually means
  // a bad crimp / corroded contact somewhere in the loop.
  float wireR, connR, fuseR;
  float estR = estimateBuildR(wireR, connR, fuseR);
  bool hasEst = estR > 0.0001f;
  char k1[48];
  auto showRow = [](int i, bool show) {
    if (show) lv_obj_clear_flag(rrRow[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(rrRow[i], LV_OBJ_FLAG_HIDDEN);
  };
  showRow(RR_WIRE, wireR > 0);
  showRow(RR_CONN, connR > 0);
  showRow(RR_FUSEEST, fuseR > 0);
  showRow(RR_ESTTOT, hasEst);
  showRow(RR_RESID, hasEst);
  if (wireR > 0) {
    snprintf(k1, sizeof(k1), "Wire %gmm2 x %gm", estWireMm2, estWireLen);
    lv_label_set_text(rrKey[RR_WIRE], k1);
    fmtOhm(v1, sizeof(v1), wireR); lv_label_set_text(rrVal[RR_WIRE], v1);
  }
  if (connR > 0) {
    snprintf(k1, sizeof(k1), "Contacts (%d x 4 mohm)", estConns);
    lv_label_set_text(rrKey[RR_CONN], k1);
    fmtOhm(v1, sizeof(v1), connR); lv_label_set_text(rrVal[RR_CONN], v1);
  }
  if (fuseR > 0) {
    snprintf(k1, sizeof(k1), "Fuse (%s %gA)", FUSE_TYPE_NAMES[estFuseType], r.fuseRating);
    lv_label_set_text(rrKey[RR_FUSEEST], k1);
    fmtOhm(v1, sizeof(v1), fuseR); lv_label_set_text(rrVal[RR_FUSEEST], v1);
  }
  if (hasEst) {
    fmtOhm(v1, sizeof(v1), estR); lv_label_set_text(rrVal[RR_ESTTOT], v1);
    lv_obj_set_style_text_color(rrVal[RR_ESTTOT], COL_ACCENT2, 0);
    float resid = r.resistanceOhm - estR;
    char rv[30];
    fmtOhm(v1, sizeof(v1), resid);
    snprintf(rv, sizeof(rv), "%s%s", resid >= 0 ? "+" : "", v1);
    lv_label_set_text(rrVal[RR_RESID], rv);
    bool residOk = fabsf(resid) <= LV_MAX(0.02f, 0.25f * r.resistanceOhm);
    lv_obj_set_style_text_color(rrVal[RR_RESID], residOk ? COL_GREEN : COL_AMBER, 0);
  }

  // Fill the V-I chart (fixed RT_CHART_PTS capacity — no reallocation).
  // Values are centivolts / milliamps to stay in lv_coord_t range.
  float xHi = iHi > 0.01f ? iHi * 1.06f : 1.0f;
  float vLoPlot = r.openCircuitVoltage - r.resistanceOhm * iHi;
  for (auto &sm : r.samples) vLoPlot = LV_MIN(vLoPlot, sm.voltage);
  float vHiPlot = LV_MAX(r.openCircuitVoltage, n ? r.samples.front().voltage : 0.0f);
  if (vHiPlot - vLoPlot < 0.2f) { vLoPlot -= 0.1f; vHiPlot += 0.1f; }
  float vPad = (vHiPlot - vLoPlot) * 0.10f;
  lv_chart_set_range(rtChart, LV_CHART_AXIS_PRIMARY_Y,
                     (lv_coord_t)((vLoPlot - vPad) * 100), (lv_coord_t)((vHiPlot + vPad) * 100));
  // A line chart places points at fixed index positions, so the N level-points
  // are RESAMPLED across all RT_CHART_PTS slots to span the full width (the same
  // approach the battery curve uses); otherwise a few levels would fill only the
  // left of the chart and leave the rest empty. Measured V in amber; the
  // least-squares fit V at the interpolated current in green.
  for (int j = 0; j < RT_CHART_PTS; j++) {
    float vMeas, cur;
    if (n == 1) {
      vMeas = r.samples[0].voltage; cur = r.samples[0].current;
    } else {
      float sf = (float)j * (n - 1) / (RT_CHART_PTS - 1);
      int i0 = (int)sf;
      if (i0 >= n - 1) { vMeas = r.samples[n - 1].voltage; cur = r.samples[n - 1].current; }
      else {
        float f = sf - i0;
        vMeas = r.samples[i0].voltage * (1 - f) + r.samples[i0 + 1].voltage * f;
        cur   = r.samples[i0].current * (1 - f) + r.samples[i0 + 1].current * f;
      }
    }
    lv_chart_set_value_by_id(rtChart, rtSerMeas, j, (lv_coord_t)(vMeas * 100));
    lv_chart_set_value_by_id(rtChart, rtSerFit, j, (lv_coord_t)((r.openCircuitVoltage - r.resistanceOhm * cur) * 100));
  }
  lv_chart_refresh(rtChart);
  snprintf(v1, sizeof(v1), "%.2f-%.2f V", vLoPlot, vHiPlot); lv_label_set_text(rcYRange, v1);
  snprintf(v1, sizeof(v1), "0-%.2f A", xHi); lv_label_set_text(rcXRange, v1);
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

void onBattProgress(float v, float i, float ah, float wh, float temp, uint32_t elapsedS, int phase) {
  if (btPhase != BT_RUN && btPhase != BT_REST) { btPhase = BT_RUN; refreshBatt(); }
  if (phase == 2 && btPhase == BT_RUN) { btPhase = BT_REST; refreshBatt(); }
  setTextIf(btPhaseLbl, phase == 2 ? "RESTING - load off" : "DISCHARGING");
  char b[32];
  char el[16];
  hhmmss((int)elapsedS, el, sizeof(el));
  setTextIf(btElapsedLbl, el);
  snprintf(b, sizeof(b), "%.2f", v); setTextIf(btVLbl, b);
  snprintf(b, sizeof(b), "%.3f A", i); setTextIf(btILbl, b);
  snprintf(b, sizeof(b), "%.3f Ah", ah); setTextIf(btAhLbl, b);
  snprintf(b, sizeof(b), "%.1f Wh", wh); setTextIf(btWhLbl, b);
  snprintf(b, sizeof(b), "%.1f\xC2\xB0" "C", temp); setTextIf(btTempLbl, b);
  if (phase == 1) {
    btLastElapsed = elapsedS;
    battHistPush(v);
    if (curScreen == SCR_BATT) battChartRefresh();
  }
}

void onBattComplete(const CapacityTest::Result &r) {
  lastBatt = r;
  btPhase = BT_RESULT;
  battSaved = false;
  char b[48];
  snprintf(b, sizeof(b), "%.3f Ah", r.capacityAh); lv_label_set_text(btAhBig, b);
  snprintf(b, sizeof(b), "%.1f Wh  -  avg %.2f V", r.energyWh, r.avgV); lv_label_set_text(btWhSub, b);
  char el[16];
  hhmmss((int)r.durationS, el, sizeof(el));
  lv_label_set_text(brVal[0], el);
  lv_label_set_text(brVal[1], r.stopReason);
  snprintf(b, sizeof(b), "%.2f V", r.startV); lv_label_set_text(brVal[2], b);
  snprintf(b, sizeof(b), "%.2f V", r.endV); lv_label_set_text(brVal[3], b);
  snprintf(b, sizeof(b), "%.2f V", r.reboundV); lv_label_set_text(brVal[4], b);
  snprintf(b, sizeof(b), "%.2f V", r.avgV); lv_label_set_text(brVal[5], b);
  snprintf(b, sizeof(b), "%.3f A", r.avgI); lv_label_set_text(brVal[6], b);
  snprintf(b, sizeof(b), "%.1f - %.1f\xC2\xB0" "C", r.minTemp, r.maxTemp); lv_label_set_text(brVal[7], b);
  snprintf(b, sizeof(b), "%.2f V", r.cutoffV); lv_label_set_text(brVal[8], b);
  snprintf(b, sizeof(b), "%.2f A", r.currentA); lv_label_set_text(brVal[9], b);
  lv_label_set_text(btSaveLbl, LV_SYMBOL_SAVE "  Save to SD card");
  battChartRefresh();
  refreshBatt();
  showScreen(SCR_BATT);
}

void onBattError(const char *msg) {
  btPhase = BT_IDLE;
  refreshBatt();
  lv_label_set_text(btStatusLbl, msg);
  lv_obj_clear_flag(btStatusLbl, LV_OBJ_FLAG_HIDDEN);
  showScreen(SCR_BATT);
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
  buildSettings();
  buildBatt();
  buildLoadBar();
  buildMenu();
  buildKeypad();
  buildPicker();

  UnitCfg c = unitCfg("A");
  stepSize = c.step[c.defStep];
  lv_timer_create([](lv_timer_t *) { settingsTick(); }, 1000, nullptr);
  showScreen(SCR_MON);
  refreshRtest();
  refreshBatt();
}

}  // namespace ui
