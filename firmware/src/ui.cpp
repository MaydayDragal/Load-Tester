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
#include "battery_model.h"
#include "display.h"
#include "prefs.h"
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
enum Overlay { OV_NONE, OV_MENU, OV_KEYPAD, OV_PICKER, OV_TEXT, OV_WIFI };
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
// Sweep shape: startA -> maxA -> startA over sweepS seconds. maxA == 0 means
// "whatever the fuse safely allows", which the engine works out from the live Voc.
static float   rtStartA = 0, rtMaxA = 0;
static int     rtSweepS = 30;
static const uint32_t TARE_SWEEP_S = 8;   // shorted probes need no dwell time
// Probe wiring. 4-wire (Kelvin) senses voltage at the DUT through separate
// leads, so the lead and contact resistance carried by the force leads never
// appears in the reading; 2-wire measures it in series with the DUT, which is
// what the tare below subtracts (measure with the probes shorted, store R).
// Probe wiring, GLOBAL to every mode (Settings ▸ Probe wiring, and mirrored on
// the R-Test setup where the tare is captured). The EL15 senses voltage at its
// own terminals, so in a 2-wire hook-up every reading is short by the drop
// across the leads and contacts; with that resistance measured once, main.cpp
// puts it back on every status packet. In 4-wire the sense path carries no
// current, so there is nothing to add and nothing to subtract.
static bool    probeFourWire = false;
static float   probeTareOhm = 0;
static bool    rtTareRunning = false;   // this sweep is a lead-tare measurement
static RtPhase rtPhase = RT_IDLE;
static ResistanceTest::Result lastResult;
static bool    rtSaved = false;   // this result is already on the card

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
  // Nameplate capacity. Presets are the common cells/packs this gets used on:
  // an 18650, a 3 Ah pack, a 5 Ah pack, a 10 Ah pack.
  if (strcmp(u, "mAh") == 0)  return {0, 999000, 0, {100, 500, 1000}, 1, {2500, 3000, 5000, 10000}};
  // Sweep duration. Long enough to be steady, short enough not to cook the
  // wiring under test — 30 s is the default for a reason.
  if (strcmp(u, "s") == 0)    return {5, 900, 0, {1, 5, 30}, 1, {10, 30, 60, 120}};
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
// "R-TEST 4/8" / "BATT 01:23" chip: the way back to a test you navigated away from.
static lv_obj_t *stTestChip, *stTestChipLbl;
static lv_obj_t *infoBar, *ibPower, *ibFan, *ibTemp, *ibRuntime, *ibExtra;
static lv_obj_t *faultBanner, *faultTitle, *faultMsg;
// The fault banner doubles as the emergency-stop acknowledgement. When shown
// for an e-stop it must stay up (a clean status packet would otherwise hide it
// on the next poll) until the user taps it or a real protection trip supersedes.
static bool faultIsEmergency = false;
// Set while the link-loss supervisor is actively trying to shut a load down:
// the banner then refuses taps and cannot be replaced, because it is the only
// thing telling the user that current may still be flowing.
static bool faultLocked = false;
// When set, tapping the banner runs this instead of dismissing it (the
// "reconnect and force LOAD OFF" offer after a crash).
static std::function<void()> guardAction;

static lv_obj_t *modeAbbrLbl, *modeNameLbl, *setLabelLbl, *setValLbl, *setUnitLbl;
static lv_obj_t *vHeroBlock, *vHeroCap, *vHeroVal, *iHeroBlock, *iHeroLabelRow, *iHeroSink, *iHeroVal, *iHeroUnit;
static lv_obj_t *rtSetupGroup, *battSetupGroup;  // reparented onto Monitor in RT/BATT mode
static lv_obj_t *loadBar, *loadBtn, *loadIcon, *loadTitle, *loadSub;

static lv_obj_t *adjCaption, *adjVal, *adjUnit, *stepChip[3], *stepChipLbl[3];
static lv_obj_t *graphVNum, *graphINum, *chart, *gVRange, *gIRange, *gWin;
static lv_chart_series_t *serV, *serI;

static lv_obj_t *rtIdleBox, *rtRunBox, *rtResultBox;
static lv_obj_t *fuseVal, *startBtn, *startBtnLbl;
static lv_obj_t *rtStartVal, *rtMaxVal, *rtSweepVal, *rtSweepHint;
static lv_obj_t *estWireVal, *estLenVal, *estConnVal, *estTypeVal, *estTotalVal;
static lv_obj_t *runStepLbl, *runBar, *runVLbl, *runILbl, *runRLbl, *runTargetLbl, *rtRunClock;
// Live sweep charts, laid out like the capacity test's: one time-series for the
// measured V and I, a second for the running resistance estimate.
static lv_obj_t *rtLiveCard, *rtLiveChart, *rtLiveYLbl, *rtLiveXLbl;
static lv_obj_t *rtRCard, *rtRChart, *rtRYLbl;
static lv_chart_series_t *rtLiveSerV, *rtLiveSerI, *rtRSer;
// Reservoirs for the live curves. Same bounded scheme as the battery graph:
// dense capture, halved when full so the whole sweep always fits.
static const int RT_RES_N = 240;    // raw reservoir per series
static const int RT_LIVE_PTS = 120; // points actually drawn
static float rtResV[RT_RES_N], rtResI[RT_RES_N], rtResR[RT_RES_N];
static int rtResN = 0, rtResStride = 1, rtResAcc = 0;
static float rtLastElapsed = 0, rtTotalS = 0;
static bool rtHaveR = false;
static lv_obj_t *resistVal, *lowConfBox, *resultList, *saveBtn, *saveBtnLbl, *rtStatusLbl;
static lv_obj_t *rtProbeVal, *rtProbeHint, *rtTareCard, *rtTareVal;
static lv_obj_t *rtChart, *rcXRange, *rcYRange;
static lv_chart_series_t *rtSerMeas, *rtSerFit;
// Result rows are built ONCE and text-updated per test. Rebuilding them per
// completion (lv_obj_clean + ~45 allocations) interleaved frees with the chart
// buffer reallocs and corrupted the heap -> Load access fault in the next
// layout pass (see the 2026-07-21 panic capture).
static const int RR_N = 18;
enum { RR_VOC, RR_PROBE, RR_RAW, RR_TOL, RR_R2, RR_PSC, RR_SAG, RR_PKW, RR_TEMP,
       RR_FAN, RR_SWEEP, RR_STEPS, RR_FUSELIM,
       RR_WIRE, RR_CONN, RR_FUSEEST, RR_ESTTOT, RR_RESID };
static lv_obj_t *rrRow[RR_N], *rrKey[RR_N], *rrVal[RR_N];
// Result V-I chart capacity = the engine's current-band count; fixed, no reallocs.
static const int RT_CHART_PTS = 32;
static lv_obj_t *setBriVal, *setBattVal, *setBattState, *setRtcVal, *setSdVal, *setHeapVal, *setMinHeapVal, *setUptimeVal;
static lv_obj_t *setPxShiftBtn, *setPxShiftLbl, *dimChip[6], *dimChipLbl[6], *setDimSummary;
static lv_obj_t *setAutoConnBtn, *setAutoConnLbl;
static lv_obj_t *setSsidVal, *setPassVal, *setTzVal, *setSyncBtn, *setSyncLbl, *setNetStatus;
static lv_obj_t *setProbeBtn, *setProbeLbl, *setTareRow, *setProbeNote;
// Text-entry overlay (password + manual/hidden SSID entry).
static lv_obj_t *kbOverlay, *kbTextArea, *kbTitle;
enum { KB_SSID = 1, KB_PASS = 2 };
static int kbTarget = KB_SSID;
// Wi-Fi network picker overlay + its scanned SSID list (ui-owned copies, so a
// row's event handler can reference one after the scan buffer is reused).
static lv_obj_t *wifiOverlay, *wifiList, *wifiStatus;
static const int WIFI_MAX = 24;
static char wifiNames[WIFI_MAX][33];
static int wifiCount = 0;
static lv_obj_t *setVolVal, *setMuteBtn, *setMuteLbl;
// ---- Battery capacity test state ---------------------------------------------
// Chemistry data (per-cell voltages, discharge curve, standard test C-rates)
// lives in battery_model.h so the engine and the UI cannot drift apart: the
// engine reads the same curve to estimate time remaining that this screen reads
// to suggest a cell count and a discharge current.
using BattChem = battmodel::Chem;
static const BattChem *const BATT_CHEMS = battmodel::CHEMS;
static const int BATT_CHEM_N = battmodel::CHEM_N;
static int battChem = 0, battCells = 3;
static float battCutoff = 9.0f;          // = cells x per-cell cutoff, or custom
static bool battCutoffCustom = false;
static float battAmps = 1.0f;
enum BattPhase { BT_IDLE, BT_RUN, BT_REST, BT_RESULT };
static BattPhase btPhase = BT_IDLE;
static CapacityTest::Result lastBatt;
// Nameplate capacity in Ah (0 = not entered). Drives the C-rate chips, the
// state-of-health result row, and the fallback time estimate for chemistries
// with no discharge curve.
static float battRatedAh = 0;
// Selected test C-rate as an index into the chemistry's cRate[] presets, or -1
// when the user typed a current by hand. While a rate is selected the current
// FOLLOWS the pack size — enter 3000 mAh at 0.2C and the controller works out
// 0.60 A — which is the way a datasheet specifies a capacity test. Typing a
// current directly drops back to -1 and the chips go quiet.
static int battCRateIdx = -1;
// Mirrors the engine's paused state so the chrome and the run card can show it
// without asking the engine on every redraw.
static bool battPausedFlag = false;
static char battPauseWhy[64] = "";
// Latest "Step n/total" text, for the running-test chip.
static char rtStepText[12] = "";

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
static lv_obj_t *btCellsVal, *btVocLbl, *btCutoffVal, *btAmpsVal, *btStartBtn, *btStartLbl, *btStatusLbl;
static lv_obj_t *btRatedVal, *btRateHint;
static lv_obj_t *btCRateChip[battmodel::CRATE_N], *btCRateChipLbl[battmodel::CRATE_N];
// The chemistry picker is ONE lv_btnmatrix, not ten buttons with ten labels.
// That is a heap decision, not a style one: this board has no PSRAM, LVGL
// allocates from the system heap, and NimBLE needs a ~30 KB contiguous block to
// establish a connection — ten chip widgets cost ~6 KB of it and pushed the
// largest free block to 31.7 KB, i.e. into the margin. A button matrix renders
// the same grid as a single object. It is also safe with the touch-snap engine:
// snapOffset() clamps a near-miss INTO the target rect rather than moving it to
// the centre, so a tap inside the matrix is never displaced onto a neighbour.
static lv_obj_t *btChemMx;
static lv_obj_t *btChemDetail, *btCellsRow;
static lv_obj_t *btPhaseLbl, *btElapsedLbl, *btVLbl, *btCutSub, *btILbl, *btAhLbl, *btWhLbl, *btTempLbl;
static lv_obj_t *btEtaLbl, *btPauseCard, *btPauseWhyLbl, *btResumeBtn;
static lv_obj_t *btChart, *btChartYLbl, *btChartXLbl;
static lv_chart_series_t *btSer;
static lv_obj_t *btAhBig, *btWhSub, *btSaveBtn, *btSaveLbl;
// Result rows. BR_RATED..BR_CRATE only appear when a rated capacity was entered;
// BR_IR..BR_IMPLIED only when the battery model established itself during the run.
static const int BR_N = 17;
enum { BR_DUR, BR_REASON, BR_STARTV, BR_ENDV, BR_REBOUND, BR_AVGV, BR_AVGI,
       BR_TEMP, BR_CUTOFF, BR_CURRENT, BR_PAUSED, BR_RATED, BR_SOH, BR_CRATE,
       BR_IR, BR_SOCSPAN, BR_IMPLIED };
static lv_obj_t *brVal[BR_N], *brRow[BR_N];

static int pollMs = 50;  // status sampling interval, mirrored to BLE + R-test
// 50 ms (20 Hz) is the device's practical max: a poll-rate sweep showed the EL15
// produces ~17-19 fresh samples/s (min ~23 ms between distinct frames), so 20 Hz
// captures ~all of it at ~88% unique. 10 Hz is 100% unique; faster than 20 Hz
// just re-fetches repeats and floods the BLE link. 1 Hz was dropped as useless.
static const int RATE_MS[4] = {50, 100, 250, 500};
static const char *RATE_NAMES[4] = {"20 Hz", "10 Hz", "4 Hz", "2 Hz"};
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
static void refreshTestChip();
static void showActiveTest();
static void refreshBatt();
static void battChartRefresh();
static void enterBattRun();
static void syncMonitorExtras();
static void refreshWifi();
static void openTextEntry(int target);
static void startWifiScan();
static void refreshProbe();
static void persistProbe();
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

// ---- SD card saves ---------------------------------------------------------
// A save blocks the loop task for the whole write AND the read-back that
// verifies it — for a full capacity report that is ~20 s, not ~2 s, because
// sd_card.cpp now proves the card actually kept the bytes rather than trusting it
// to say so. So the button has to be repainted to its in-progress state and
// flushed to the panel before the call is made, and it has to say that verifying
// is part of the job — otherwise a correct save looks like a frozen UI.
static void armSaveButton(lv_obj_t *btn, lv_obj_t *lblObj) {
  lv_label_set_text(lblObj, LV_SYMBOL_SAVE "  Writing + verifying...");
  lv_obj_set_style_bg_color(btn, COL_AMBER, 0);
  lv_refr_now(nullptr);
}

// Report the outcome verbatim: the file name when the report really is on the
// card, the failure reason when it is not. A failed save leaves the button
// armed so the user can swap the card and tap again.
static void showSaveOutcome(lv_obj_t *btn, lv_obj_t *lblObj, bool ok, const char *msg) {
  char b[64];
  snprintf(b, sizeof(b), ok ? LV_SYMBOL_OK "  Saved %s" : LV_SYMBOL_WARNING "  %s", msg);
  lv_label_set_text(lblObj, b);
  lv_obj_set_style_bg_color(btn, ok ? COL_GREEN : COL_RED, 0);
}

// Back to the idle "Save to SD card" look — called when a fresh result lands.
static void resetSaveButton(lv_obj_t *btn, lv_obj_t *lblObj) {
  lv_label_set_text(lblObj, LV_SYMBOL_SAVE "  Save to SD card");
  lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
}

// Write the report the moment a test finishes, without waiting to be asked. A
// completed run is data you do not want to lose to a stray tap on "New test",
// and a long unattended discharge may finish with nobody watching.
//
// This runs from the completion callback, i.e. inside lv_timer_handler, so it
// paints the in-progress state and forces a flush first (armSaveButton). On
// failure the button is left showing why and stays tappable, so the user can
// insert a card and retry — the result and its flash datapoint log both survive
// until the next test starts.
static void autoSave(lv_obj_t *btn, lv_obj_t *lblObj, bool &savedFlag,
                     const std::function<bool(char *, size_t)> &fn) {
  if (!fn) return;
  armSaveButton(btn, lblObj);
  char msg[48] = "";
  savedFlag = fn(msg, sizeof(msg));
  showSaveOutcome(btn, lblObj, savedFlag, msg);
  Serial.printf("[save] auto-save %s: %s\n", savedFlag ? "OK" : "FAILED", msg);
}

// ---- Navigation ------------------------------------------------------------
static void showOverlay(Overlay o) {
  curOverlay = o;
  lv_obj_t *ovs[5] = {menuOverlay, kpOverlay, pickerOverlay, kbOverlay, wifiOverlay};
  Overlay ids[5] = {OV_MENU, OV_KEYPAD, OV_PICKER, OV_TEXT, OV_WIFI};
  for (int i = 0; i < 5; i++) {
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
  // Landing on a test screen must always show the phase the engine is ACTUALLY
  // in. Without this, navigating away mid-test and back left whichever box was
  // last unhidden on screen — so a running test could look idle and its result
  // could look unreachable.
  if (s == SCR_RTEST) refreshRtest();
  if (s == SCR_BATT) { refreshBatt(); battChartRefresh(); }
  refreshTestChip();
}

// Jump to whichever test is live (or showing an unsaved result), so the
// running-test chip and the load bar always have somewhere sensible to go.
static void showActiveTest() {
  if (btPhase != BT_IDLE) showScreen(SCR_BATT);
  else if (rtPhase != RT_IDLE) showScreen(SCR_RTEST);
}

// Show/label the running-test chip. Called from every place a test phase can
// change, plus the 1 Hz tick so the battery elapsed time stays live.
static void refreshTestChip() {
  if (!stTestChip) return;
  const bool show = btPhase != BT_IDLE || rtPhase != RT_IDLE;
  // Hidden on the test's own screen: you are already there, and the chip would
  // just crowd the strip.
  const bool here = (btPhase != BT_IDLE && curScreen == SCR_BATT) ||
                    (rtPhase != RT_IDLE && curScreen == SCR_RTEST);
  if (!show || here) { lv_obj_add_flag(stTestChip, LV_OBJ_FLAG_HIDDEN); return; }
  char b[24];
  lv_color_t col = COL_ACCENT2, border = COL_ACCENT;
  if (btPhase == BT_RUN || btPhase == BT_REST) {
    if (battPausedFlag) { snprintf(b, sizeof(b), "BATT PAUSED"); col = COL_AMBER; border = COL_AMBER; }
    else { char el[16]; hhmmss((int)btLastElapsed, el, sizeof(el)); snprintf(b, sizeof(b), "BATT %s", el); }
  } else if (btPhase == BT_RESULT) {
    snprintf(b, sizeof(b), "BATT result");
    col = COL_GREEN; border = COL_GREEN;
  } else if (rtPhase == RT_RUN) {
    snprintf(b, sizeof(b), "R-TEST %s", rtStepText);
  } else {
    snprintf(b, sizeof(b), "R-TEST result");
    col = COL_GREEN; border = COL_GREEN;
  }
  setTextIf(stTestChipLbl, b);   // called at the poll rate; don't churn the label
  lv_obj_set_style_text_color(stTestChipLbl, col, 0);
  lv_obj_set_style_border_color(stTestChip, border, 0);
  lv_obj_clear_flag(stTestChip, LV_OBJ_FLAG_HIDDEN);
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

  // Running-test chip. Visible on EVERY screen whenever an engine owns the load
  // or is holding an unsaved result, and tapping it goes straight back to that
  // test. Without it, walking away from a running test (the Menu button is right
  // next to it) left no obvious way back and the test looked lost.
  stTestChip = flatBtn(strip);
  lv_obj_set_size(stTestChip, LV_SIZE_CONTENT, 30);
  lv_obj_set_ext_click_area(stTestChip, 10);
  styleCard(stTestChip, lv_color_hex(0x2a2140), COL_ACCENT, 9, 0);
  lv_obj_set_style_pad_hor(stTestChip, 9, 0);
  stTestChipLbl = lbl(stTestChip, "TEST", COL_ACCENT2, F14);
  lv_obj_center(stTestChipLbl);
  lv_obj_add_event_cb(stTestChip, [](lv_event_t *) { showActiveTest(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(stTestChip, LV_OBJ_FLAG_HIDDEN);

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
  // On lv_layer_top(), NOT scrRoot: the full-screen overlays (menu, keypad,
  // picker, text entry, Wi-Fi) are opaque top-layer children, and the top layer
  // always renders above the active screen — a banner parented to scrRoot is
  // invisible while any overlay is open, which is exactly when a "LOAD MAY
  // STILL BE ON" warning must not disappear. Overlays are built AFTER this, so
  // every show path also calls lv_obj_move_foreground(faultBanner) to end up
  // above them within the layer.
  faultBanner = cont(lv_layer_top());
  lv_obj_set_size(faultBanner, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_align(faultBanner, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(faultBanner, COL_RED, 0);
  lv_obj_set_style_bg_opa(faultBanner, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(faultBanner, 9, 0);
  lv_obj_set_flex_flow(faultBanner, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(faultBanner, [](lv_event_t *) {
    // A banner with an armed action (crash recovery) runs it instead of
    // dismissing; a locked one (the supervisor mid-recovery) ignores taps
    // entirely, because the load is still energised and the message is the
    // only thing telling the user so.
    if (guardAction) { std::function<void()> a = guardAction; guardAction = nullptr; a(); return; }
    if (faultLocked) return;
    faultIsEmergency = false;
    lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, nullptr);
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
  // The caption doubles as the probe-wiring indicator: a lead-corrected reading
  // deliberately DIFFERS from what the EL15's own panel shows, so the screen has
  // to say why rather than leave the user to find the discrepancy. Reusing this
  // label costs no extra widget, which on this heap matters (HANDOVER §7).
  vHeroCap = lbl(vh, "VOLTAGE", COL_GREEN, F12);
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
      if (A.startRTest) { rtTareRunning = false; A.startRTest(fuseRating, rtStartA, rtMaxA, (uint32_t)rtSweepS, probeFourWire, probeTareOhm); enterRtRun(); }
      return;
    }
    if (isBatt()) {
      if (battCutoff <= 0.05f || battAmps <= 0.005f) return;
      if (!connected) { showScreen(SCR_CONNECT); return; }
      if (A.startBatt) { A.startBatt(battCutoff, battAmps, battRatedAh, battChem, battCells); enterBattRun(); }
      return;
    }
    // The warning gate blocks only turning the load ON: with a protection
    // warning active while current still flows (thermal pre-trip, latched
    // fault), the primary on-screen control must NEVER refuse to send
    // LOAD OFF — that path stays open unconditionally.
    if (!lastLoadOn && lastStatus.warning[0]) return;
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
  lv_obj_t *expl = lbl(rtIdleBox, "Ramps current smoothly up and back down, fitting a line through every reading to measure series resistance.", COL_MUTED, F12);
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
  // Sweep shape. The ramp is continuous, so what defines it is where it starts,
  // where it turns around, and how long it takes — not a step count.
  lv_obj_t *sweepCard = cont(rtSetupGroup);
  lv_obj_set_size(sweepCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(sweepCard, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(sweepCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sweepCard, 2, 0);
  lbl(sweepCard, "SWEEP", COL_MUTED, F12);
  auto swRow = [](lv_obj_t *parent, const char *k, lv_obj_t **valOut, lv_event_cb_t cb) {
    lv_obj_t *row = flatBtn(parent);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lbl(row, k, COL_MUTED, F14);
    *valOut = lbl(row, "--", COL_ACCENT2, F16);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
  };
  swRow(sweepCard, "Start current - tap to type", &rtStartVal, [](lv_event_t *) { openKeypad(7); });
  swRow(sweepCard, "Max current - tap to type", &rtMaxVal, [](lv_event_t *) { openKeypad(8); });
  swRow(sweepCard, "Duration - tap to type", &rtSweepVal, [](lv_event_t *) { openKeypad(9); });
  rtSweepHint = lbl(sweepCard, "", COL_FAINT, F12);
  lv_label_set_long_mode(rtSweepHint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(rtSweepHint, LV_PCT(100));

  // Probe wiring. At the milliohm scale the leads are usually a bigger
  // resistance than the thing being measured, so how they are connected is not
  // a detail: 4-wire removes them physically, 2-wire removes them arithmetically
  // (the tare below), and nothing removes them if you do neither.
  lv_obj_t *probeCard = flatBtn(rtSetupGroup);
  lv_obj_set_size(probeCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(probeCard, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(probeCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(probeCard, 5, 0);
  lbl(probeCard, "PROBE WIRING - tap to switch", COL_MUTED, F12);
  rtProbeVal = lbl(probeCard, "2-wire", COL_INK, F24);
  rtProbeHint = lbl(probeCard, "", COL_FAINT, F12);
  lv_label_set_long_mode(rtProbeHint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(rtProbeHint, LV_PCT(100));
  lv_obj_add_event_cb(probeCard, [](lv_event_t *) {
    // Same two values Settings ▸ Probe wiring edits — this card is the place the
    // tare is MEASURED, not a second copy of the setting.
    probeFourWire = !probeFourWire;
    persistProbe();
    refreshProbe();
    refreshRtest();
    refreshMonitor();
  }, LV_EVENT_CLICKED, nullptr);

  // Lead tare (2-wire only): one shorted-probe sweep, stored and subtracted
  // from every later result.
  rtTareCard = cont(rtSetupGroup);
  lv_obj_set_size(rtTareCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(rtTareCard, COL_CARD, COL_BORDER, 12, 12);
  lv_obj_set_flex_flow(rtTareCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rtTareCard, 8, 0);
  lv_obj_t *tRow = cont(rtTareCard);
  lv_obj_set_size(tRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lbl(tRow, "LEAD TARE", COL_MUTED, F12);
  rtTareVal = lbl(tRow, "not set", COL_FAINT, F16);
  lv_obj_t *tBtnRow = cont(rtTareCard);
  lv_obj_set_size(tBtnRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tBtnRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tBtnRow, 7, 0);
  lv_obj_t *tMeas = flatBtn(tBtnRow);
  lv_obj_set_flex_grow(tMeas, 3);
  lv_obj_set_height(tMeas, 46);
  styleCard(tMeas, COL_INSET, COL_BORDER, 11, 0);
  lv_obj_t *tMeasLbl = lbl(tMeas, "Measure (short the probes)", COL_ACCENT2, F14);
  lv_obj_center(tMeasLbl);
  lv_obj_add_event_cb(tMeas, [](lv_event_t *) {
    if (!fuseRating) {   // the sweep's safe ceiling is derived from it
      lv_label_set_text(rtStatusLbl, "Set the fuse rating first - the tare sweep uses it.");
      lv_obj_clear_flag(rtStatusLbl, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    if (!connected || engineBusy() || !A.startRTest) return;
    rtTareRunning = true;
    enterRtRun();
    // A tare is a 2-wire measurement of the leads themselves, with no tare of
    // its own to subtract. Shorted probes settle instantly, so it uses a short
    // sweep regardless of what the user picked for real runs.
    A.startRTest(fuseRating, rtStartA, rtMaxA, TARE_SWEEP_S, false, 0);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *tClear = flatBtn(tBtnRow);
  lv_obj_set_flex_grow(tClear, 1);
  lv_obj_set_height(tClear, 46);
  styleCard(tClear, COL_INSET, COL_BORDER, 11, 0);
  lv_obj_t *tClearLbl = lbl(tClear, "Clear", COL_MUTED, F14);
  lv_obj_center(tClearLbl);
  lv_obj_add_event_cb(tClear, [](lv_event_t *) {
    probeTareOhm = 0;
    persistProbe();
    refreshProbe();
    refreshRtest();
    refreshMonitor();
  }, LV_EVENT_CLICKED, nullptr);

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
    if (A.startRTest) { rtTareRunning = false; A.startRTest(fuseRating, rtStartA, rtMaxA, (uint32_t)rtSweepS, probeFourWire, probeTareOhm); enterRtRun(); }
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
  runStepLbl = lbl(rr, "PRIMING", COL_ACCENT, F14);
  rtRunClock = lbl(rr, "0.0 / 30 s", COL_INK, F16);
  runBar = lv_bar_create(runCard);
  lv_obj_set_size(runBar, LV_PCT(100), 12);
  lv_obj_set_style_bg_color(runBar, lv_color_hex(0x161d26), LV_PART_MAIN);
  lv_obj_set_style_bg_color(runBar, COL_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_radius(runBar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(runBar, 6, LV_PART_INDICATOR);
  lv_bar_set_range(runBar, 0, 100);
  // Hero readouts, laid out like the capacity test's: the two measured values
  // big, the derived one under them.
  lv_obj_t *rv = cont(runCard); lv_obj_set_size(rv, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rv, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(rv, 20, 0);
  runVLbl = lbl(rv, "--", COL_GREEN, F34);
  runILbl = lbl(rv, "-- A", COL_AMBER, F34);
  lv_obj_t *rr2 = cont(runCard); lv_obj_set_size(rr2, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rr2, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rr2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  runRLbl = lbl(rr2, "R --", COL_ACCENT2, F24);
  runTargetLbl = lbl(rr2, "target -- A", COL_MUTED, F12);

  // ---- live V + I vs time ----
  rtLiveCard = cont(rtRunBox);
  lv_obj_set_size(rtLiveCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(rtLiveCard, COL_READOUT, COL_BORDER2, 12, 8);
  lv_obj_set_flex_flow(rtLiveCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rtLiveCard, 4, 0);
  lbl(rtLiveCard, "VOLTAGE + CURRENT vs time", COL_MUTED, F12);
  rtLiveChart = lv_chart_create(rtLiveCard);
  lv_obj_set_width(rtLiveChart, LV_PCT(100));
  lv_obj_set_height(rtLiveChart, 110);
  lv_obj_set_style_bg_opa(rtLiveChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rtLiveChart, 0, 0);
  lv_obj_set_style_pad_all(rtLiveChart, 2, 0);
  lv_obj_set_style_line_color(rtLiveChart, COL_BORDER2, LV_PART_MAIN);
  lv_obj_set_style_width(rtLiveChart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(rtLiveChart, 0, LV_PART_INDICATOR);
  lv_chart_set_type(rtLiveChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(rtLiveChart, RT_LIVE_PTS);   // fixed capacity, no reallocs
  lv_chart_set_div_line_count(rtLiveChart, 3, 4);
  rtLiveSerV = lv_chart_add_series(rtLiveChart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
  rtLiveSerI = lv_chart_add_series(rtLiveChart, COL_AMBER, LV_CHART_AXIS_SECONDARY_Y);
  lv_obj_t *lrng = cont(rtLiveCard);
  lv_obj_set_size(lrng, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(lrng, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(lrng, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  rtLiveYLbl = lbl(lrng, "", COL_GREEN, F12);
  rtLiveXLbl = lbl(lrng, "", COL_AMBER, F12);

  // ---- live resistance vs time ----
  rtRCard = cont(rtRunBox);
  lv_obj_set_size(rtRCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(rtRCard, COL_READOUT, COL_BORDER2, 12, 8);
  lv_obj_set_flex_flow(rtRCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rtRCard, 4, 0);
  lbl(rtRCard, "RESISTANCE estimate vs time", COL_MUTED, F12);
  rtRChart = lv_chart_create(rtRCard);
  lv_obj_set_width(rtRChart, LV_PCT(100));
  lv_obj_set_height(rtRChart, 90);
  lv_obj_set_style_bg_opa(rtRChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rtRChart, 0, 0);
  lv_obj_set_style_pad_all(rtRChart, 2, 0);
  lv_obj_set_style_line_color(rtRChart, COL_BORDER2, LV_PART_MAIN);
  lv_obj_set_style_width(rtRChart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(rtRChart, 0, LV_PART_INDICATOR);
  lv_chart_set_type(rtRChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(rtRChart, RT_LIVE_PTS);
  lv_chart_set_div_line_count(rtRChart, 3, 4);
  rtRSer = lv_chart_add_series(rtRChart, COL_ACCENT2, LV_CHART_AXIS_PRIMARY_Y);
  rtRYLbl = lbl(rtRCard, "", COL_ACCENT2, F12);
  lv_obj_add_flag(rtRCard, LV_OBJ_FLAG_HIDDEN);

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
      "Open-circuit voltage", "Probe wiring", "Measured (incl. leads)",
      "Uncertainty (+/-)", "Fit quality (R2)", "Est. short-circuit I",
      "Sag at max current", "Peak test power", "Load temp", "Max fan",
      "Current sweep", "Samples / bands", "Fuse limit",
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
    if (rtSaved || !A.saveRTest) return;   // one file per result; retry on failure
    armSaveButton(saveBtn, saveBtnLbl);
    char msg[48] = "";
    rtSaved = A.saveRTest(msg, sizeof(msg));
    showSaveOutcome(saveBtn, saveBtnLbl, rtSaved, msg);
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

// Mirror the on-screen test setup into NVS so the next boot starts where this
// one left off. Called from the two refresh functions rather than from each of
// the dozen mutation sites, so a new control can't forget to persist itself.
// Cheap: NVS skips writes whose value is unchanged, and commits are debounced.
static void persistSetup() {
  prefs::change([](prefs::Data &d) {
    d.fuseRating = fuseRating;
    d.rtStartA = rtStartA;
    d.rtMaxA = rtMaxA;
    d.rtSweepS = (uint16_t)rtSweepS;
    d.fourWire = probeFourWire;
    d.tareOhm = probeTareOhm;
    d.battChem = (uint8_t)battChem;
    d.battCells = (uint8_t)battCells;
    d.battCutoff = battCutoff;
    d.battCutoffCustom = battCutoffCustom;
    d.battAmps = battAmps;
    d.battRatedMah = battRatedAh * 1000.0f;
    d.battCRateIdx = (int8_t)battCRateIdx;
  });
}

static void refreshRtest() {
  persistSetup();
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
    // Sweep shape. The safe ceiling depends on the source voltage, which is only
    // known once connected — show the live figure when we have it so the user
    // can see what their fuse actually permits before committing.
    snprintf(b, sizeof(b), "%.2f A", rtStartA); lv_label_set_text(rtStartVal, b);
    float voc = lastStatus.valid ? lastStatus.voltage : 0;
    float cap = fuseRating > 0 ? ResistanceTest::safeMaxCurrent(fuseRating, voc) : 0;
    if (rtMaxA > 0) snprintf(b, sizeof(b), "%.2f A", rtMaxA);
    else if (cap > 0) snprintf(b, sizeof(b), "auto (%.2f A)", cap);
    else strcpy(b, "auto");
    lv_label_set_text(rtMaxVal, b);
    lv_obj_set_style_text_color(rtMaxVal, (rtMaxA > 0 && cap > 0 && rtMaxA > cap + 1e-3f) ? COL_AMBER : COL_ACCENT2, 0);
    snprintf(b, sizeof(b), "%d s", rtSweepS); lv_label_set_text(rtSweepVal, b);
    {
      char hint[224];
      float peak = rtMaxA > 0 ? rtMaxA : cap;
      if (cap > 0 && rtMaxA > cap + 1e-3f) {
        snprintf(hint, sizeof(hint),
                 "Capped at %.2f A: %g A fuse x 80%%, the EL15's 12 A / 150 W, and the "
                 "source voltage. The sweep will use the cap, not %.2f A.", cap, fuseRating, rtMaxA);
      } else if (peak > 0) {
        // The ramp re-commands at a fixed 10 Hz and no longer costs a poll, so
        // the sample count follows the Settings sample rate directly.
        snprintf(hint, sizeof(hint),
                 "%.2f A up and back down over %d s, about %d readings. "
                 "Longer is steadier; shorter heats the wiring less.",
                 peak, rtSweepS, rtSweepS * 1000 / (pollMs > 0 ? pollMs : 50));
      } else {
        snprintf(hint, sizeof(hint), "Set a fuse rating - it sets the safe peak current.");
      }
      lv_label_set_text(rtSweepHint, hint);
    }
    // Probe wiring + tare. The hint is deliberately a hook-up instruction
    // rather than a definition: 4-wire only works if the sense leads land on
    // the DUT itself, past the force-lead contacts.
    lv_label_set_text(rtProbeVal, probeFourWire ? "4-wire (Kelvin)" : "2-wire");
    lv_obj_set_style_text_color(rtProbeVal, probeFourWire ? COL_GREEN : COL_INK, 0);
    lv_label_set_text(rtProbeHint,
        probeFourWire ? "Sense leads land ON the part, inside the force-lead clamps. Lead and contact resistance is excluded, so no tare is used."
                   : "Lead and contact resistance is measured in series with the part. Tare it below, or switch to 4-wire.");
    if (probeFourWire) lv_obj_add_flag(rtTareCard, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(rtTareCard, LV_OBJ_FLAG_HIDDEN);
    if (probeTareOhm > 0) {
      char ob[20]; fmtOhm(ob, sizeof(ob), probeTareOhm);
      lv_label_set_text(rtTareVal, ob);
      lv_obj_set_style_text_color(rtTareVal, COL_GREEN, 0);
    } else {
      lv_label_set_text(rtTareVal, "not set");
      lv_obj_set_style_text_color(rtTareVal, COL_FAINT, 0);
    }
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

// ---- Live sweep curves -----------------------------------------------------
// Same bounded-reservoir scheme as the battery discharge curve: capture densely,
// halve the resolution when full so the whole sweep always fits, and resample
// across the chart width on every refresh so the time axis grows smoothly.
static void rtLiveReset() {
  rtResN = 0; rtResStride = 1; rtResAcc = 0;
  rtLastElapsed = 0; rtHaveR = false;
}

static void rtLivePush(float v, float i, float r) {
  if (++rtResAcc < rtResStride) return;
  rtResAcc = 0;
  if (rtResN == RT_RES_N) {
    for (int k = 0; k < RT_RES_N / 2; k++) {
      rtResV[k] = rtResV[k * 2]; rtResI[k] = rtResI[k * 2]; rtResR[k] = rtResR[k * 2];
    }
    rtResN = RT_RES_N / 2;
    rtResStride *= 2;
  }
  rtResV[rtResN] = v; rtResI[rtResN] = i; rtResR[rtResN] = r;
  rtResN++;
}

// Resample `src` across the full chart width into `ser`. `scale` converts the
// float to the chart's integer coordinate space; lv_coord_t is int16_t, so
// callers pick a scale that keeps the range inside +/-32767.
static void rtResampleInto(lv_obj_t *chart, lv_chart_series_t *ser,
                           const float *src, float scale) {
  for (int j = 0; j < RT_LIVE_PTS; j++) {
    float val;
    if (rtResN == 1) val = src[0];
    else {
      float sf = (float)j * (rtResN - 1) / (RT_LIVE_PTS - 1);
      int i0 = (int)sf;
      if (i0 >= rtResN - 1) val = src[rtResN - 1];
      else { float f = sf - i0; val = src[i0] * (1 - f) + src[i0 + 1] * f; }
    }
    lv_chart_set_value_by_id(chart, ser, j, (lv_coord_t)(val * scale));
  }
}

// Largest power-of-ten scale that keeps `hi` inside the int16 chart coordinate
// space. Milliohms for a normal low-R circuit, coarser for a big resistance.
static float rtChartScale(float hi) {
  float s = 1000.0f;
  while (s > 1.0f && fabsf(hi) * s > 30000.0f) s /= 10.0f;
  return s;
}

static void rtLiveRefresh() {
  if (!rtLiveChart || rtResN == 0) return;
  // Throttle to ~6 Hz. Three 120-point series resampled at the 20 Hz poll rate
  // would be ~7000 chart writes a second for a curve the eye cannot follow
  // anyway; the numeric readouts still update on every packet.
  static uint32_t lastDraw = 0;
  uint32_t now = millis();
  if (now - lastDraw < 160) return;
  lastDraw = now;
  // V and I share one chart on two independent Y axes — the only way two series
  // whose units differ by orders of magnitude can be read together.
  float vLo = rtResV[0], vHi = rtResV[0], iLo = rtResI[0], iHi = rtResI[0];
  float rLo = rtResR[0], rHi = rtResR[0];
  for (int k = 1; k < rtResN; k++) {
    vLo = LV_MIN(vLo, rtResV[k]); vHi = LV_MAX(vHi, rtResV[k]);
    iLo = LV_MIN(iLo, rtResI[k]); iHi = LV_MAX(iHi, rtResI[k]);
    rLo = LV_MIN(rLo, rtResR[k]); rHi = LV_MAX(rHi, rtResR[k]);
  }
  if (vHi - vLo < 0.05f) { vHi = vLo + 0.05f; }
  if (iHi - iLo < 0.05f) { iHi = iLo + 0.05f; }
  float vPad = (vHi - vLo) * 0.08f, iPad = (iHi - iLo) * 0.08f;
  lv_chart_set_range(rtLiveChart, LV_CHART_AXIS_PRIMARY_Y,
                     (lv_coord_t)((vLo - vPad) * 100), (lv_coord_t)((vHi + vPad) * 100));
  lv_chart_set_range(rtLiveChart, LV_CHART_AXIS_SECONDARY_Y,
                     (lv_coord_t)((iLo - iPad) * 100), (lv_coord_t)((iHi + iPad) * 100));
  rtResampleInto(rtLiveChart, rtLiveSerV, rtResV, 100.0f);
  rtResampleInto(rtLiveChart, rtLiveSerI, rtResI, 100.0f);
  lv_chart_refresh(rtLiveChart);
  char b[40];
  snprintf(b, sizeof(b), "%.2f-%.2f V", vLo, vHi); lv_label_set_text(rtLiveYLbl, b);
  snprintf(b, sizeof(b), "%.2f-%.2f A  -  0-%.0f s", iLo, iHi, rtLastElapsed);
  lv_label_set_text(rtLiveXLbl, b);

  // Resistance chart. Only meaningful once the fit has something to work with,
  // so the whole card stays hidden until then rather than drawing a flat zero.
  if (!rtHaveR) { lv_obj_add_flag(rtRCard, LV_OBJ_FLAG_HIDDEN); return; }
  lv_obj_clear_flag(rtRCard, LV_OBJ_FLAG_HIDDEN);
  if (rHi - rLo < 1e-4f) { rHi = rLo + 1e-4f; }
  float rPad = (rHi - rLo) * 0.1f;
  // Scale chosen from the actual magnitude: milliohms give a low-R circuit real
  // resolution, but a multi-ohm result at that scale would overflow int16.
  float rScale = rtChartScale(rHi + rPad);
  lv_chart_set_range(rtRChart, LV_CHART_AXIS_PRIMARY_Y,
                     (lv_coord_t)((rLo - rPad) * rScale), (lv_coord_t)((rHi + rPad) * rScale));
  rtResampleInto(rtRChart, rtRSer, rtResR, rScale);
  lv_chart_refresh(rtRChart);
  char lo[20], hi[20], rr[48];
  fmtOhm(lo, sizeof(lo), rLo); fmtOhm(hi, sizeof(hi), rHi);
  snprintf(rr, sizeof(rr), "%s - %s", lo, hi); lv_label_set_text(rtRYLbl, rr);
}

// Immediate feedback when a sweep starts: the engine primes for ~1 s before the
// ramp begins, which would otherwise leave the UI on the previous screen looking
// dead after RUN TEST / Start sweep.
static void enterRtRun() {
  rtPhase = RT_RUN;
  rtLiveReset();
  rtTotalS = (float)(rtTareRunning ? TARE_SWEEP_S : (uint32_t)rtSweepS);
  lv_label_set_text(runStepLbl, rtTareRunning ? "TARE SWEEP" : "PRIMING");
  lv_bar_set_value(runBar, 0, LV_ANIM_OFF);
  lv_label_set_text(runVLbl, "--");
  lv_label_set_text(runILbl, "-- A");
  lv_label_set_text(runRLbl, "R --");
  lv_label_set_text(runTargetLbl, "target -- A");
  lv_obj_add_flag(rtRCard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(rtStatusLbl, LV_OBJ_FLAG_HIDDEN);  // clear a stale error
  resetSaveButton(saveBtn, saveBtnLbl);   // drop the previous run's save outcome
  refreshRtest();
  showScreen(SCR_RTEST);
  refreshTestChip();
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

// A key/value row that is itself a button — used where the value is edited
// somewhere else (the text-entry overlay) rather than in place.
static lv_obj_t *tapRow(lv_obj_t *parent, const char *k, lv_event_cb_t cb, void *ud) {
  lv_obj_t *row = flatBtn(parent);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_ver(row, 8, 0);
  lv_obj_set_style_pad_hor(row, 0, 0);
  lbl(row, k, COL_MUTED, F14);
  lv_obj_t *v = lbl(row, "--", COL_ACCENT2, F14);
  lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
  return v;
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

// Idle-dim choices, in seconds (0 = never).
// Screen-timeout choices. The panel dims after this long without a touch or a
// button press and blanks at 5x that, so "30s" is a genuinely useful bench
// setting (short dim, blank at 2.5 min) and "30 min" suits watching a long
// discharge. Laid out as two rows of three.
static const int DIM_N = 6;
static const uint16_t DIM_SECS[DIM_N] = {0, 30, 60, 300, 600, 1800};
static const char *DIM_NAMES[DIM_N] = {"Never", "30 s", "1 min", "5 min", "10 min", "30 min"};
static const int DIM_PER_ROW = 3;

static void refreshAutoConn() {
  bool on = prefs::get().autoConnect;
  bool have = prefs::get().lastAddr[0] != '\0';
  lv_label_set_text(setAutoConnLbl, on ? "Auto-connect on" : "Auto-connect off");
  lv_obj_set_style_border_color(setAutoConnBtn, on ? COL_GREEN : COL_FAINT, 0);
  lv_obj_set_style_text_color(setAutoConnLbl, on ? COL_GREEN : COL_FAINT, 0);
  // Honest hint when enabled but nothing has ever connected to store an address.
  if (on && !have) {
    lv_label_set_text(setAutoConnLbl, "Auto-connect on (no device yet)");
    lv_obj_set_style_text_color(setAutoConnLbl, COL_AMBER, 0);
  }
}

static void refreshScreenProt() {
  bool on = display::pixelShift();
  lv_label_set_text(setPxShiftLbl, on ? "Pixel shift on" : "Pixel shift off");
  lv_obj_set_style_border_color(setPxShiftBtn, on ? COL_GREEN : COL_FAINT, 0);
  lv_obj_set_style_text_color(setPxShiftLbl, on ? COL_GREEN : COL_FAINT, 0);
  uint16_t cur = display::idleDim();
  for (int i = 0; i < DIM_N; i++) {
    bool sel = DIM_SECS[i] == cur;
    lv_obj_set_style_bg_color(dimChip[i], sel ? lv_color_hex(0x1d1b33) : COL_INSET, 0);
    lv_obj_set_style_border_color(dimChip[i], sel ? COL_ACCENT : COL_BORDER, 0);
    lv_obj_set_style_text_color(dimChipLbl[i], sel ? COL_ACCENT2 : COL_MUTED, 0);
  }
  if (setDimSummary) {
    char b[224];
    if (cur == 0) {
      snprintf(b, sizeof(b), "The screen stays at full brightness indefinitely.");
    } else {
      char dimTxt[16], blankTxt[16];
      auto fmtDur = [](uint32_t s, char *out, int n) {
        if (s < 60) snprintf(out, n, "%lu s", (unsigned long)s);
        else if (s % 60 == 0) snprintf(out, n, "%lu min", (unsigned long)(s / 60));
        else snprintf(out, n, "%lu min %lu s", (unsigned long)(s / 60), (unsigned long)(s % 60));
      };
      fmtDur(cur, dimTxt, sizeof(dimTxt));
      fmtDur((uint32_t)cur * 5u, blankTxt, sizeof(blankTxt));
      snprintf(b, sizeof(b),
               "Dims after %s idle, goes black at %s. Any touch or button wakes it; "
               "blanking is suppressed while a test is running.", dimTxt, blankTxt);
    }
    lv_label_set_text(setDimSummary, b);
  }
}

static void refreshWifi() {
  const prefs::Data &p = prefs::get();
  setTextIf(setSsidVal, p.ssid[0] ? p.ssid : "Tap to scan");
  setTextIf(setPassVal, p.pass[0] ? "********" : "Not set");
  char tz[12];
  int m = p.tzMinutes;
  snprintf(tz, sizeof(tz), "UTC%+d%s", m / 60, m % 60 ? ":30" : "");
  setTextIf(setTzVal, tz);
  bool ready = p.ssid[0] != '\0';
  lv_obj_set_style_text_color(setSyncLbl, ready ? COL_ACCENT2 : COL_FAINT, 0);
}

// Probe wiring is read by main.cpp on EVERY status packet, so it has to reach
// prefs the moment it changes rather than waiting for a screen refresh to
// persist it. prefs::change updates the live struct immediately; the flash write
// stays debounced.
static void persistProbe() {
  prefs::change([](prefs::Data &d) {
    d.fourWire = probeFourWire;
    d.tareOhm = probeTareOhm;
  });
}

static void refreshProbe() {
  if (!setProbeBtn) return;
  setTextIf(setProbeLbl, probeFourWire ? "4-wire (Kelvin)" : "2-wire");
  lv_obj_set_style_text_color(setProbeLbl, probeFourWire ? COL_GREEN : COL_INK, 0);
  lv_obj_set_style_border_color(setProbeBtn, probeFourWire ? COL_GREEN : COL_BORDER, 0);
  // The tare only exists to stand in for sense leads you do not have; in 4-wire
  // there is nothing for it to correct, so the row goes away rather than sitting
  // there implying it still does something.
  lv_obj_t *tareRow = lv_obj_get_parent(setTareRow);
  if (probeFourWire) lv_obj_add_flag(tareRow, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(tareRow, LV_OBJ_FLAG_HIDDEN);
  if (probeTareOhm > 0) {
    char ob[24];
    fmtOhm(ob, sizeof(ob), probeTareOhm);
    setTextIf(setTareRow, ob);
    lv_obj_set_style_text_color(setTareRow, COL_GREEN, 0);
  } else {
    setTextIf(setTareRow, "not set");
    lv_obj_set_style_text_color(setTareRow, COL_FAINT, 0);
  }
  char note[288];
  if (probeFourWire) {
    snprintf(note, sizeof(note),
             "Sense leads land ON the part, inside the force-lead clamps, so lead and "
             "contact resistance never enters the reading. Readings are used exactly as "
             "the load reports them. Applies to every mode.");
  } else if (probeTareOhm > 0) {
    char ob[24];
    fmtOhm(ob, sizeof(ob), probeTareOhm);
    snprintf(note, sizeof(note),
             "Every voltage is corrected back to the part by adding current x %s - "
             "%.0f mV at 1 A, %.0f mV at 5 A. Applies to every mode: monitor, graph, "
             "battery cutoff and reports. R-Test is excluded (it already subtracts the "
             "tare from its own slope).", ob, probeTareOhm * 1000.0f, probeTareOhm * 5000.0f);
  } else {
    snprintf(note, sizeof(note),
             "The load senses voltage at its own terminals, so readings are short by the "
             "drop across your leads. Set the lead resistance - type it, or let "
             "R-Test > Measure (short the probes) find it - and every mode is corrected "
             "for it.");
  }
  lv_label_set_text(setProbeNote, note);
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
  prefs::change([](prefs::Data &d) { d.pollMs = (uint16_t)pollMs; });
  refreshRateChips();
}

static void onBrightness(lv_event_t *e) {
  int v = lv_slider_get_value(lv_event_get_target(e));
  display::setBrightness((uint8_t)v);
  prefs::change([v](prefs::Data &d) { d.brightness = (uint8_t)v; });
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
  prefs::change([v](prefs::Data &d) { d.volume = (uint8_t)v; });
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
    prefs::change([](prefs::Data &d) { d.muted = audio::muted(); });
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

  // probe wiring — global, applied to every mode (not just the R-test)
  lv_obj_t *pwc = settingsCard("PROBE WIRING");
  lv_obj_set_style_pad_row(pwc, 8, 0);
  setProbeBtn = flatBtn(pwc);
  lv_obj_set_size(setProbeBtn, LV_PCT(100), 46);
  styleCard(setProbeBtn, COL_BLACK, COL_GREEN, 11, 0);
  lv_obj_set_style_bg_opa(setProbeBtn, LV_OPA_TRANSP, 0);
  setProbeLbl = lbl(setProbeBtn, "2-wire", COL_INK, F16);
  lv_obj_center(setProbeLbl);
  lv_obj_add_event_cb(setProbeBtn, [](lv_event_t *) {
    probeFourWire = !probeFourWire;
    persistProbe();
    refreshProbe();
    refreshRtest();     // the R-Test setup mirrors these two values
    refreshMonitor();   // the voltage caption says when a correction is live
  }, LV_EVENT_CLICKED, nullptr);
  setTareRow = tapRow(pwc, "Lead resistance", [](lv_event_t *) { openKeypad(10); }, nullptr);
  setProbeNote = lbl(pwc, "", COL_FAINT, F12);
  lv_label_set_long_mode(setProbeNote, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(setProbeNote, LV_PCT(100));
  refreshProbe();

  // connection: auto-connect to the last device on startup
  lv_obj_t *conc = settingsCard("CONNECTION");
  lv_obj_set_style_pad_row(conc, 8, 0);
  lv_obj_t *acNote = lbl(conc, "When on, the controller reconnects to the last device automatically at startup - no scan or tap needed.", COL_FAINT, F12);
  lv_label_set_long_mode(acNote, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(acNote, LV_PCT(100));
  setAutoConnBtn = flatBtn(conc);
  lv_obj_set_size(setAutoConnBtn, LV_PCT(100), 44);
  styleCard(setAutoConnBtn, COL_BLACK, COL_GREEN, 11, 0);
  lv_obj_set_style_bg_opa(setAutoConnBtn, LV_OPA_TRANSP, 0);
  setAutoConnLbl = lbl(setAutoConnBtn, "Auto-connect off", COL_FAINT, F16);
  lv_obj_center(setAutoConnLbl);
  lv_obj_add_event_cb(setAutoConnBtn, [](lv_event_t *) {
    prefs::change([](prefs::Data &d) { d.autoConnect = !d.autoConnect; });
    prefs::flush();
    refreshAutoConn();
  }, LV_EVENT_CLICKED, nullptr);
  refreshAutoConn();

  // battery (AXP2101)
  lv_obj_t *pc = settingsCard("BATTERY");
  setBattVal = kvRow(pc, "Level");
  setBattState = kvRow(pc, "State");

  // clock (PCF85063) + Wi-Fi NTP sync
  lv_obj_t *cc2 = settingsCard("CLOCK");
  lv_obj_set_style_pad_row(cc2, 6, 0);
  setRtcVal = kvRow(cc2, "RTC");
  lv_obj_t *ntpNote = lbl(cc2, "Set the clock from the internet so saved reports carry a real timestamp. The Wi-Fi radio is powered only for the sync, and a sync is refused while a test is running - the BLE link is the only way to stop the load.", COL_FAINT, F12);
  lv_label_set_long_mode(ntpNote, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ntpNote, LV_PCT(100));
  setSsidVal = tapRow(cc2, "Wi-Fi network", [](lv_event_t *) { startWifiScan(); }, nullptr);
  setPassVal = tapRow(cc2, "Password", [](lv_event_t *) { openTextEntry(KB_PASS); }, nullptr);
  lv_obj_t *tzRow = cont(cc2);
  lv_obj_set_size(tzRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tzRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tzRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_ver(tzRow, 6, 0);
  lbl(tzRow, "UTC offset", COL_MUTED, F14);
  lv_obj_t *tzCtl = cont(tzRow);
  lv_obj_set_size(tzCtl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tzCtl, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tzCtl, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(tzCtl, 8, 0);
  lv_obj_t *tzM = flatBtn(tzCtl); lv_obj_set_size(tzM, 44, 40);
  styleCard(tzM, COL_INSET, COL_BORDER, 10, 0);
  lv_obj_t *tzMl = lbl(tzM, LV_SYMBOL_MINUS, COL_INK, F16); lv_obj_center(tzMl);
  lv_obj_add_event_cb(tzM, [](lv_event_t *) {
    prefs::change([](prefs::Data &d) { if (d.tzMinutes > -12 * 60) d.tzMinutes -= 60; });
    refreshWifi();
  }, LV_EVENT_CLICKED, nullptr);
  setTzVal = lbl(tzCtl, "UTC+0", COL_INK, F16);
  lv_obj_t *tzP = flatBtn(tzCtl); lv_obj_set_size(tzP, 44, 40);
  styleCard(tzP, COL_INSET, COL_BORDER, 10, 0);
  lv_obj_t *tzPl = lbl(tzP, LV_SYMBOL_PLUS, COL_INK, F16); lv_obj_center(tzPl);
  lv_obj_add_event_cb(tzP, [](lv_event_t *) {
    prefs::change([](prefs::Data &d) { if (d.tzMinutes < 14 * 60) d.tzMinutes += 60; });
    refreshWifi();
  }, LV_EVENT_CLICKED, nullptr);
  setSyncBtn = flatBtn(cc2);
  lv_obj_set_size(setSyncBtn, LV_PCT(100), 46);
  styleCard(setSyncBtn, COL_INSET, COL_BORDER, 11, 0);
  setSyncLbl = lbl(setSyncBtn, "Sync clock now", COL_ACCENT2, F16);
  lv_obj_center(setSyncLbl);
  lv_obj_add_event_cb(setSyncBtn, [](lv_event_t *) {
    if (!A.syncClock) return;
    // Never touch the radio while ANYTHING is sinking current — engines AND a
    // manually-energised load (a successful sync deliberately reboots the
    // controller, which would abandon a hot load with no supervisor).
    if (engineBusy() || lastLoadOn) {
      lv_label_set_text(setNetStatus, "Not while the load is on.");
      lv_obj_set_style_text_color(setNetStatus, COL_AMBER, 0);
      return;
    }
    lv_label_set_text(setNetStatus, "Starting Wi-Fi...");
    lv_obj_set_style_text_color(setNetStatus, COL_MUTED, 0);
    lv_refr_now(nullptr);                 // paint before the buffer shrinks
    display::setLowMemMode(true);         // free RAM for the Wi-Fi stack
    A.syncClock();
  }, LV_EVENT_CLICKED, nullptr);
  setNetStatus = lbl(cc2, "", COL_MUTED, F12);
  lv_label_set_long_mode(setNetStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(setNetStatus, LV_PCT(100));
  refreshWifi();

  // AMOLED protection: pixel shift + idle dim/blank.
  lv_obj_t *pc2 = settingsCard("SCREEN PROTECTION");
  lv_obj_set_style_pad_row(pc2, 8, 0);
  lv_obj_t *psNote = lbl(pc2, "This panel is an AMOLED: static labels burn in permanently. Pixel shift creeps the layout by a few pixels; idle dim drops brightness (and blanks it later) when nobody is touching it.", COL_FAINT, F12);
  lv_label_set_long_mode(psNote, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(psNote, LV_PCT(100));
  setPxShiftBtn = flatBtn(pc2);
  lv_obj_set_size(setPxShiftBtn, LV_PCT(100), 44);
  styleCard(setPxShiftBtn, COL_BLACK, COL_GREEN, 11, 0);
  lv_obj_set_style_bg_opa(setPxShiftBtn, LV_OPA_TRANSP, 0);
  setPxShiftLbl = lbl(setPxShiftBtn, "Pixel shift on", COL_GREEN, F16);
  lv_obj_center(setPxShiftLbl);
  lv_obj_add_event_cb(setPxShiftBtn, [](lv_event_t *) {
    display::setPixelShift(!display::pixelShift());
    prefs::change([](prefs::Data &d) { d.pixelShift = display::pixelShift(); });
    refreshScreenProt();
  }, LV_EVENT_CLICKED, nullptr);
  lbl(pc2, "SCREEN TIMEOUT", COL_MUTED, F12);
  // Two rows of three: six chips will not fit legibly across 368 px.
  for (int row = 0; row < (DIM_N + DIM_PER_ROW - 1) / DIM_PER_ROW; row++) {
    lv_obj_t *dimRow = cont(pc2);
    lv_obj_set_size(dimRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dimRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dimRow, 7, 0);
    for (int c = 0; c < DIM_PER_ROW; c++) {
      int i = row * DIM_PER_ROW + c;
      if (i >= DIM_N) break;
      dimChip[i] = flatBtn(dimRow);
      lv_obj_set_flex_grow(dimChip[i], 1);
      lv_obj_set_height(dimChip[i], 42);
      styleCard(dimChip[i], COL_INSET, COL_BORDER, 11, 0);
      dimChipLbl[i] = lbl(dimChip[i], DIM_NAMES[i], COL_MUTED, F14);
      lv_obj_center(dimChipLbl[i]);
      lv_obj_add_event_cb(dimChip[i], [](lv_event_t *e) {
        int k = (int)(intptr_t)lv_event_get_user_data(e);
        display::setIdleDim(DIM_SECS[k]);
        prefs::change([](prefs::Data &d) { d.idleDimS = display::idleDim(); });
        refreshScreenProt();
      }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
  }
  // Spell out what the selected value actually does — "dim then blank at 5x" is
  // not guessable from a bare duration.
  setDimSummary = lbl(pc2, "", COL_FAINT, F12);
  lv_label_set_long_mode(setDimSummary, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(setDimSummary, LV_PCT(100));
  refreshScreenProt();

  // SD card. Probed on demand only: mounting takes ~1 s and stops the panel
  // drawing (shared SPI bus), so it must never sit on the 1 Hz settings tick.
  lv_obj_t *sdc = settingsCard("SD CARD");
  setSdVal = kvRow(sdc, "Status");
  lv_label_set_text(setSdVal, "Not checked");
  lv_obj_t *sdBtn = flatBtn(sdc);
  lv_obj_set_size(sdBtn, LV_PCT(100), 44);
  styleCard(sdBtn, COL_INSET, COL_BORDER, 11, 0);
  lv_obj_t *sdBtnLbl = lbl(sdBtn, "Check card", COL_ACCENT2, F14);
  lv_obj_center(sdBtnLbl);
  lv_obj_add_event_cb(sdBtn, [](lv_event_t *) {
    if (!A.sdInfo) return;
    lv_label_set_text(setSdVal, "Checking...");
    lv_refr_now(nullptr);   // paint before the bus goes to the card
    char msg[48] = "";
    bool ok = A.sdInfo(msg, sizeof(msg));
    lv_label_set_text(setSdVal, msg);
    lv_obj_set_style_text_color(setSdVal, ok ? COL_GREEN : COL_AMBER, 0);
  }, LV_EVENT_CLICKED, nullptr);

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
  lv_obj_add_event_cb(rb, [](lv_event_t *) {
    prefs::flush();   // a deliberate restart shouldn't lose the last edit
    ESP.restart();
  }, LV_EVENT_CLICKED, nullptr);
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
// Persistent per-run state for the discharge frame (see battChartRefresh).
static float btSmooth = 0;                 // EMA-filtered voltage
static bool  btFiltInit = false;
static float btRangeLo = 0, btRangeHi = 0; // currently-applied Y frame
static bool  btRangeInit = false;

static void battHistReset() {
  btHistN = 0; btHistStride = 1; btHistAcc = 0; btLastElapsed = 0;
  btFiltInit = false; btRangeInit = false;
}

// The EL15's own envelope applied to a requested discharge current. Voc comes
// from the live reading when something is connected; with nothing connected only
// the 12 A ceiling can be applied here. The engine re-clamps against the real
// source voltage at priming either way — this is about the setup screen telling
// the truth before the user commits, rather than promising a current the load
// will quietly refuse.
static float clampBattAmps(float a) {
  if (a < 0.01f) a = 0.01f;
  float cap = el15::MAX_CURRENT_A;
  if (connected && lastStatus.valid && lastStatus.voltage > el15::MIN_VOLTAGE_V)
    cap = LV_MIN(cap, el15::MAX_POWER_W / lastStatus.voltage);
  return a > cap ? cap : a;
}

// Work the discharge current out from the selected C-rate and the pack's rated
// capacity: 0.2C of a 3000 mAh pack is 0.60 A. This is the direction a datasheet
// specifies a capacity test in — a capacity figure only means something at a
// stated rate — so the controller does the arithmetic rather than the user.
// No-ops when no rate is selected (the user typed a current by hand) or no
// rating has been entered.
static void applyCRate() {
  if (battCRateIdx < 0 || battCRateIdx >= battmodel::CRATE_N || battRatedAh <= 0) return;
  battAmps = clampBattAmps(BATT_CHEMS[battChem].cRate[battCRateIdx] * battRatedAh);
}

// Bring the pack size into range for a chemistry without touching a cutoff the
// user typed themselves. Used both when adopting a chemistry and at boot, where
// a setup stored by an older firmware may name a cell count this build no longer
// allows (lead-acid is fixed at 12 V now, and every limit is capped so a FULL
// pack stays under the EL15's 60 V input rating).
static void clampCells() {
  if (battChem < 0 || battChem >= BATT_CHEM_N) battChem = 0;
  const BattChem &c = BATT_CHEMS[battChem];
  if (c.fixedCells) battCells = c.fixedCells;
  else if (c.maxCells && battCells > c.maxCells) battCells = c.maxCells;
  if (battCells < 1) battCells = 1;
}

// Adopt a chemistry: fix or clamp the pack size, re-derive the auto cutoff, and
// re-apply the selected test rate (the C-rate presets are per chemistry).
static void applyChem(int idx) {
  battChem = idx;
  clampCells();
  const BattChem &c = BATT_CHEMS[battChem];
  battCutoffCustom = false;
  if (c.maxCells) battCutoff = c.cut * battCells;
  applyCRate();
}

static void battHistPush(float v) {
  // Light EMA first: the raw load-ADC voltage carries a few mV of noise that
  // otherwise shows as squiggle. The discharge is slow, so the lag is trivial.
  if (!btFiltInit) { btSmooth = v; btFiltInit = true; }
  else btSmooth += (v - btSmooth) * 0.4f;
  v = btSmooth;
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
  // Dynamic auto-zoom to the actual voltage band, but SNAPPED to a 0.1 V grid
  // and only ever EXPANDED — so the frame moves in occasional discrete steps as
  // the discharge crosses a grid line, not on every poll. The curve fills the
  // chart (shows detail) yet keeps its shape between steps instead of rescaling
  // constantly. Never shrinking is what stops it hunting/squiggling.
  float dlo = btHistV[0], dhi = btHistV[0];
  for (int k = 1; k < btHistN; k++) { dlo = LV_MIN(dlo, btHistV[k]); dhi = LV_MAX(dhi, btHistV[k]); }
  const float STEP = 0.1f, PAD = 0.04f, MIN_SPAN = 0.20f;
  float wantLo = floorf((dlo - PAD) / STEP) * STEP;
  float wantHi = ceilf((dhi + PAD) / STEP) * STEP;
  if (wantHi - wantLo < MIN_SPAN) wantHi = wantLo + MIN_SPAN;
  if (!btRangeInit) { btRangeLo = wantLo; btRangeHi = wantHi; btRangeInit = true; }
  else { if (wantLo < btRangeLo) btRangeLo = wantLo; if (wantHi > btRangeHi) btRangeHi = wantHi; }
  float top = btRangeHi, bottom = btRangeLo;
  lv_chart_set_range(btChart, LV_CHART_AXIS_PRIMARY_Y,
                     (lv_coord_t)(bottom * 100), (lv_coord_t)(top * 100));
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
  snprintf(b, sizeof(b), "%.2f-%.2f V", bottom, top); lv_label_set_text(btChartYLbl, b);
  char el[16];
  hhmmss((int)btLastElapsed, el, sizeof(el));
  snprintf(b, sizeof(b), "0 - %s", el); lv_label_set_text(btChartXLbl, b);
}

static void refreshBatt() {
  persistSetup();
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
    char b[96];
    // one_checked mode clears the other buttons for us
    lv_btnmatrix_set_btn_ctrl(btChemMx, (uint16_t)battChem, LV_BTNMATRIX_CTRL_CHECKED);
    // The cell-count control only earns its space when the count is actually a
    // choice: lead-acid is fixed at 12 V here, and Custom has no cell model.
    if (c.maxCells > 0 && !c.fixedCells) lv_obj_clear_flag(btCellsRow, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(btCellsRow, LV_OBJ_FLAG_HIDDEN);
    char det[224];
    if (c.maxCells) {
      int n = snprintf(det, sizeof(det),
                       "%s: %.2f V/cell nominal, %.2f-%.2f V/cell.\n%dS pack = %.1f V nominal, "
                       "%.1f V empty to %.1f V full.",
                       c.name, c.nom, c.cut, c.full, battCells,
                       c.nom * battCells, c.cut * battCells, c.full * battCells);
      if (!c.fixedCells && n < (int)sizeof(det))
        snprintf(det + n, sizeof(det) - n, " Up to %dS (the EL15 tops out at 60 V).", c.maxCells);
    } else {
      snprintf(det, sizeof(det),
               "No cell model - set the cutoff voltage directly. Custom has no discharge "
               "curve, so time remaining falls back to the rated capacity.");
    }
    lv_label_set_text(btChemDetail, det);
    if (c.maxCells) { snprintf(b, sizeof(b), "%dS", battCells); lv_label_set_text(btCellsVal, b); }
    else lv_label_set_text(btCellsVal, "-");
    snprintf(b, sizeof(b), "%.2f V", battCutoff); lv_label_set_text(btCutoffVal, b);
    snprintf(b, sizeof(b), "%.2f A", battAmps); lv_label_set_text(btAmpsVal, b);
    if (battRatedAh > 0) snprintf(b, sizeof(b), "%.0f mAh", battRatedAh * 1000.0f);
    else snprintf(b, sizeof(b), "not set");
    lv_label_set_text(btRatedVal, b);
    lv_obj_set_style_text_color(btRatedVal, battRatedAh > 0 ? COL_ACCENT2 : COL_FAINT, 0);
    // C-rate chips. Live only once a rating is known, since a C-rate without a
    // capacity to multiply is meaningless.
    bool rated = battRatedAh > 0;
    for (int i = 0; i < battmodel::CRATE_N; i++) {
      char cb[24];   // %g is worst-case wide; the real values render as "0.05C"
      snprintf(cb, sizeof(cb), "%gC", c.cRate[i]);
      lv_label_set_text(btCRateChipLbl[i], cb);
      bool on = rated && battCRateIdx == i;
      lv_obj_set_style_bg_color(btCRateChip[i], on ? lv_color_hex(0x1d1b33) : COL_INSET, 0);
      lv_obj_set_style_border_color(btCRateChip[i], on ? COL_ACCENT : COL_BORDER, 0);
      lv_obj_set_style_text_color(btCRateChipLbl[i],
                                  on ? COL_ACCENT2 : rated ? COL_MUTED : COL_FAINT, 0);
    }
    // State the plan in the terms a battery datasheet uses (C-rate + expected
    // runtime); without a rating, say exactly what is lost — which since the
    // discharge-curve estimate landed is only the C-rate and state of health,
    // not the time remaining.
    char hint[224];
    int n = 0;
    bool curve = battmodel::hasCurve(battChem);
    if (rated && battAmps > 0.005f) {
      float cRate = battAmps / battRatedAh;
      float hrs = battRatedAh / battAmps;
      n = snprintf(hint, sizeof(hint), "%.2fC - a healthy pack should last about %dh %02dm.",
                   cRate, (int)hrs, (int)((hrs - (int)hrs) * 60));
      // Say so when the load's envelope, not the chosen rate, is setting the
      // current — otherwise the chip reads as selected while the test runs slower.
      if (battCRateIdx >= 0) {
        float want = c.cRate[battCRateIdx] * battRatedAh;
        if (want > battAmps * 1.02f && n < (int)sizeof(hint))
          n += snprintf(hint + n, sizeof(hint) - n,
                        " Capped from %.2f A by the load's 150 W / 12 A envelope.", want);
      }
    } else if (rated) {
      n = snprintf(hint, sizeof(hint), "Pick a test rate above to set the discharge current.");
    } else {
      n = snprintf(hint, sizeof(hint),
                   "Optional. Enter the pack's rated mAh to set the current from a C-rate "
                   "and get a state-of-health figure.");
    }
    if (!curve && n < (int)sizeof(hint))
      snprintf(hint + n, sizeof(hint) - n,
               " Custom has no discharge curve, so time remaining falls back to the rating.");
    lv_label_set_text(btRateHint, hint);
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
    if (battPausedFlag) {
      char b[256];
      snprintf(b, sizeof(b), LV_SYMBOL_WARNING "  PAUSED - %s\nThe load is off and the clock is "
               "stopped; every reading so far is kept. Resume to carry on.",
               battPauseWhy[0] ? battPauseWhy : "suspended");
      lv_label_set_text(btPauseWhyLbl, b);
      lv_obj_clear_flag(btPauseCard, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(btPauseCard, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    lv_obj_clear_flag(btResultBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(btChartCard, LV_OBJ_FLAG_HIDDEN);
  }
}

// Immediate feedback on start (the engine primes for ~1.5 s before discharging).
static void enterBattRun() {
  btPhase = BT_RUN;
  battPausedFlag = false;
  battPauseWhy[0] = '\0';
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
  lv_obj_add_flag(btEtaLbl, LV_OBJ_FLAG_HIDDEN);
  resetSaveButton(btSaveBtn, btSaveLbl);   // drop the previous run's save outcome
  refreshBatt();
  showScreen(SCR_BATT);
  refreshTestChip();
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
  // Chemistry: one chip per family rather than a tap-to-cycle row. With ten of
  // them, cycling would be up to ten taps to get back to where you started —
  // and picking the wrong chemistry silently mis-reads every charge estimate,
  // so it should be a choice you can see, not one you land on.
  lbl(setupCard, "Chemistry", COL_MUTED, F12);
  // Three per row. The map is built rather than written out so adding a
  // chemistry to battery_model.h needs no change here.
  static const char *chemMap[BATT_CHEM_N + BATT_CHEM_N / 3 + 2];
  {
    int m = 0;
    for (int i = 0; i < BATT_CHEM_N; i++) {
      chemMap[m++] = BATT_CHEMS[i].shortName;
      if ((i + 1) % 3 == 0 && i + 1 < BATT_CHEM_N) chemMap[m++] = "\n";
    }
    chemMap[m] = "";   // terminator
  }
  btChemMx = lv_btnmatrix_create(setupCard);
  lv_btnmatrix_set_map(btChemMx, chemMap);
  lv_btnmatrix_set_one_checked(btChemMx, true);   // radio behaviour
  lv_obj_set_width(btChemMx, LV_PCT(100));
  lv_obj_set_height(btChemMx, 4 * 32 + 3 * 5 + 8);   // 4 rows of 32 px + gaps
  lv_obj_set_style_bg_opa(btChemMx, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btChemMx, 0, 0);
  lv_obj_set_style_pad_all(btChemMx, 0, 0);
  lv_obj_set_style_pad_row(btChemMx, 5, 0);
  lv_obj_set_style_pad_column(btChemMx, 5, 0);
  lv_obj_set_style_bg_color(btChemMx, COL_INSET, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(btChemMx, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_border_color(btChemMx, COL_BORDER, LV_PART_ITEMS);
  lv_obj_set_style_border_width(btChemMx, 1, LV_PART_ITEMS);
  lv_obj_set_style_radius(btChemMx, 9, LV_PART_ITEMS);
  lv_obj_set_style_text_color(btChemMx, COL_MUTED, LV_PART_ITEMS);
  lv_obj_set_style_text_font(btChemMx, F12, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(btChemMx, lv_color_hex(0x1d1b33), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(btChemMx, COL_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(btChemMx, COL_ACCENT2, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_add_event_cb(btChemMx, [](lv_event_t *e) {
    uint16_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    // Refresh either way: a rejected tap has already moved the matrix's own
    // checked state, so the screen has to be put back the way it was.
    if (!engineBusy() && id < BATT_CHEM_N) applyChem((int)id);
    refreshBatt();
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  // What the selection means in pack volts — the number the user actually wires
  // up to, rather than a per-cell figure they have to multiply in their head.
  btChemDetail = lbl(setupCard, "", COL_FAINT, F12);
  lv_label_set_long_mode(btChemDetail, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(btChemDetail, LV_PCT(100));
  // cells -/+ row (hidden for chemistries whose pack size is fixed or absent)
  lv_obj_t *crow = cont(setupCard);
  btCellsRow = crow;
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
  // Optional nameplate capacity. Everything derived from it (C-rate, ETA,
  // state of health) is simply hidden when it is left at 0, so the test never
  // pretends to know a pack's rating.
  bRow(setupCard, "Rated capacity (mAh) - optional", &btRatedVal, [](lv_event_t *) { openKeypad(6); });
  // C-rate chips: the discharge current is worked out FROM the pack size rather
  // than typed. Greyed out until a rating is entered, and dropped the moment the
  // user types a current by hand.
  lbl(setupCard, "Test rate - tap to set the current", COL_MUTED, F12);
  lv_obj_t *crRow = cont(setupCard);
  lv_obj_set_size(crRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(crRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(crRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(crRow, 6, 0);
  lv_obj_set_style_pad_ver(crRow, 4, 0);
  for (int i = 0; i < battmodel::CRATE_N; i++) {
    btCRateChip[i] = flatBtn(crRow);
    lv_obj_set_flex_grow(btCRateChip[i], 1);
    lv_obj_set_height(btCRateChip[i], 34);
    styleCard(btCRateChip[i], COL_INSET, COL_BORDER, 9, 0);
    btCRateChipLbl[i] = lbl(btCRateChip[i], "", COL_MUTED, F14);
    lv_obj_center(btCRateChipLbl[i]);
    lv_obj_add_event_cb(btCRateChip[i], [](lv_event_t *e) {
      if (engineBusy() || battRatedAh <= 0) return;
      battCRateIdx = (int)(intptr_t)lv_event_get_user_data(e);
      applyCRate();
      refreshBatt();
    }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  btRateHint = lbl(setupCard, "", COL_FAINT, F12);
  lv_label_set_long_mode(btRateHint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(btRateHint, LV_PCT(100));

  btStartBtn = flatBtn(btIdleBox);
  lv_obj_set_size(btStartBtn, LV_PCT(100), 62);
  lv_obj_set_style_radius(btStartBtn, 14, 0);
  btStartLbl = lbl(btStartBtn, LV_SYMBOL_PLAY "  Start discharge", COL_DARKINK, F16);
  lv_obj_center(btStartLbl);
  lv_obj_add_event_cb(btStartBtn, [](lv_event_t *) {
    if (engineBusy()) return;
    if (battCutoff <= 0.05f || battAmps <= 0.005f) return;
    if (!connected) { showScreen(SCR_CONNECT); return; }
    if (A.startBatt) { A.startBatt(battCutoff, battAmps, battRatedAh, battChem, battCells); enterBattRun(); }
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
  // Progress against the nameplate capacity + estimated time to reach it.
  // Hidden entirely when no rating was entered.
  btEtaLbl = lbl(runCard, "", COL_ACCENT2, F14);
  lv_label_set_long_mode(btEtaLbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(btEtaLbl, LV_PCT(100));
  lv_obj_add_flag(btEtaLbl, LV_OBJ_FLAG_HIDDEN);

  // Paused banner + RESUME. A paused test is load-off but still owns the run,
  // so the user needs both an explanation and a one-tap way back into it.
  btPauseCard = cont(btRunBox);
  lv_obj_set_size(btPauseCard, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(btPauseCard, lv_color_hex(0x2a2210), COL_AMBER, 13, 11);
  lv_obj_set_flex_flow(btPauseCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(btPauseCard, 8, 0);
  btPauseWhyLbl = lbl(btPauseCard, "", COL_AMBER, F12);
  lv_label_set_long_mode(btPauseWhyLbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(btPauseWhyLbl, LV_PCT(100));
  btResumeBtn = flatBtn(btPauseCard);
  lv_obj_set_size(btResumeBtn, LV_PCT(100), 54);
  lv_obj_set_style_bg_color(btResumeBtn, COL_ACCENT, 0);
  lv_obj_set_style_bg_opa(btResumeBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btResumeBtn, 13, 0);
  lv_obj_t *rsl = lbl(btResumeBtn, LV_SYMBOL_PLAY "  RESUME", COL_DARKINK, F16);
  lv_obj_center(rsl);
  lv_obj_add_event_cb(btResumeBtn, [](lv_event_t *) {
    if (!A.resumeBatt) return;
    if (!connected) { showScreen(SCR_CONNECT); return; }   // nothing to resume onto
    if (A.resumeBatt()) { battPausedFlag = false; refreshBatt(); refreshTestChip(); }
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(btPauseCard, LV_OBJ_FLAG_HIDDEN);

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
      "Cutoff", "Discharge current", "Paused for",
      "Rated capacity", "State of health", "Discharge rate",
      "Pack + lead resistance", "Charge used", "Implied full capacity"};
  for (int i = 0; i < BR_N; i++) {
    brVal[i] = kvRow(rowsCard, BR_KEYS[i]);
    brRow[i] = lv_obj_get_parent(brVal[i]);
  }
  btSaveBtn = flatBtn(btResultBox);
  lv_obj_set_size(btSaveBtn, LV_PCT(100), 56);
  lv_obj_set_style_bg_color(btSaveBtn, COL_ACCENT, 0);
  lv_obj_set_style_bg_opa(btSaveBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btSaveBtn, 13, 0);
  btSaveLbl = lbl(btSaveBtn, LV_SYMBOL_SAVE "  Save to SD card", COL_DARKINK, F16);
  lv_obj_center(btSaveLbl);
  lv_obj_add_event_cb(btSaveBtn, [](lv_event_t *) {
    if (battSaved || !A.saveBatt) return;   // one file per result; retry on failure
    armSaveButton(btSaveBtn, btSaveLbl);
    char msg[48] = "";
    battSaved = A.saveBatt(msg, sizeof(msg));
    showSaveOutcome(btSaveBtn, btSaveLbl, battSaved, msg);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *newBtn2 = flatBtn(btResultBox);
  lv_obj_set_size(newBtn2, LV_PCT(100), 52);
  styleCard(newBtn2, COL_BLACK, COL_BORDER, 13, 0);
  lv_obj_set_style_bg_opa(newBtn2, LV_OPA_TRANSP, 0);
  lv_obj_t *nbl2 = lbl(newBtn2, "New test", COL_ACCENT2, F16); lv_obj_center(nbl2);
  lv_obj_add_event_cb(newBtn2, [](lv_event_t *) {
    btPhase = BT_IDLE; battSaved = false; refreshBatt(); refreshTestChip();
  }, LV_EVENT_CLICKED, nullptr);

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
                   : kpTarget == 5 ? "A" : kpTarget == 6 ? "mAh"
                   : kpTarget == 7 ? "A" : kpTarget == 8 ? "A" : kpTarget == 9 ? "s"
                   : kpTarget == 10 ? "mohm"
                   : modeUnit();
  lv_label_set_text(kpUnit, unit);
  lv_label_set_text(kpTitle, kpTarget == 2 ? "Fuse rating" : kpTarget == 3 ? "Wire length"
                             : kpTarget == 4 ? "Cutoff voltage" : kpTarget == 5 ? "Discharge current"
                             : kpTarget == 6 ? "Rated capacity"
                             : kpTarget == 7 ? "Sweep start current"
                             : kpTarget == 8 ? "Sweep max current (0 = auto)"
                             : kpTarget == 9 ? "Sweep duration"
                             : kpTarget == 10 ? "Lead resistance" : modeName());
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
  else if (target == 6) { if (battRatedAh > 0) { snprintf(b, sizeof(b), "%g", battRatedAh * 1000.0f); kpBuf = b; } else kpBuf = ""; }
  else if (target == 7) { snprintf(b, sizeof(b), "%g", rtStartA); kpBuf = b; }
  else if (target == 8) { if (rtMaxA > 0) { snprintf(b, sizeof(b), "%g", rtMaxA); kpBuf = b; } else kpBuf = ""; }
  else if (target == 9) { snprintf(b, sizeof(b), "%d", rtSweepS); kpBuf = b; }
  else if (target == 10) { if (probeTareOhm > 0) { snprintf(b, sizeof(b), "%g", probeTareOhm * 1000.0f); kpBuf = b; } else kpBuf = ""; }
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
  // Targets 4/5 are the battery cutoff/current: the RUNNING engine copied its
  // values at start(), so applying an edit mid-discharge would update the UI
  // (and NVS) while the test silently keeps the old threshold — the display
  // would promise a cutoff the engine will not honor. Refuse, same as target 1.
  if (kpTarget == 1) { if (!engineBusy()) { setpoint = v; if (A.setSetpoint) A.setSetpoint(v); } refreshAdjust(); refreshMonitor(); }
  else if (kpTarget == 3) { estWireLen = v < 0 ? 0 : v > 100 ? 100 : v; refreshRtest(); }
  else if (kpTarget == 4) { if (!engineBusy()) { battCutoff = v < 0.1f ? 0.1f : v > 60 ? 60 : v; battCutoffCustom = true; } refreshBatt(); refreshMonitor(); }
  // A current typed by hand overrides the C-rate chips — the user has said what
  // they want in amps, so the current must stop chasing the rated capacity.
  else if (kpTarget == 5) { if (!engineBusy()) { battAmps = v < 0.01f ? 0.01f : v > 12 ? 12 : v; battCRateIdx = -1; } refreshBatt(); }
  // Rated capacity in mAh, stored in Ah. 0 clears it (metrics go back to hidden);
  // capped at 999 Ah, well past anything a 12 A load will ever finish. Telling
  // the controller the pack's size is all it needs to work the test current out,
  // so a selected rate is re-applied against the new capacity immediately.
  else if (kpTarget == 6) {
    if (!engineBusy()) {
      battRatedAh = v <= 0 ? 0 : (v > 999000 ? 999.0f : v / 1000.0f);
      applyCRate();
    }
    refreshBatt();
  }
  // Sweep shape. Clamped to the EL15's own 12 A ceiling here; the engine applies
  // the fuse/power caps too, once it knows the source voltage.
  else if (kpTarget == 7) { if (!engineBusy()) rtStartA = v < 0 ? 0 : (v > 12 ? 12 : v); refreshRtest(); }
  else if (kpTarget == 8) { if (!engineBusy()) rtMaxA = v <= 0 ? 0 : (v > 12 ? 12 : v); refreshRtest(); }
  else if (kpTarget == 9) {
    if (!engineBusy()) {
      int s = (int)(v + 0.5f);
      rtSweepS = s < (int)ResistanceTest::MIN_SWEEP_S ? (int)ResistanceTest::MIN_SWEEP_S
               : s > (int)ResistanceTest::MAX_SWEEP_S ? (int)ResistanceTest::MAX_SWEEP_S : s;
    }
    refreshRtest();
  }
  // Lead resistance, typed in milliohms. Capped at 1 ohm: this stands in for test
  // leads and clips, and a figure past that is a typo, not a lead — one that
  // would inflate every voltage on the device by volts.
  else if (kpTarget == 10) {
    if (!engineBusy()) {
      probeTareOhm = v <= 0 ? 0 : (v > 1000.0f ? 1.0f : v / 1000.0f);
      persistProbe();
    }
    refreshProbe();
    refreshRtest();
    refreshMonitor();
  }
  else { fuseRating = v; refreshRtest(); refreshMonitor(); }
  showOverlay(OV_NONE);
}
static void onKey(lv_event_t *e) { kpPress((const char *)lv_event_get_user_data(e)); }
static void onPreset(lv_event_t *e) {
  int cents = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  char b[16]; snprintf(b, sizeof(b), "%g", cents / 100.0f); kpBuf = b; kpRefresh();
}
// ---- Text-entry overlay (Wi-Fi credentials) --------------------------------
// LVGL's keyboard, in its own modal overlay. Only used for text that cannot be
// picked from a list — today the Wi-Fi SSID and password.
static void openTextEntry(int target) {
  kbTarget = target;
  const prefs::Data &p = prefs::get();
  lv_label_set_text(kbTitle, target == KB_SSID ? "Wi-Fi network name" : "Wi-Fi password");
  lv_textarea_set_password_mode(kbTextArea, target == KB_PASS);
  lv_textarea_set_max_length(kbTextArea, target == KB_SSID ? 32 : 64);
  lv_textarea_set_text(kbTextArea, target == KB_SSID ? p.ssid : p.pass);
  showOverlay(OV_TEXT);
}

static void commitTextEntry() {
  const char *txt = lv_textarea_get_text(kbTextArea);
  if (kbTarget == KB_SSID)
    prefs::change([txt](prefs::Data &d) { snprintf(d.ssid, sizeof(d.ssid), "%s", txt); });
  else
    prefs::change([txt](prefs::Data &d) { snprintf(d.pass, sizeof(d.pass), "%s", txt); });
  prefs::flush();   // credentials are worth committing immediately
  showOverlay(OV_NONE);
  refreshWifi();
}

static void buildTextEntry() {
  kbOverlay = cont(lv_layer_top());
  lv_obj_add_flag(kbOverlay, LV_OBJ_FLAG_CLICKABLE);   // modal barrier
  lv_obj_set_size(kbOverlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(kbOverlay, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(kbOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(kbOverlay, 10, 0);
  lv_obj_set_flex_flow(kbOverlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(kbOverlay, 8, 0);
  lv_obj_add_flag(kbOverlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *hdr = cont(kbOverlay);
  lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 8, 0);
  lv_obj_t *x = flatBtn(hdr);
  lv_obj_set_size(x, 40, 40);
  lv_obj_set_ext_click_area(x, 12);
  styleCard(x, COL_CARD, COL_BORDER, 10, 0);
  lv_obj_t *xl = lbl(x, LV_SYMBOL_CLOSE, COL_MUTED, F16);
  lv_obj_center(xl);
  lv_obj_add_event_cb(x, [](lv_event_t *) { showOverlay(OV_NONE); }, LV_EVENT_CLICKED, nullptr);
  kbTitle = lbl(hdr, "", COL_INK, F16);

  kbTextArea = lv_textarea_create(kbOverlay);
  lv_obj_set_width(kbTextArea, LV_PCT(100));
  lv_textarea_set_one_line(kbTextArea, true);
  lv_obj_set_style_bg_color(kbTextArea, COL_READOUT, 0);
  lv_obj_set_style_border_color(kbTextArea, COL_BORDER, 0);
  lv_obj_set_style_text_color(kbTextArea, COL_INK, 0);

  lv_obj_t *kb = lv_keyboard_create(kbOverlay);
  lv_obj_set_width(kb, LV_PCT(100));
  lv_obj_set_flex_grow(kb, 1);
  lv_keyboard_set_textarea(kb, kbTextArea);
  // The keyboard's own OK/close keys are the commit and cancel actions.
  lv_obj_add_event_cb(kb, [](lv_event_t *) { commitTextEntry(); }, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(kb, [](lv_event_t *) { showOverlay(OV_NONE); }, LV_EVENT_CANCEL, nullptr);
}

// ---- Wi-Fi network picker overlay ------------------------------------------
// Scan (via net::), list the SSIDs, pick one. Replaces typing the network name;
// the password still needs the keyboard, so picking a network opens it next.
static void onWifiRowPicked(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= wifiCount) return;
  prefs::change([i](prefs::Data &d) { snprintf(d.ssid, sizeof(d.ssid), "%s", wifiNames[i]); });
  prefs::flush();
  refreshWifi();
  openTextEntry(KB_PASS);   // a network is no use without its password
}

static void startWifiScan() {
  // Radio shares the antenna with the BLE link to the load; block while an
  // engine OR a manually-energised load is sinking (same rule as the sync).
  if (engineBusy() || lastLoadOn) {
    lv_label_set_text(setNetStatus, "Not while the load is on.");
    lv_obj_set_style_text_color(setNetStatus, COL_AMBER, 0);
    return;
  }
  if (!A.scanWifi) return;
  // Clear the previous list and show the scanning state before the overlay is
  // shown, so it never flashes stale results.
  lv_obj_clean(wifiList);
  wifiCount = 0;
  lv_label_set_text(wifiStatus, "Scanning...");
  lv_obj_clear_flag(wifiStatus, LV_OBJ_FLAG_HIDDEN);
  showOverlay(OV_WIFI);
  // Free ~70 KB for the Wi-Fi stack (no PSRAM on this board) before it inits.
  // Painted the overlay above first; low-mem mode re-renders it in small chunks.
  display::setLowMemMode(true);
  if (!A.scanWifi()) {   // radio busy (a sync is mid-flight)
    lv_label_set_text(wifiStatus, "Wi-Fi is busy - try again in a moment.");
    display::setLowMemMode(false);
  }
}

void onWifiScanResult(const char *const *ssids, const int *rssi, int n, const char *err) {
  // The scan is done and the radio is off again — restore the full draw buffer
  // so the list (and the rest of the UI) renders at full speed while the user
  // picks and types.
  display::setLowMemMode(false);
  // A scan the user has already navigated away from should not repaint anything.
  if (curOverlay != OV_WIFI) return;
  lv_obj_clean(wifiList);
  wifiCount = 0;
  if (err) {
    lv_label_set_text(wifiStatus, err);
    lv_obj_set_style_text_color(wifiStatus, COL_AMBER, 0);
    lv_obj_clear_flag(wifiStatus, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (n <= 0) {
    lv_label_set_text(wifiStatus, "No networks found. Tap Rescan, or enter a hidden one manually.");
    lv_obj_set_style_text_color(wifiStatus, COL_MUTED, 0);
    lv_obj_clear_flag(wifiStatus, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(wifiStatus, LV_OBJ_FLAG_HIDDEN);
  if (n > WIFI_MAX) n = WIFI_MAX;
  wifiCount = n;
  const char *sel = prefs::get().ssid;
  for (int i = 0; i < n; i++) {
    snprintf(wifiNames[i], sizeof(wifiNames[0]), "%s", ssids[i]);
    lv_obj_t *row = flatBtn(wifiList);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    styleCard(row, COL_CARD, COL_BORDER, 11, 12);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    bool current = strcmp(wifiNames[i], sel) == 0;
    lv_obj_t *nm = lbl(row, wifiNames[i], current ? COL_ACCENT2 : COL_INK, F16);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, LV_PCT(72));
    // RSSI → 4 signal bars. Rough but conventional thresholds (dBm).
    int r = rssi[i];
    int bars = r >= -55 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : r >= -85 ? 1 : 0;
    char sig[24];
    snprintf(sig, sizeof(sig), LV_SYMBOL_WIFI " %d/4", bars);
    lbl(row, sig, bars >= 2 ? COL_GREEN : COL_AMBER, F14);
    lv_obj_add_event_cb(row, onWifiRowPicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}

static void buildWifiPicker() {
  wifiOverlay = cont(lv_layer_top());
  lv_obj_add_flag(wifiOverlay, LV_OBJ_FLAG_CLICKABLE);   // modal barrier
  lv_obj_set_size(wifiOverlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(wifiOverlay, COL_BLACK, 0);
  lv_obj_set_style_bg_opa(wifiOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(wifiOverlay, 16, 0);
  lv_obj_set_flex_flow(wifiOverlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(wifiOverlay, 8, 0);
  lv_obj_add_flag(wifiOverlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *hdr = cont(wifiOverlay);
  lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 8, 0);
  lv_obj_t *x = flatBtn(hdr);
  lv_obj_set_size(x, 40, 40);
  lv_obj_set_ext_click_area(x, 12);
  styleCard(x, COL_CARD, COL_BORDER, 10, 0);
  lv_obj_t *xl = lbl(x, LV_SYMBOL_CLOSE, COL_MUTED, F16);
  lv_obj_center(xl);
  lv_obj_add_event_cb(x, [](lv_event_t *) { showOverlay(OV_NONE); }, LV_EVENT_CLICKED, nullptr);
  lbl(hdr, "Wi-Fi networks", COL_INK, F16);

  wifiStatus = lbl(wifiOverlay, "", COL_MUTED, F14);
  lv_label_set_long_mode(wifiStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(wifiStatus, LV_PCT(100));

  wifiList = cont(wifiOverlay);
  lv_obj_set_width(wifiList, LV_PCT(100));
  lv_obj_set_flex_grow(wifiList, 1);
  lv_obj_set_flex_flow(wifiList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(wifiList, 8, 0);
  lv_obj_add_flag(wifiList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(wifiList, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *foot = cont(wifiOverlay);
  lv_obj_set_size(foot, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(foot, 8, 0);
  lv_obj_t *rescan = flatBtn(foot);
  lv_obj_set_flex_grow(rescan, 1);
  lv_obj_set_height(rescan, 46);
  styleCard(rescan, COL_INSET, COL_BORDER, 11, 0);
  lv_obj_t *rsl = lbl(rescan, LV_SYMBOL_REFRESH "  Rescan", COL_ACCENT2, F14);
  lv_obj_center(rsl);
  lv_obj_add_event_cb(rescan, [](lv_event_t *) { startWifiScan(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *manual = flatBtn(foot);
  lv_obj_set_flex_grow(manual, 1);
  lv_obj_set_height(manual, 46);
  styleCard(manual, COL_INSET, COL_BORDER, 11, 0);
  lv_obj_t *ml = lbl(manual, "Hidden / manual", COL_MUTED, F14);
  lv_obj_center(ml);
  lv_obj_add_event_cb(manual, [](lv_event_t *) { openTextEntry(KB_SSID); }, LV_EVENT_CLICKED, nullptr);
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
  // A mode change mid-test would re-regulate the load out from under the
  // engine (both engines drive CC; CV with a stale target on a charged battery
  // runs current to the 12 A limit). Same gate as the load bar: stop the test
  // first. The picker itself stays reachable — this refuses only the commit.
  if (engineBusy()) { showOverlay(OV_NONE); return; }
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
  // Probe wiring, on the voltage caption. Only says something when the reading
  // is NOT the load's own number: 4-wire means it was sensed at the part, and a
  // lead correction means we added the drop back. Plain "VOLTAGE" therefore
  // always means "exactly what the EL15 reports".
  if (vHeroCap) {
    char cap[28];
    if (probeFourWire) snprintf(cap, sizeof(cap), "VOLTAGE - 4-WIRE");
    else if (probeTareOhm > 0) snprintf(cap, sizeof(cap), "VOLTAGE - LEAD-CORRECTED");
    else snprintf(cap, sizeof(cap), "VOLTAGE");
    setTextIf(vHeroCap, cap);
  }
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
  // Mode sync — but NOT while a protection warning is active: the warn flag
  // masks mode bits 1+2, so at the byte level CAP|fault reads as CC and
  // DCR|fault reads as CV (real-device behavior; see DM40GUI's
  // _apply_status_buttons, which holds the last good mode for the same reason).
  // Syncing during a fault would flap the mode chip/units to the aliased mode
  // and back once the fault clears.
  if (!isRT() && !isBatt() && !s.warning[0] && curMode != s.mode) {
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
      if (c.fixedCells) {
        // Nothing to suggest when the pack size is fixed — but the per-cell
        // figure still tells the user whether they hooked up what they think.
        snprintf(vb, sizeof(vb), "Voc %.2f V - %.2f V/cell", s.voltage, perCell);
      } else {
        int sug = (int)(s.voltage / c.nom + 0.5f);
        if (sug < 1) sug = 1;
        if (sug > c.maxCells) sug = c.maxCells;
        snprintf(vb, sizeof(vb), "Voc %.2f V - %.2f V/cell (looks like %dS)", s.voltage, perCell, sug);
      }
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

  // fault banner (flags toggled only on a visibility change). A supervisor
  // warning outranks everything else on screen while it is live, so leave the
  // banner alone until it stands down.
  if (s.warning[0] && !faultLocked) {
    faultIsEmergency = false;   // a real protection trip supersedes an e-stop ack
    guardAction = nullptr;
    lv_obj_set_style_bg_color(faultBanner, COL_RED, 0);
    snprintf(b, sizeof(b), LV_SYMBOL_WARNING "  %s - PROTECTION", s.warning); setTextIf(faultTitle, b);
    setTextIf(faultMsg, "Load protection tripped - check the setup");
    if (lv_obj_has_flag(faultBanner, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(faultBanner);   // above any overlay on the top layer
      audio::fault();   // alarm only on the transition into a new fault
    }
  } else if (!s.warning[0] && !faultIsEmergency && !faultLocked && !guardAction &&
             !lv_obj_has_flag(faultBanner, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  }

  // running r-test live values
  if (rtPhase == RT_RUN) {
    snprintf(b, sizeof(b), "%.2f V", s.voltage); setTextIf(runVLbl, b);
    snprintf(b, sizeof(b), "%.3f A", s.current); setTextIf(runILbl, b);
  }

  refreshMonitor();
}

// ---- Link-loss supervisor / crash recovery ---------------------------------
void onGuardAlert(const char *title, const char *msg, bool resolved) {
  // A live warning must not be dismissable by a stray tap, and must not be
  // hidden behind a blanked screen.
  guardAction = nullptr;
  faultIsEmergency = !resolved;
  faultLocked = !resolved;
  display::noteActivity();
  lv_obj_set_style_bg_color(faultBanner, resolved ? COL_GREEN : COL_RED, 0);
  char t[64];
  snprintf(t, sizeof(t), "%s  %s", resolved ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, title);
  lv_label_set_text(faultTitle, t);
  lv_label_set_text(faultMsg, msg);
  lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(faultBanner);
  // The supervisor blocks for seconds inside a reconnect attempt, so this has
  // to reach the panel before we return to it.
  lv_refr_now(nullptr);
}

void offerRecovery(const char *msg, std::function<void()> action) {
  guardAction = action;
  faultIsEmergency = true;
  faultLocked = false;   // tapping runs the action rather than dismissing
  display::noteActivity();
  lv_obj_set_style_bg_color(faultBanner, COL_AMBER, 0);
  lv_label_set_text(faultTitle, LV_SYMBOL_WARNING "  LOAD MAY STILL BE ON");
  lv_label_set_text(faultMsg, msg);
  lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(faultBanner);
}

void onNetProgress(int state, const char *text) {
  // net::State — 3 DONE, 4 FAILED, 5 SCANNING (scan uses onWifiScanResult for
  // its own low-mem handling, so ignore SCANNING here).
  const bool done = state == 3, failed = state == 4, scanning = state == 5;
  if (scanning) return;
  lv_label_set_text(setNetStatus, text);
  lv_obj_set_style_text_color(setNetStatus, failed ? COL_RED : done ? COL_GREEN : COL_MUTED, 0);
  lv_label_set_text(setSyncLbl, (done || failed) ? "Sync clock now" : "Syncing...");
  if (done) {
    // The clock is in the RTC and every setting is in NVS, so a restart loses
    // nothing — and it is the only way to get the full-speed draw buffer back
    // (Wi-Fi fragmented the heap; the 82 KB block can't be reassembled). Reboot
    // rather than leave the UI on the reduced buffer.
    settingsTick();   // reflect the new RTC time for the moment it's visible
    lv_label_set_text(setNetStatus, "Clock set - restarting to restore the display...");
    lv_obj_set_style_text_color(setNetStatus, COL_GREEN, 0);
    lv_refr_now(nullptr);
    prefs::flush();
    delay(1600);      // let the confirmation be read
    ESP.restart();
  }
  if (failed) display::setLowMemMode(false);   // restore (best-effort) so a retry is usable
}

// The user has taken manual control (scan / connect): any stale guard banner —
// a locked mid-recovery warning or a failed-recovery retry offer — is now
// superseded by their own actions and must not linger (a permanently locked
// banner also suppressed all later protection alerts via the faultLocked gate).
void clearGuardBanner() {
  guardAction = nullptr;
  faultLocked = false;
  faultIsEmergency = false;
  lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
}

void onPowerCritical(int percent, bool wasHot) {
  // Full-width amber banner; not locked (the load is already off), so a tap
  // dismisses it. Reuses the fault banner on the top layer so it shows even
  // over an open overlay.
  guardAction = nullptr;
  faultLocked = false;
  faultIsEmergency = true;   // keep it up across clean status packets
  display::noteActivity();
  lv_obj_set_style_bg_color(faultBanner, COL_AMBER, 0);
  lv_label_set_text(faultTitle, LV_SYMBOL_WARNING "  CONTROLLER BATTERY CRITICAL");
  char b[96];
  snprintf(b, sizeof(b), "%d%% - the load was stopped so the controller can't die mid-test. Plug in USB. Tap to dismiss.",
           percent);
  lv_label_set_text(faultMsg, b);
  lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(faultBanner);
  if (rtPhase == RT_RUN) { rtPhase = RT_IDLE; refreshRtest(); }
  if (btPhase == BT_RUN || btPhase == BT_REST) { btPhase = BT_IDLE; refreshBatt(); }
}

void onPoweringOff() {
  guardAction = nullptr;
  faultLocked = false;
  lv_obj_set_style_bg_color(faultBanner, COL_RED, 0);
  lv_label_set_text(faultTitle, LV_SYMBOL_POWER "  POWERING OFF");
  lv_label_set_text(faultMsg, "Load stopped. Cutting power...");
  lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(faultBanner);
  lv_refr_now(nullptr);   // paint before main.cpp cuts the rails
}

void onEmergencyStop(bool wasTestRunning) {
  // A stale recovery offer must not hijack the acknowledgement tap.
  guardAction = nullptr;
  // While the guard is mid-recovery or failed, the link is down — our LOAD_OFF
  // write went into a dead link and stopped nothing. The guard's "LOAD MAY
  // STILL BE ON" banner is the truth; do not overwrite it with a false ack.
  if (faultLocked) {
    if (rtPhase == RT_RUN) { rtPhase = RT_IDLE; refreshRtest(); }
    if (btPhase == BT_RUN || btPhase == BT_REST) { btPhase = BT_IDLE; refreshBatt(); }
    return;
  }
  // Reuse the full-width fault banner as the acknowledgement; it stays up until
  // tapped (faultIsEmergency guards onStatus from auto-hiding it).
  faultIsEmergency = true;
  lv_obj_set_style_bg_color(faultBanner, COL_RED, 0);  // guard paths recolor it
  lv_label_set_text(faultTitle, LV_SYMBOL_WARNING "  EMERGENCY STOP");
  // Honest wording: with no link, the stop command reached nothing.
  lv_label_set_text(faultMsg,
      !connected     ? "Not connected - the stop could NOT reach the load. Check the device. Tap to dismiss."
      : wasTestRunning ? "Test aborted - load forced off. Tap to dismiss."
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
    // A stale warning from the dropped session would otherwise keep gating the
    // load button after the next connect, until the first clean packet lands.
    lastStatus.warning[0] = '\0';
    lastLoadOn = false;
    // A drop mid-sweep stops the engine without a callback; unstick the UI.
    if (rtPhase == RT_RUN) { rtPhase = RT_IDLE; refreshRtest(); }
  }
  // info bar visibility depends on connection
  if (connected && (curScreen == SCR_MON || curScreen == SCR_ADJ || curScreen == SCR_GRAPH))
    lv_obj_clear_flag(infoBar, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(infoBar, LV_OBJ_FLAG_HIDDEN);
  // A manual connect blocks the loop task inside NimBLE for up to ~20 s on an
  // absent peer; nothing repaints until it returns. Push the "Connecting"
  // chrome to the panel NOW, exactly like every other blocking path does
  // (SD saves, Wi-Fi sync), so the user sees progress instead of a freeze.
  if (state == 2 /* CONNECTING */) lv_refr_now(nullptr);
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

void onTestProgress(float elapsedS, float totalS, float target,
                    float v, float i, float r, bool rValid) {
  if (rtPhase != RT_RUN) { rtPhase = RT_RUN; refreshRtest(); }
  char b[40];
  rtLastElapsed = elapsedS;
  rtTotalS = totalS > 0 ? totalS : rtTotalS;
  // Which half of the triangle we are on is worth showing: it tells the user
  // whether the peak has passed and roughly how long is left.
  bool rising = totalS <= 0 || elapsedS < totalS / 2;
  setTextIf(runStepLbl, rtTareRunning ? "TARE SWEEP"
                        : rising ? LV_SYMBOL_UP " RAMPING UP" : LV_SYMBOL_DOWN " RAMPING DOWN");
  snprintf(b, sizeof(b), "%.1f / %.0f s", elapsedS, rtTotalS);
  setTextIf(rtRunClock, b);
  snprintf(rtStepText, sizeof(rtStepText), "%.0fs", rtTotalS - elapsedS);
  refreshTestChip();
  lv_bar_set_value(runBar, rtTotalS > 0 ? (int)(elapsedS * 100 / rtTotalS) : 0, LV_ANIM_OFF);
  snprintf(b, sizeof(b), "%.2f", v); setTextIf(runVLbl, b);
  snprintf(b, sizeof(b), "%.3f A", i); setTextIf(runILbl, b);
  snprintf(b, sizeof(b), "target %.2f A", target); setTextIf(runTargetLbl, b);
  if (rValid) {
    char ob[20];
    fmtOhm(ob, sizeof(ob), r);
    snprintf(b, sizeof(b), "R %s", ob);
    setTextIf(runRLbl, b);
    lv_obj_set_style_text_color(runRLbl, COL_ACCENT2, 0);
    rtHaveR = true;
  } else {
    setTextIf(runRLbl, "R --");
    lv_obj_set_style_text_color(runRLbl, COL_FAINT, 0);
  }
  rtLivePush(v, i, rValid ? r : 0);
  if (curScreen == SCR_RTEST) rtLiveRefresh();
}
void onTestComplete(const ResistanceTest::Result &r) {
  // A tare run isn't a result — it is a calibration of the leads. Store the
  // RAW slope (nothing is subtracted from a tare) and go straight back to setup.
  if (rtTareRunning) {
    rtTareRunning = false;
    probeTareOhm = r.rawResistanceOhm;
    rtPhase = RT_IDLE;
    char t[96], ob[20];
    fmtOhm(ob, sizeof(ob), probeTareOhm);
    snprintf(t, sizeof(t), "Lead tare stored: %s. Now corrected for in every mode.", ob);
    lv_label_set_text(rtStatusLbl, t);
    lv_obj_clear_flag(rtStatusLbl, LV_OBJ_FLAG_HIDDEN);
    persistProbe();   // main.cpp reads it per packet, so it must land now
    refreshProbe();
    refreshRtest();
    refreshMonitor();
    showScreen(SCR_RTEST);
    return;
  }
  lastResult = r; rtPhase = RT_RESULT; rtSaved = false;
  char b[32];
  if (r.resistanceOhm < 1.0f) snprintf(b, sizeof(b), "%.4f ohm", r.resistanceOhm);
  else snprintf(b, sizeof(b), "%.3f ohm", r.resistanceOhm);
  lv_label_set_text(resistVal, b);
  if (r.reliable) lv_obj_add_flag(lowConfBox, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(lowConfBox, LV_OBJ_FLAG_HIDDEN);
  char v1[32];
  int n = (int)r.samples.size();
  if (n > RT_CHART_PTS) n = RT_CHART_PTS;  // engine bins to RT_CHART_PTS; belt & braces
  float iHi = n ? r.samples.back().current : 0;

  snprintf(v1, sizeof(v1), "%.2f V", r.openCircuitVoltage); lv_label_set_text(rrVal[RR_VOC], v1);
  // Probe wiring, and — when a tare was subtracted — what was actually measured
  // before it. Hiding the raw figure would make the correction invisible.
  char probe[40];
  if (r.fourWire) strcpy(probe, "4-wire (Kelvin)");
  else if (r.tareOhm > 0) { char ob[20]; fmtOhm(ob, sizeof(ob), r.tareOhm); snprintf(probe, sizeof(probe), "2-wire, tare %s", ob); }
  else strcpy(probe, "2-wire, no tare");
  lv_label_set_text(rrVal[RR_PROBE], probe);
  lv_obj_set_style_text_color(rrVal[RR_PROBE], r.fourWire || r.tareOhm > 0 ? COL_GREEN : COL_AMBER, 0);
  if (r.tareOhm > 0) {
    fmtOhm(v1, sizeof(v1), r.rawResistanceOhm); lv_label_set_text(rrVal[RR_RAW], v1);
    lv_obj_clear_flag(rrRow[RR_RAW], LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(rrRow[RR_RAW], LV_OBJ_FLAG_HIDDEN);
  }
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
  // Run statistics now come straight from the Result: the engine tracked them
  // over every raw sample, which is strictly better than re-deriving them from
  // the binned curve.
  snprintf(v1, sizeof(v1), "%.2f V (%.1f%%)", r.sagV,
           r.openCircuitVoltage > 0.01f ? r.sagV * 100.0f / r.openCircuitVoltage : 0.0f);
  lv_label_set_text(rrVal[RR_SAG], v1);
  snprintf(v1, sizeof(v1), "%.1f W", r.peakPowerW); lv_label_set_text(rrVal[RR_PKW], v1);
  snprintf(v1, sizeof(v1), "%.1f -> %.1f\xC2\xB0" "C", r.tempMin, r.tempMax);
  lv_label_set_text(rrVal[RR_TEMP], v1);
  int fanPct = (r.maxFan > el15::FAN_SPEED_MAX ? el15::FAN_SPEED_MAX : r.maxFan) * 100 / el15::FAN_SPEED_MAX;
  snprintf(v1, sizeof(v1), "%d%%", fanPct); lv_label_set_text(rrVal[RR_FAN], v1);
  // Sweep as MEASURED, not as commanded — the load's regulation means the two
  // differ slightly, and what was actually drawn is the honest figure.
  snprintf(v1, sizeof(v1), "%.2f-%.2f A in %lus", r.minCurrent, r.maxCurrentSeen,
           (unsigned long)r.sweepSeconds);
  lv_label_set_text(rrVal[RR_SWEEP], v1);
  snprintf(v1, sizeof(v1), "%d pts / %d bands", r.rawSamples, n);
  lv_label_set_text(rrVal[RR_STEPS], v1);
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
    char rv[40];
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
  refreshRtest();
  showScreen(SCR_RTEST);
  refreshTestChip();
  autoSave(saveBtn, saveBtnLbl, rtSaved, A.saveRTest);
}
void onTestError(const char *msg) {
  rtTareRunning = false;   // a failed tare sweep leaves the old tare in place
  rtPhase = RT_IDLE; refreshRtest();
  lv_label_set_text(rtStatusLbl, msg);
  lv_obj_clear_flag(rtStatusLbl, LV_OBJ_FLAG_HIDDEN);
  refreshTestChip();
}

void onBattProgress(float v, float i, float ah, float wh, float temp, uint32_t elapsedS, int phase) {
  if (btPhase != BT_RUN && btPhase != BT_REST) { btPhase = BT_RUN; refreshBatt(); }
  if (phase == 2 && btPhase == BT_RUN) { btPhase = BT_REST; refreshBatt(); }
  setTextIf(btPhaseLbl, phase == 3 ? "PAUSED - load off"
                        : phase == 2 ? "RESTING - load off" : "DISCHARGING");
  lv_obj_set_style_text_color(btPhaseLbl, phase == 3 ? COL_AMBER : COL_ACCENT, 0);
  char b[160];   // the charge/ETA/progress line is the long one
  char el[16];
  hhmmss((int)elapsedS, el, sizeof(el));
  setTextIf(btElapsedLbl, el);
  snprintf(b, sizeof(b), "%.2f", v); setTextIf(btVLbl, b);
  snprintf(b, sizeof(b), "%.3f A", i); setTextIf(btILbl, b);
  snprintf(b, sizeof(b), "%.3f Ah", ah); setTextIf(btAhLbl, b);
  snprintf(b, sizeof(b), "%.1f Wh", wh); setTextIf(btWhLbl, b);
  snprintf(b, sizeof(b), "%.1f\xC2\xB0" "C", temp); setTextIf(btTempLbl, b);

  // Where the run stands: state of charge read off the chemistry's discharge
  // curve, the time left before the CUTOFF (not before the rating — the cutoff
  // is what actually ends the test), and progress against the nameplate when one
  // was entered. Each part is shown only when it is actually known, and the SoC
  // carries a "~" because a curve for the family is not a curve for your cell.
  float socPct = (phase == 1 && A.battSocPct) ? A.battSocPct() : -1;
  uint32_t left = (phase == 1 && A.battRemainingS) ? A.battRemainingS() : 0;
  bool fromCurve = A.battEtaFromCurve && A.battEtaFromCurve();
  if (socPct >= 0 || left > 0 || battRatedAh > 0) {
    int n = 0;
    if (socPct >= 0) n += snprintf(b + n, sizeof(b) - n, "~%.0f%% charge left", socPct);
    if (left > 0) {
      char rem[16];
      hhmmss((int)left, rem, sizeof(rem));
      n += snprintf(b + n, sizeof(b) - n, "%sapprox %s to cutoff%s",
                    n ? "  -  " : "", rem, fromCurve ? "" : " (from the rating)");
    }
    if (battRatedAh > 0 && n < (int)sizeof(b))
      snprintf(b + n, sizeof(b) - n, "%s%.0f%% of %.0f mAh drawn",
               n ? "\n" : "", ah / battRatedAh * 100.0f, battRatedAh * 1000.0f);
    setTextIf(btEtaLbl, b);
    lv_obj_clear_flag(btEtaLbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(btEtaLbl, LV_OBJ_FLAG_HIDDEN);
  }

  if (phase == 1) {
    btLastElapsed = elapsedS;
    battHistPush(v);
    if (curScreen == SCR_BATT) battChartRefresh();
  }
  refreshTestChip();
}

void onBattPaused(bool paused, const char *reason) {
  battPausedFlag = paused;
  snprintf(battPauseWhy, sizeof(battPauseWhy), "%s", reason ? reason : "");
  // A pause is a state the user must not miss — it means their test is sitting
  // there not progressing — so wake the screen for it.
  if (paused) display::noteActivity();
  refreshBatt();
  refreshTestChip();
}

void onBattComplete(const CapacityTest::Result &r) {
  lastBatt = r;
  btPhase = BT_RESULT;
  battSaved = false;
  battPausedFlag = false;
  char b[48];
  snprintf(b, sizeof(b), "%.3f Ah", r.capacityAh); lv_label_set_text(btAhBig, b);
  snprintf(b, sizeof(b), "%.0f mAh  -  %.1f Wh  -  avg %.2f V",
           r.capacityAh * 1000.0f, r.energyWh, r.avgV);
  lv_label_set_text(btWhSub, b);
  char el[16];
  hhmmss((int)r.durationS, el, sizeof(el));
  lv_label_set_text(brVal[BR_DUR], el);
  lv_label_set_text(brVal[BR_REASON], r.stopReason);
  snprintf(b, sizeof(b), "%.2f V", r.startV); lv_label_set_text(brVal[BR_STARTV], b);
  snprintf(b, sizeof(b), "%.2f V", r.endV); lv_label_set_text(brVal[BR_ENDV], b);
  snprintf(b, sizeof(b), "%.2f V", r.reboundV); lv_label_set_text(brVal[BR_REBOUND], b);
  snprintf(b, sizeof(b), "%.2f V", r.avgV); lv_label_set_text(brVal[BR_AVGV], b);
  snprintf(b, sizeof(b), "%.3f A", r.avgI); lv_label_set_text(brVal[BR_AVGI], b);
  snprintf(b, sizeof(b), "%.1f - %.1f\xC2\xB0" "C", r.minTemp, r.maxTemp); lv_label_set_text(brVal[BR_TEMP], b);
  snprintf(b, sizeof(b), "%.2f V", r.cutoffV); lv_label_set_text(brVal[BR_CUTOFF], b);
  snprintf(b, sizeof(b), "%.2f A", r.currentA); lv_label_set_text(brVal[BR_CURRENT], b);
  // "Paused for" only earns a row when it actually happened.
  if (r.pausedS > 0) {
    char pel[16];
    hhmmss((int)r.pausedS, pel, sizeof(pel));
    lv_label_set_text(brVal[BR_PAUSED], pel);
    lv_obj_clear_flag(brRow[BR_PAUSED], LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(brRow[BR_PAUSED], LV_OBJ_FLAG_HIDDEN);
  }
  // Rating-derived rows. State of health is coloured against the usual
  // end-of-life convention: >=80 % healthy, 60-80 % worn, below that failed.
  bool rated = r.ratedAh > 0;
  for (int i = BR_RATED; i <= BR_CRATE; i++) {
    if (rated) lv_obj_clear_flag(brRow[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(brRow[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (rated) {
    snprintf(b, sizeof(b), "%.0f mAh", r.ratedAh * 1000.0f); lv_label_set_text(brVal[BR_RATED], b);
    snprintf(b, sizeof(b), "%.0f%%", r.sohPct); lv_label_set_text(brVal[BR_SOH], b);
    lv_obj_set_style_text_color(brVal[BR_SOH],
        r.sohPct >= 80 ? COL_GREEN : r.sohPct >= 60 ? COL_AMBER : COL_RED, 0);
    snprintf(b, sizeof(b), "%.2fC", r.cRate); lv_label_set_text(brVal[BR_CRATE], b);
  }
  // Battery-model rows, each shown only if the run actually established it.
  if (r.internalResistanceOhm > 0) {
    fmtOhm(b, sizeof(b), r.internalResistanceOhm);   // ASCII "mohm" — Montserrat has no omega
    lv_label_set_text(brVal[BR_IR], b);
    // What that resistance actually covers depends on the probe wiring: with the
    // leads corrected out (4-wire, or a 2-wire tare) the sag measures the pack
    // alone; with no correction it is the pack plus everything in series with it.
    lv_label_set_text(lv_obj_get_child(brRow[BR_IR], 0),
                      (probeFourWire || probeTareOhm > 0) ? "Pack resistance"
                                                          : "Pack + lead resistance");
    lv_obj_clear_flag(brRow[BR_IR], LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(brRow[BR_IR], LV_OBJ_FLAG_HIDDEN);
  }
  // The span of charge the run covered. A partial discharge is exactly when the
  // headline Ah understates the pack, so say so rather than let the state-of-
  // health row be read as a verdict on a run that started at 60 %.
  if (r.startSocPct >= 0 && r.endSocPct >= 0) {
    snprintf(b, sizeof(b), "~%.0f%% -> ~%.0f%%", r.startSocPct, r.endSocPct);
    lv_label_set_text(brVal[BR_SOCSPAN], b);
    lv_obj_set_style_text_color(brVal[BR_SOCSPAN],
        r.startSocPct >= 90 ? COL_ACCENT2 : COL_AMBER, 0);
    lv_obj_clear_flag(brRow[BR_SOCSPAN], LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(brRow[BR_SOCSPAN], LV_OBJ_FLAG_HIDDEN);
  }
  if (r.impliedFullAh > 0) {
    snprintf(b, sizeof(b), "~%.0f mAh", r.impliedFullAh * 1000.0f);
    lv_label_set_text(brVal[BR_IMPLIED], b);
    lv_obj_clear_flag(brRow[BR_IMPLIED], LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(brRow[BR_IMPLIED], LV_OBJ_FLAG_HIDDEN);
  }
  battChartRefresh();
  refreshBatt();
  showScreen(SCR_BATT);
  refreshTestChip();
  autoSave(btSaveBtn, btSaveLbl, battSaved, A.saveBatt);
}

void onBattError(const char *msg) {
  btPhase = BT_IDLE;
  battPausedFlag = false;
  refreshBatt();
  lv_label_set_text(btStatusLbl, msg);
  lv_obj_clear_flag(btStatusLbl, LV_OBJ_FLAG_HIDDEN);
  showScreen(SCR_BATT);
  refreshTestChip();
}

// ---- Entry -----------------------------------------------------------------
void begin(const UiActions &actions) {
  A = actions;

  // Restore the last session's setup BEFORE building the screens, so every
  // widget is created showing the value it will actually use.
  const prefs::Data &p = prefs::get();
  pollMs = p.pollMs;
  fuseRating = p.fuseRating;
  rtStartA = p.rtStartA;
  rtMaxA = p.rtMaxA;
  rtSweepS = p.rtSweepS;
  probeFourWire = p.fourWire;
  probeTareOhm = p.tareOhm;
  battChem = p.battChem;
  battCells = p.battCells;
  battCutoff = p.battCutoff;
  battCutoffCustom = p.battCutoffCustom;
  battAmps = p.battAmps;
  battRatedAh = p.battRatedMah / 1000.0f;
  battCRateIdx = p.battCRateIdx >= battmodel::CRATE_N ? -1 : p.battCRateIdx;
  clampCells();   // a stored pack size may be out of range for its chemistry
  if (A.setPollRate) A.setPollRate(pollMs);

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
  buildTextEntry();
  buildWifiPicker();
  buildPicker();

  UnitCfg c = unitCfg("A");
  stepSize = c.step[c.defStep];
  lv_timer_create([](lv_timer_t *) { settingsTick(); }, 1000, nullptr);
  showScreen(SCR_MON);
  refreshRtest();
  refreshBatt();
}

}  // namespace ui
