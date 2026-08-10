package com.loadtester.el15

import android.Manifest
import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.os.Build
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.View
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import com.loadtester.el15.databinding.ActivityMainBinding
import java.util.Locale
import kotlin.math.ceil

/**
 * The instrument panel. All device/session state lives in [DeviceCore] — this
 * activity is a view over the core (it can be destroyed and recreated freely
 * while a connection, sweep, or bench session keeps running underneath).
 */
class MainActivity : BaseActivity(), DeviceCore.Ui {

    private lateinit var binding: ActivityMainBinding
    private lateinit var core: DeviceCore

    private var userEditingSetpoint = false
    private var updatingChips = false
    private var maxVoltageSeen = 0f

    private var pendingThemeRecreate = false
    private var nightMode = -1
    private var pendingResultKind: String? = null

    /** Which control panel is showing under the mode chips. */
    private enum class Panel { DEVICE, RTEST, BATT }
    private var panel = Panel.DEVICE

    /** Selected battery chemistry (index into [BatteryModel.CHEMS]). */
    private var chemIndex = 0

    // Only the six device modes map to chips that send a mode command.
    private val chipToMode: Map<Int, Int> by lazy {
        with(binding.monitor) {
            mapOf(
                chipCc.id to El15Protocol.MODE_CC, chipCv.id to El15Protocol.MODE_CV,
                chipCr.id to El15Protocol.MODE_CR, chipCp.id to El15Protocol.MODE_CP,
                chipCap.id to El15Protocol.MODE_CAP, chipDcr.id to El15Protocol.MODE_DCR,
            )
        }
    }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) startBleScan()
        else toast("Bluetooth permission denied — the demo device is still available")
    }

    private val notifPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* optional — alarms/results notifications simply stay silent if denied */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        core = DeviceCore.get(this)
        core.syncSettings()
        nightMode = resources.configuration.uiMode and
            android.content.res.Configuration.UI_MODE_NIGHT_MASK

        binding.headerStop.setOnClickListener { confirmEmergencyStop() }
        binding.headerSettings.setOnClickListener { startActivity(Intent(this, SettingsActivity::class.java)) }
        binding.headerInfo.setOnClickListener { showAbout() }

        with(binding.monitor) {
            gaugeVoltage.arcColor = color(R.color.value_green); gaugeVoltage.unit = "V"
            gaugeCurrent.arcColor = color(R.color.value_amber); gaugeCurrent.unit = "A"
        }
        setupControls()
        setupModeChips()
        setupWaveformControls()
        setupTest()
        setupBattery()
        applyLegend()
        clearReadouts()

        core.addUi(this)
        renderConnectionState()
        restoreLiveState()

        if (Build.VERSION.SDK_INT >= 33 && !Notifications.canPost(this)) {
            notifPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
        // If the last session died with the load energised, the guard offers a
        // reconnect-and-kill. Only meaningful once a UI exists to show it.
        core.checkCrashRecovery()
    }

    /**
     * Emergency stop. Confirmed only when nothing is actually energised — if the
     * load is on or a test is running, this fires immediately: a dialog in front
     * of an e-stop defeats the point of having one.
     */
    private fun confirmEmergencyStop() {
        val live = core.busy || (core.lastStatus?.loadOn == true)
        if (live) {
            core.emergencyLoadOff()
            toast(getString(R.string.estop_done))
            return
        }
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.estop)
            .setMessage(R.string.estop_idle)
            .setPositiveButton(R.string.estop_do) { _, _ -> core.emergencyLoadOff() }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    override fun onDestroy() {
        super.onDestroy()
        // The core (and its foreground service) owns the session — never tear
        // down the connection just because the Activity died.
        if (::core.isInitialized) core.removeUi(this)
    }

    /** Rebuild transient view state from the core after (re)creation. */
    private fun restoreLiveState() = with(binding.monitor) {
        for (pt in core.waveLog) waveformView.add(pt.v, pt.i, pt.p, pt.temp, pt.fan, pt.tMs)
        if (core.waveLog.isNotEmpty()) waveStats.text = waveformView.statsText()
        core.lastStatus?.let { if (core.isConnected) renderStatus(it) }
        if (core.rtest.running) {
            panel = Panel.RTEST
            testBar.visibility = View.VISIBLE
            testProgressText.visibility = View.VISIBLE
            testProgressText.text = core.lastProgressText
            startTestButton.setText(R.string.rt_stop)
        } else if (core.batt.running) {
            panel = Panel.BATT
            battProgressText.visibility = View.VISIBLE
            battProgressText.text = core.lastProgressText
            startBattButton.setText(R.string.batt_stop)
            battPauseButton.visibility = View.VISIBLE
            battPauseButton.setText(
                if (core.batt.isPaused) R.string.batt_resume else R.string.batt_pause)
        }
        applyPanel()
        updateRecordIcon()
    }

    override fun onResume() {
        super.onResume()
        pendingResultKind?.let { kind ->
            pendingResultKind = null
            startActivity(Intent(this, ResultActivity::class.java)
                .putExtra(ResultActivity.EXTRA_KIND, kind))
        }
        core.syncSettings()
        // Demo circuit may have been edited in Settings; apply it live.
        val newEmf = Prefs.demoEmf(this); val newR = Prefs.demoR(this)
        if (newEmf != core.demoEmf || newR != core.demoSeriesR) {
            core.applyDemoCircuit(newEmf, newR)
            if (core.isDemo) updateDemoStatusText()
        }
        if (Prefs.keepScreenOn(this)) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        // Only a real day/night flip triggers a re-theme, and never mid-session.
        val newNight = newConfig.uiMode and android.content.res.Configuration.UI_MODE_NIGHT_MASK
        if (newNight == nightMode) return
        nightMode = newNight
        if (!core.isConnected && !core.busy) recreate() else pendingThemeRecreate = true
    }

    private fun color(res: Int) = ContextCompat.getColor(this, res)

    private fun showAbout() = MaterialAlertDialogBuilder(this)
        .setTitle(getString(R.string.app_name))
        .setMessage(getString(R.string.about_body, BuildConfig.VERSION_NAME))
        .setPositiveButton(android.R.string.ok, null).show()

    private fun deviceLabel(): String =
        if (core.isDemo) "Demo (%.1f V, %.3f Ω)".format(core.demoEmf, core.demoSeriesR)
        else "EL15 (BLE)"

    // ---- Controls ---------------------------------------------------------
    private fun setupControls() = with(binding.monitor) {
        connButton.setOnClickListener {
            if (core.isConnected || core.ble.state == El15BleManager.State.CONNECTING) core.disconnect()
            else requestScan()
        }
        connStatusTap.setOnClickListener {
            if (core.isDemo) showDemoConfigDialog(applyLive = true, onDone = null)
        }
        setpointInput.setOnFocusChangeListener { _, f -> userEditingSetpoint = f }
        setSetpointButton.setOnClickListener {
            val v = setpointInput.text.toString().trim().toFloatOrNull()
            if (v == null) toast("Enter a valid number")
            else { core.controller.setSetpoint(v); setpointInput.clearFocus(); toast("Setpoint → $v") }
        }
        loadToggle.setOnClickListener {
            if (!core.isConnected) requestScan()
            else core.controller.setLoad(!(core.lastStatus?.loadOn ?: false))
        }
        lockButton.setOnClickListener { core.controller.setLock() }
    }

    private fun setupModeChips() = with(binding.monitor) {
        for (id in chipToMode.keys) findViewById<Chip>(id).setOnClickListener {
            if (updatingChips) return@setOnClickListener
            panel = Panel.DEVICE
            applyPanel()
            chipToMode[id]?.let { core.controller.setMode(it) }
        }
        fun panelChip(chip: Chip, target: Panel, clearLoad: Boolean) {
            chip.setOnClickListener {
                if (updatingChips) return@setOnClickListener
                if (panel == target && !chip.isChecked) {
                    // Re-tap on the active chip: keep it selected, panel stays.
                    updatingChips = true; chip.isChecked = true; updatingChips = false
                    return@setOnClickListener
                }
                panel = target
                if (clearLoad && core.isConnected && !core.busy) core.controller.setLoad(false)
                applyPanel()
            }
        }
        panelChip(chipRtest, Panel.RTEST, clearLoad = true)
        panelChip(chipBatt, Panel.BATT, clearLoad = true)
    }

    private fun applyPanel() = with(binding.monitor) {
        deviceControls.visibility = if (panel == Panel.DEVICE) View.VISIBLE else View.GONE
        rtestPanel.visibility = if (panel == Panel.RTEST) View.VISIBLE else View.GONE
        battPanel.visibility = if (panel == Panel.BATT) View.VISIBLE else View.GONE
        if (panel != Panel.DEVICE) {
            updatingChips = true
            modeChipGroup.check(if (panel == Panel.RTEST) chipRtest.id else chipBatt.id)
            updatingChips = false
        }
        if (panel == Panel.RTEST) updateProbeText()
        if (panel == Panel.BATT) refreshBattSummary()
        updateControlsEnabled()
    }

    private fun setupWaveformControls() = with(binding.monitor) {
        wavePauseButton.setOnClickListener {
            waveformView.paused = !waveformView.paused
            wavePauseButton.setIconResource(if (waveformView.paused) R.drawable.ic_play else R.drawable.ic_pause)
        }
        waveRecordButton.setOnClickListener {
            // Recording lives in the core so it survives the Activity.
            if (core.recording) {
                core.stopRecording()
                toast("Recording stopped (${core.recordLog.size} samples)")
            } else {
                core.startRecording()
                toast("Recording… (kept while the app runs in background)")
            }
            updateRecordIcon()
        }
        waveExportButton.setOnClickListener { waveformExportChooser() }
    }

    private fun updateRecordIcon() = with(binding.monitor) {
        waveRecordButton.alpha = if (core.recording) 1f else 0.55f
    }

    private fun applyLegend() {
        val sb = SpannableStringBuilder()
        sb.append("● Voltage")
        sb.setSpan(ForegroundColorSpan(color(R.color.value_green)), 0, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        val s = sb.length
        sb.append("    ● Current")
        sb.setSpan(ForegroundColorSpan(color(R.color.value_amber)), s, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        binding.monitor.waveLegend.text = sb
    }

    // ---- Resistance test ----------------------------------------------------
    private fun setupTest() = with(binding.monitor) {
        val a = this@MainActivity
        if (fuseInput.text.isNullOrBlank()) fuseInput.setText(fmtField(Prefs.fuseA(a)))
        if (sweepInput.text.isNullOrBlank()) sweepInput.setText(Prefs.sweepSeconds(a).toString())
        if (startAInput.text.isNullOrBlank()) startAInput.setText(fmtField(Prefs.sweepStartA(a)))
        if (peakAInput.text.isNullOrBlank()) peakAInput.setText(fmtField(Prefs.sweepMaxA(a)))

        fun info(layout: TextInputLayout, title: Int, body: Int) {
            layout.setEndIconOnClickListener {
                MaterialAlertDialogBuilder(a)
                    .setTitle(title).setMessage(body)
                    .setPositiveButton(android.R.string.ok, null).show()
            }
        }
        info(sweepLayout, R.string.rt_sweep, R.string.rt_sweep_info)
        info(startALayout, R.string.rt_start_a, R.string.rt_start_a_info)
        info(peakALayout, R.string.rt_peak_a, R.string.rt_peak_a_info)

        tareButton.setOnClickListener {
            if (core.rtest.running) { core.rtest.stop(); onTestFinishedUi("Stopped"); return@setOnClickListener }
            if (!connectedGuard()) return@setOnClickListener
            val fuse = readFuse() ?: return@setOnClickListener
            MaterialAlertDialogBuilder(a)
                .setTitle(R.string.rt_tare)
                .setMessage(R.string.rt_tare_confirm)
                .setPositiveButton(R.string.rt_start) { _, _ -> startSweep(fuse, tare = true) }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }

        startTestButton.setOnClickListener {
            if (core.rtest.running) { core.rtest.stop(); onTestFinishedUi("Test stopped"); return@setOnClickListener }
            if (!connectedGuard()) return@setOnClickListener
            val fuse = readFuse() ?: return@setOnClickListener
            val (peak, limiter) = predictedPeak(fuse)
            val sweepS = sweepInput.text.toString().trim().toLongOrNull()
                ?.coerceIn(ResistanceTest.MIN_SWEEP_S, ResistanceTest.MAX_SWEEP_S)
                ?: Prefs.sweepSeconds(a)
            MaterialAlertDialogBuilder(a)
                .setTitle(R.string.rt_confirm_title)
                .setMessage(getString(R.string.rt_confirm_msg, peak, limiter, sweepS))
                .setPositiveButton(R.string.rt_start) { _, _ -> startSweep(fuse, tare = false) }
                .setNegativeButton(R.string.cancel, null).show()
        }
    }

    private fun connectedGuard(): Boolean {
        if (!core.isConnected) { toast("Connect to the EL15 (or the demo) first"); return false }
        if (core.busy) { toast("Another test is already running"); return false }
        return true
    }

    private fun readFuse(): Float? {
        val fuse = binding.monitor.fuseInput.text.toString().trim().toFloatOrNull()
        if (fuse == null || fuse <= 0f) { toast("Enter the circuit's fuse rating in amps"); return null }
        if (fuse > 200f) { toast("That fuse rating looks too high — double-check it"); return null }
        Prefs.setFuseA(this, fuse)
        return fuse
    }

    /** What the sweep will actually peak at, and which limit is binding. */
    private fun predictedPeak(fuse: Float): Pair<Float, String> {
        val v = core.lastStatus?.voltage?.takeIf { it > El15Protocol.MIN_VOLTAGE_V }
            ?: El15Protocol.MAX_VOLTAGE_V
        val safety = Prefs.safetyFactor(this)
        val fuseCap = fuse * safety
        val powerCap = El15Protocol.MAX_POWER_W / v
        val peak = minOf(fuseCap, powerCap, El15Protocol.MAX_CURRENT_A)
        val limiter = when (peak) {
            fuseCap -> "%.0f%% of the %.1f A fuse".format(safety * 100, fuse)
            powerCap -> "the %.0f W power limit at %.1f V".format(El15Protocol.MAX_POWER_W, v)
            else -> "the %.0f A current limit".format(El15Protocol.MAX_CURRENT_A)
        }
        return peak to limiter
    }

    /** Say which probe wiring is in force and what it will do to the result. */
    private fun updateProbeText() = with(binding.monitor) {
        val fourWire = Prefs.fourWire(this@MainActivity)
        val tare = Prefs.tareOhm(this@MainActivity)
        probeText.text = when {
            fourWire -> getString(R.string.probe_4wire)
            tare > 0f -> getString(R.string.probe_2wire_tared, DeviceCore.formatOhm(tare))
            else -> getString(R.string.probe_2wire_untared)
        }
    }

    private fun startSweep(fuse: Float, tare: Boolean) = with(binding.monitor) {
        val a = this@MainActivity
        fuseInput.clearFocus()
        val sweepS = sweepInput.text.toString().trim().toLongOrNull()
            ?.coerceIn(ResistanceTest.MIN_SWEEP_S, ResistanceTest.MAX_SWEEP_S) ?: Prefs.sweepSeconds(a)
        val startA = startAInput.text.toString().trim().toFloatOrNull()?.coerceAtLeast(0f) ?: 0f
        val peakA = peakAInput.text.toString().trim().toFloatOrNull()?.coerceAtLeast(0f) ?: 0f
        sweepInput.setText(sweepS.toString())
        Prefs.setSweepSetup(a, sweepS, startA, peakA)

        testBar.visibility = View.VISIBLE; testBar.progress = 0
        testProgressText.visibility = View.VISIBLE
        testProgressText.text = if (tare) "Priming (lead tare)…" else "Priming… (~${sweepS}s sweep)"
        startTestButton.setText(R.string.rt_stop)
        // start FIRST so rtest.running locks the controls below
        core.startRTest(fuse, sweepS, startA, peakA, tare)
        updateControlsEnabled()
    }

    private fun onTestFinishedUi(message: String?) = with(binding.monitor) {
        startTestButton.setText(R.string.rt_start)
        testBar.visibility = View.GONE
        message?.let { testProgressText.visibility = View.VISIBLE; testProgressText.text = it }
        updateProbeText()
        updateControlsEnabled()
    }

    // ---- DeviceCore.Ui: resistance test -------------------------------------
    override fun coreRTestProgress(
        elapsedS: Float, totalS: Float, target: Float,
        voltage: Float, current: Float, resistance: Float, rValid: Boolean,
    ) = with(binding.monitor) {
        testBar.visibility = View.VISIBLE
        testBar.progress = if (totalS > 0f) (elapsedS / totalS * 100f).toInt().coerceIn(0, 100) else 0
        testProgressText.visibility = View.VISIBLE
        testProgressText.text = core.lastProgressText
    }

    override fun coreRTestComplete(result: ResistanceTest.Result) {
        onTestFinishedUi(core.lastProgressText)
        // A tare sweep has no result screen: its whole output is the stored lead
        // resistance, which updateProbeText() above now shows.
        if (core.lastRTestResult !== result) return
        openResult(ResultActivity.KIND_RTEST)
    }

    override fun coreRTestError(message: String) {
        onTestFinishedUi(message)
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            MaterialAlertDialogBuilder(this).setTitle("Test stopped").setMessage(message)
                .setPositiveButton(android.R.string.ok, null).show()
        }
    }

    private fun openResult(kind: String) {
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            startActivity(Intent(this, ResultActivity::class.java)
                .putExtra(ResultActivity.EXTRA_KIND, kind))
        } else {
            // Background activity starts are blocked; open it when we return.
            pendingResultKind = kind
        }
    }

    // ---- Battery capacity test -------------------------------------------------
    private fun setupBattery() = with(binding.monitor) {
        val a = this@MainActivity
        chemIndex = Prefs.battChem(a)

        // One chip per chemistry curve, in battmodel order.
        BatteryModel.CHEMS.forEachIndexed { i, chem ->
            val chip = layoutInflater.inflate(R.layout.chip_mode, chemChipGroup, false) as Chip
            chip.text = chem.shortName
            chip.id = View.generateViewId()
            chip.isCheckable = true
            chip.tag = i
            chip.setOnClickListener {
                if (updatingChips) return@setOnClickListener
                chemIndex = i
                onChemistryChanged(userPicked = true)
            }
            chemChipGroup.addView(chip)
        }

        if (cellsInput.text.isNullOrBlank()) cellsInput.setText(Prefs.battCells(a).toString())
        if (ratedInput.text.isNullOrBlank()) {
            ratedInput.setText(Prefs.battRatedAh(a).let { if (it > 0f) fmtField(it) else "" })
        }
        if (battCurrentInput.text.isNullOrBlank()) battCurrentInput.setText(fmtField(Prefs.battCurrentA(a)))
        if (cutoffInput.text.isNullOrBlank()) cutoffInput.setText(fmtField(Prefs.battCutoffV(a)))

        for (e in listOf(cellsInput, ratedInput, battCurrentInput, cutoffInput)) {
            e.addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, x: Int, y: Int, z: Int) {}
                override fun onTextChanged(s: CharSequence?, x: Int, y: Int, z: Int) {}
                override fun afterTextChanged(s: android.text.Editable?) = refreshBattSummary()
            })
        }

        startBattButton.setOnClickListener {
            if (core.batt.running) { confirmStopBatt(); return@setOnClickListener }
            if (!connectedGuard()) return@setOnClickListener
            startBatteryFlow()
        }

        battPauseButton.setOnClickListener {
            if (core.batt.isPaused) {
                if (core.batt.resume()) toast(getString(R.string.batt_resumed))
            } else {
                if (core.batt.pause(getString(R.string.batt_paused_manual))) {
                    toast(getString(R.string.batt_paused))
                }
            }
            updateBattButtons()
        }

        onChemistryChanged(userPicked = false)
    }

    /**
     * Apply a chemistry choice: check its chip, show its detail line, rebuild the
     * C-rate chips, and — when the user picked it — re-seed cells and cutoff from
     * the model and the live pack voltage.
     */
    private fun onChemistryChanged(userPicked: Boolean) = with(binding.monitor) {
        val a = this@MainActivity
        val chem = BatteryModel.CHEMS[chemIndex]

        updatingChips = true
        for (i in 0 until chemChipGroup.childCount) {
            val chip = chemChipGroup.getChildAt(i) as Chip
            chip.isChecked = (chip.tag as Int) == chemIndex
        }
        updatingChips = false

        chemDetail.text = if (BatteryModel.hasCurve(chemIndex)) {
            getString(R.string.batt_chem_detail, chem.name, chem.nom, chem.full, chem.cut, chem.maxCells)
        } else {
            getString(R.string.batt_chem_custom_detail)
        }

        // Lead-acid 12 V is a fixed six-cell pack: offering a cell count that
        // cannot be changed would be a control that does nothing.
        val fixed = BatteryModel.cellsFixed(chemIndex)
        cellsLayout.isEnabled = !fixed && BatteryModel.hasCurve(chemIndex)
        cellsInput.isEnabled = cellsLayout.isEnabled

        if (userPicked) {
            if (fixed) {
                cellsInput.setText(chem.fixedCells.toString())
            } else if (BatteryModel.hasCurve(chemIndex)) {
                // Suggest a cell count from the pack actually connected, so the
                // cutoff below lands somewhere sane without the user doing sums.
                val voc = core.lastStatus?.voltage ?: 0f
                val suggested = BatteryModel.suggestCells(chemIndex, voc)
                if (suggested > 0) cellsInput.setText(suggested.toString())
            }
            val n = cellsInput.text.toString().trim().toIntOrNull() ?: 0
            if (n > 0 && chem.cut > 0f) cutoffInput.setText("%.2f".format(Locale.US, n * chem.cut))
        }

        rebuildCRateChips()
        refreshBattSummary()
    }

    /**
     * C-rate quick-set chips from the chemistry's conventional rates: tapping one
     * sets the discharge current to C x the rated capacity. The chemistry's
     * standard rate is marked, because capacity is only meaningful at a stated
     * rate and every chemistry has a conventional one.
     */
    private fun rebuildCRateChips() = with(binding.monitor) {
        cRateChipGroup.removeAllViews()
        val chem = BatteryModel.CHEMS[chemIndex]
        chem.cRate.forEachIndexed { i, c ->
            val chip = layoutInflater.inflate(R.layout.chip_mode, cRateChipGroup, false) as Chip
            val label = if (c >= 1f) "%.0fC".format(c) else "%.2fC".format(c).trimEnd('0').trimEnd('.')
            chip.text = if (i == chem.stdRate) "$label ★" else label
            chip.id = View.generateViewId()
            chip.isCheckable = false
            chip.setOnClickListener {
                val rated = ratedInput.text.toString().trim().toFloatOrNull() ?: 0f
                if (rated <= 0f) { toast(getString(R.string.batt_need_rated)); return@setOnClickListener }
                battCurrentInput.setText("%.3f".format(Locale.US,
                    minOf(rated * c, El15Protocol.MAX_CURRENT_A)))
            }
            cRateChipGroup.addView(chip)
        }
    }

    /**
     * The live "what this run will do" line: estimated duration and C-rate, plus
     * warnings for a cutoff below the chemistry's floor or above what the pack
     * currently reads (which would stop the test instantly).
     */
    private fun refreshBattSummary() = with(binding.monitor) {
        val chem = BatteryModel.CHEMS[chemIndex]
        val rated = ratedInput.text.toString().trim().toFloatOrNull() ?: 0f
        val cur = battCurrentInput.text.toString().trim().toFloatOrNull() ?: 0f
        val cutoff = cutoffInput.text.toString().trim().toFloatOrNull() ?: 0f
        val n = cellsInput.text.toString().trim().toIntOrNull() ?: 0

        val parts = ArrayList<String>()
        if (rated > 0f && cur > 0f) {
            parts += "≈ ${DeviceCore.formatDuration((rated / cur * 3600).toLong())} at %.2fC"
                .format(cur / rated)
        }
        if (cutoff > 0f) parts += "stops at %.2f V".format(cutoff)
        if (n > 0 && chem.full > 0f) parts += "%dS · full ≈ %.1f V".format(n, n * chem.full)

        val warn = StringBuilder()
        val floor = n * chem.cut
        if (n > 0 && chem.cut > 0f && cutoff in 0.01f..floor && cutoff < floor) {
            warn.append("\n⚠ below the %s safe floor of %.2f V".format(chem.name, floor))
        }
        if (n > 0 && chem.full > 0f && n * chem.full > El15Protocol.MAX_VOLTAGE_V) {
            warn.append("\n⚠ a full %dS pack is %.1f V — above the EL15's %.0f V limit"
                .format(n, n * chem.full, El15Protocol.MAX_VOLTAGE_V))
        }
        val liveV = core.lastStatus?.voltage ?: 0f
        if (cutoff > 0f && liveV > El15Protocol.MIN_VOLTAGE_V && cutoff >= liveV) {
            warn.append("\n⚠ pack reads %.2f V now — the test would stop immediately".format(liveV))
        }
        battSummary.text =
            (if (parts.isEmpty()) getString(R.string.batt_enter_hint) else parts.joinToString(" · ")) +
                warn.toString()
    }

    private fun startBatteryFlow(): Unit = with(binding.monitor) {
        val a = this@MainActivity
        val chem = BatteryModel.CHEMS[chemIndex]
        val n = cellsInput.text.toString().trim().toIntOrNull() ?: 0
        val cur = battCurrentInput.text.toString().trim().toFloatOrNull()
        val cutoff = cutoffInput.text.toString().trim().toFloatOrNull()
        val rated = ratedInput.text.toString().trim().toFloatOrNull() ?: 0f

        if (cur == null || cur <= 0f) { toast("Enter a discharge current"); return }
        if (cutoff == null || cutoff <= 0f) { toast("Enter a cutoff voltage"); return }
        if (BatteryModel.hasCurve(chemIndex)) {
            if (n < 1) { toast("Enter the number of cells in series"); return }
            if (n > chem.maxCells) {
                toast("%s tops out at %dS to stay under %.0f V"
                    .format(chem.shortName, chem.maxCells, El15Protocol.MAX_VOLTAGE_V))
                return
            }
        }
        Prefs.setBattSetup(a, chemIndex, n, cutoff, cur, rated)

        val eta = if (rated > 0f) " ≈ ${DeviceCore.formatDuration((rated / cur * 3600).toLong())}" else ""
        MaterialAlertDialogBuilder(a)
            .setTitle(R.string.batt_confirm_title)
            .setMessage(getString(R.string.batt_confirm_msg, cur, cutoff, eta))
            .setPositiveButton(R.string.batt_start) { _, _ ->
                battProgressText.visibility = View.VISIBLE
                battProgressText.text = getString(R.string.batt_priming)
                core.startBattTest(chemIndex, n, cutoff, cur, rated, Prefs.battRestS(a))
                updateBattButtons()
                updateControlsEnabled()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun confirmStopBatt() = MaterialAlertDialogBuilder(this)
        .setTitle(R.string.batt_stop)
        .setMessage(R.string.batt_stop_confirm)
        .setPositiveButton(R.string.batt_stop) { _, _ ->
            core.batt.stop(getString(R.string.batt_stopped_manual))
        }
        .setNegativeButton(R.string.cancel, null)
        .show()

    private fun updateBattButtons() = with(binding.monitor) {
        val running = core.batt.running
        startBattButton.setText(if (running) R.string.batt_stop else R.string.batt_start)
        battPauseButton.visibility = if (running) View.VISIBLE else View.GONE
        battPauseButton.setText(if (core.batt.isPaused) R.string.batt_resume else R.string.batt_pause)
    }

    // ---- DeviceCore.Ui: battery capacity -------------------------------------
    override fun coreBattProgress(
        v: Float, i: Float, ah: Float, wh: Float, temp: Float,
        elapsedS: Long, phase: Int,
    ) = with(binding.monitor) {
        battProgressText.visibility = View.VISIBLE
        val eta = core.batt.remainingS()
        val soc = core.batt.socPct()
        val extra = buildString {
            if (soc >= 0f) append("  ·  ~%.0f%%".format(soc))
            if (eta > 0L) {
                append("  ·  ${DeviceCore.formatDuration(eta)} left")
                // Say which estimate this is: the curve-based one and the
                // nameplate one are not equally trustworthy.
                append(if (core.batt.etaFromCurve()) " (curve)" else " (rated)")
            }
        }
        battProgressText.text = core.lastProgressText + extra
    }

    override fun coreBattComplete(result: CapacityTest.Result) {
        with(binding.monitor) {
            battProgressText.visibility = View.VISIBLE
            battProgressText.text = core.lastProgressText
        }
        updateBattButtons()
        updateControlsEnabled()
        openResult(ResultActivity.KIND_BATT)
    }

    override fun coreBattError(message: String) {
        with(binding.monitor) {
            battProgressText.visibility = View.VISIBLE
            battProgressText.text = message
        }
        updateBattButtons()
        updateControlsEnabled()
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            MaterialAlertDialogBuilder(this).setTitle("Test stopped").setMessage(message)
                .setPositiveButton(android.R.string.ok, null).show()
        }
    }

    override fun coreBattPause(paused: Boolean, reason: String?) {
        updateBattButtons()
        if (paused && reason != null) toast(reason)
    }

    // ---- DeviceCore.Ui: link guard ---------------------------------------------
    override fun coreGuardAlert(title: String, message: String, resolved: Boolean) {
        with(binding.monitor) {
            warningBox.visibility = View.VISIBLE
            warningText.text = "$title — $message"
        }
        if (resolved) toast(message)
    }

    override fun coreGuardFailed(message: String) {
        with(binding.monitor) {
            warningBox.visibility = View.VISIBLE
            warningText.text = message
            // Tap to retry: a locked banner with no exit would suppress every
            // later protection alert.
            warningBox.setOnClickListener { core.guard.retry() }
        }
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.guard_failed_title)
                .setMessage(message)
                .setPositiveButton(R.string.guard_retry) { _, _ -> core.guard.retry() }
                .setNegativeButton(android.R.string.ok, null)
                .show()
        }
    }

    // ---- Permissions / scanning ------------------------------------------
    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    private fun hasPermissions() = requiredPermissions().all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    private fun requestScan() {
        showDevicePicker()
        when {
            !core.ble.isBluetoothOn -> toast("Bluetooth is off — the demo device is still available")
            hasPermissions() -> startBleScan()
            else -> permissionLauncher.launch(requiredPermissions())
        }
    }

    private fun startBleScan() {
        if (deviceDialog?.isShowing != true) return
        core.foundDevices.clear()
        core.ble.pollIntervalMs = Prefs.pollMs(this)
        core.ble.startScan()
    }

    private var deviceDialog: AlertDialog? = null
    private lateinit var deviceListAdapter: ArrayAdapter<String>

    /** Fixed rows before the scanned devices: demo, then optional reconnect. */
    private var pickerLastDevice: Pair<String, String>? = null

    @SuppressLint("MissingPermission")
    private fun showDevicePicker() {
        core.foundDevices.clear()
        pickerLastDevice = Prefs.lastDevice(this)
        deviceListAdapter = ArrayAdapter(this, android.R.layout.simple_list_item_1)
        deviceListAdapter.add(getString(R.string.demo_device))
        pickerLastDevice?.let { (addr, name) ->
            deviceListAdapter.add(getString(R.string.reconnect_row, name, addr))
        }
        val fixedRows = 1 + (if (pickerLastDevice != null) 1 else 0)
        deviceDialog = MaterialAlertDialogBuilder(this)
            .setTitle("Select device")
            .setAdapter(deviceListAdapter) { _, which ->
                when {
                    which == 0 -> showDemoConfigDialog(applyLive = false) { startSimulator() }
                    which == 1 && pickerLastDevice != null -> {
                        val (addr, name) = pickerLastDevice!!
                        when {
                            !hasPermissions() -> {
                                toast("Grant the Bluetooth permission, then scan to reconnect")
                                permissionLauncher.launch(requiredPermissions())
                            }
                            else -> {
                                // Scan-based reconnect (reconnect() stops the picker
                                // scan itself), so a random-address device reconnects.
                                if (!core.ble.reconnect(addr)) toast("Could not reconnect to $name — is Bluetooth on?")
                            }
                        }
                    }
                    else -> core.foundDevices.values.toList().getOrNull(which - fixedRows)?.let { dev ->
                        Prefs.setLastDevice(this, dev.address,
                            try { dev.name ?: "EL15" } catch (se: SecurityException) { "EL15" })
                        core.ble.connect(dev)
                    }
                }
            }
            .setNegativeButton("Cancel") { _, _ -> core.ble.stopScan() }
            .setOnDismissListener { core.ble.stopScan() }.show()
    }

    // ---- Demo device ------------------------------------------------------
    private fun startSimulator() {
        core.ble.stopScan(); deviceDialog?.dismiss()
        maxVoltageSeen = 0f
        core.startSimulator()
        clearReadouts()
        renderConnectionState()
    }

    private fun updateDemoStatusText() {
        binding.monitor.connStatusText.text = "Demo simulator"
        binding.monitor.connStatusSub.text =
            "%.1f V · %.3f Ω · tap to edit circuit".format(core.demoEmf, core.demoSeriesR)
    }

    private fun showDemoConfigDialog(applyLive: Boolean, onDone: (() -> Unit)?) {
        val view = layoutInflater.inflate(R.layout.dialog_demo_circuit, null)
        val emfIn = view.findViewById<TextInputEditText>(R.id.demoEmfInput)
        val resIn = view.findViewById<TextInputEditText>(R.id.demoResInput)
        emfIn.setText(fmtField(core.demoEmf)); resIn.setText(fmtField(core.demoSeriesR))
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.demo_cfg_title).setView(view)
            .setPositiveButton(if (onDone != null) R.string.demo_cfg_connect else R.string.demo_cfg_apply) { _, _ ->
                val emf = (emfIn.text.toString().trim().toFloatOrNull() ?: core.demoEmf).coerceIn(0.1f, 100f)
                val r = (resIn.text.toString().trim().toFloatOrNull() ?: core.demoSeriesR).coerceIn(0f, 100f)
                Prefs.setDemo(this, emf, r)
                core.applyDemoCircuit(emf, r)
                if (applyLive && core.isDemo) updateDemoStatusText()
                onDone?.invoke()
            }
            .setNegativeButton(R.string.cancel, null).show()
    }

    private fun fmtField(v: Float) = if (v == v.toLong().toFloat()) v.toLong().toString() else v.toString()

    // ---- Waveform CSV -----------------------------------------------------
    private fun buildWaveformCsv(rows: List<WaveformView.WPoint>): ByteArray {
        val sb = StringBuilder("time_ms,elapsed_s,voltage_V,current_A,power_W,temp_C,fan\n")
        val t0 = rows.first().tMs
        for (r in rows) sb.append("%d,%.3f,%.4f,%.4f,%.4f,%.2f,%d\n".format(
            Locale.US, r.tMs, (r.tMs - t0) / 1000.0, r.v, r.i, r.p, r.temp, r.fan))
        return sb.toString().toByteArray()
    }

    private fun waveformExportChooser() {
        // Prefer the core's recorded log (survives app backgrounding); fall
        // back to whatever the on-screen waveform holds.
        val rows = if (core.recordLog.isNotEmpty()) core.recordLog.toList()
        else binding.monitor.waveformView.exportRows()
        if (rows.isEmpty()) { toast("No waveform data yet"); return }
        val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)
            .format(java.util.Date())
        val name = "el15-waveform-$stamp.csv"
        MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.wave_export_title, rows.size))
            .setItems(arrayOf(getString(R.string.rt_share), getString(R.string.rt_save_device))) { _, which ->
                try {
                    val bytes = buildWaveformCsv(rows)
                    if (which == 0) {
                        Exporter.share(this, name, "text/csv", bytes, "EL15 waveform (${rows.size} samples)")
                    } else {
                        withStoragePermission {
                            val where = Exporter.saveToDownloads(this, name, "text/csv", bytes)
                            toast(if (where != null) getString(R.string.rt_saved_to, where) else "Save failed")
                        }
                    }
                } catch (e: Exception) { toast("Export failed: ${e.message}") }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private var pendingStorageAction: (() -> Unit)? = null
    private val storagePermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) pendingStorageAction?.invoke() else toast("Storage permission denied")
        pendingStorageAction = null
    }

    private fun withStoragePermission(action: () -> Unit) {
        if (!Exporter.needsLegacyWritePermission() ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            action()
        } else {
            pendingStorageAction = action
            storagePermission.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
    }

    // ---- Status / rendering ----------------------------------------------
    private fun setConnDot(connected: Boolean) {
        binding.monitor.connDot.backgroundTintList =
            ColorStateList.valueOf(color(if (connected) R.color.value_green else R.color.value_red))
    }

    private fun clearReadouts() = with(binding.monitor) {
        loadToggle.setText(R.string.load_on)
        loadToggle.backgroundTintList = ColorStateList.valueOf(color(R.color.value_green))
        lockButton.setText(R.string.lock)
        gaugeVoltage.set(0f, 20f, 2, "VOLTS · 0–20V", false)
        gaugeCurrent.set(0f, 12f, 3, "AMPS · 0–12A", false)
        powerValue.text = "—"; powerBar.progress = 0; modeValue.text = "—"; extraValue.text = ""
        warningBox.visibility = View.GONE
        packetHex.text = "— no packets —"; packetCrc.text = "CRC —"
        packetCrc.setTextColor(color(R.color.muted))
        waveStats.text = "—"; waveformView.clear()
    }

    /** Renders connection status, button labels, and control enablement. */
    private fun renderConnectionState() = with(binding.monitor) {
        val state = core.ble.state
        when {
            core.isDemo -> updateDemoStatusText()
            state == El15BleManager.State.CONNECTED -> {
                connStatusText.text = "EL15"; connStatusSub.text = "Connected · FFF0"
            }
            state == El15BleManager.State.CONNECTING -> {
                connStatusText.text = "Connecting…"; connStatusSub.text = ""
            }
            state == El15BleManager.State.SCANNING -> {
                connStatusText.text = "Scanning…"; connStatusSub.text = ""
            }
            else -> {
                connStatusText.text = getString(R.string.disconnected)
                connStatusSub.text = getString(R.string.no_device)
            }
        }
        connButton.text = when {
            core.isDemo -> getString(R.string.disconnect)
            state == El15BleManager.State.CONNECTED -> getString(R.string.disconnect)
            state == El15BleManager.State.CONNECTING -> getString(R.string.cancel)
            else -> getString(R.string.scan_connect)
        }
        setConnDot(core.isConnected)
        updateControlsEnabled()
    }

    // ---- DeviceCore.Ui: transport ---------------------------------------------
    override fun coreStateChanged() {
        val connected = core.isConnected
        renderConnectionState()
        // A test may have been stopped from outside this UI (the notification's
        // Load-OFF action, an e-stop) — resync the Start/Stop controls with the
        // engines rather than showing a phantom running test.
        with(binding.monitor) {
            if (!core.rtest.running) {
                startTestButton.setText(R.string.rt_start)
                testBar.visibility = View.GONE
                if (core.lastProgressText.isNotEmpty() && testProgressText.visibility == View.VISIBLE) {
                    testProgressText.text = core.lastProgressText
                }
            }
            if (!core.batt.running &&
                core.lastProgressText.isNotEmpty() && battProgressText.visibility == View.VISIBLE
            ) {
                battProgressText.text = core.lastProgressText
            }
        }
        updateBattButtons()
        if (!connected) {
            onTestFinishedUi(null)
            maxVoltageSeen = 0f
            clearReadouts()
            if (pendingThemeRecreate && !core.busy) {
                pendingThemeRecreate = false
                recreate()
                return
            }
        }
        if (core.ble.state != El15BleManager.State.SCANNING && !core.isDemo &&
            core.ble.state != El15BleManager.State.IDLE) {
            deviceDialog?.dismiss()
        }
    }

    override fun coreDeviceFound(address: String, name: String) {
        if (deviceDialog?.isShowing == true) {
            deviceListAdapter.add("$name\n$address")
            deviceListAdapter.notifyDataSetChanged()
        }
    }

    override fun coreStatus(status: El15Status) = renderStatus(status)

    private fun renderStatus(status: El15Status): Unit = with(binding.monitor) {
        // Packet inspector shows every frame, including corrupt ones…
        packetHex.text = status.raw.ifEmpty { "— no packets —" }
        packetCrc.text = "CRC ${if (status.crcPass) "✓" else "✗"}"
        packetCrc.setTextColor(color(if (status.crcPass) R.color.value_green else R.color.value_red))
        // …but nothing else may consume a corrupt/truncated frame's values.
        if (!status.valid) return

        maxVoltageSeen = maxOf(maxVoltageSeen, status.voltage)
        val vMax = maxOf(15f, ceil((maxVoltageSeen + 3f) / 5f) * 5f)
        gaugeVoltage.set(status.voltage, vMax, 2, "VOLTS · 0–${vMax.toInt()}V", true)
        val curr = if (status.mode == El15Protocol.MODE_DCR) status.dcrI1 else status.current
        gaugeCurrent.set(curr, 12f, 3, "AMPS · 0–12A", true)

        powerValue.text = if (status.mode == El15Protocol.MODE_DCR) "—" else "%.1f W".format(status.power)
        powerBar.progress = status.power.toInt().coerceIn(0, 150)
        modeValue.text = status.modeName
        extraValue.text = buildExtraLine(status)

        loadToggle.text = if (status.loadOn) getString(R.string.load_off) else getString(R.string.load_on)
        loadToggle.backgroundTintList = ColorStateList.valueOf(color(if (status.loadOn) R.color.value_red else R.color.value_green))
        lockButton.setText(if (status.lockOn) R.string.locked else R.string.lock)

        if (status.warning.isNotEmpty()) {
            warningBox.visibility = View.VISIBLE
            warningText.text = getString(R.string.protection, status.warning)
        } else warningBox.visibility = View.GONE

        if (panel == Panel.DEVICE) {
            val chipId = chipToMode.entries.firstOrNull { it.value == status.mode }?.key
            updatingChips = true
            if (chipId != null) modeChipGroup.check(chipId) else modeChipGroup.clearCheck()
            updatingChips = false
        }

        setpointLayout.hint = "${status.setpointLabel} (${status.setpointUnit})"
        if (!userEditingSetpoint && status.setpointInPacket) {
            // Locale.US: this text is re-parsed by toFloatOrNull on SET, which
            // rejects comma decimals.
            setpointInput.setText("%.${status.setpointDecimals}f".format(Locale.US, status.setpoint))
        }

        waveformView.add(status.voltage, curr, status.power, status.temperature, status.fanSpeed, System.currentTimeMillis())
        waveStats.text = waveformView.statsText()
    }

    private fun buildExtraLine(s: El15Status): String {
        val parts = mutableListOf<String>()
        when (s.mode) {
            El15Protocol.MODE_CAP -> { parts += "Energy %.3f Wh".format(s.energyWh); parts += "Capacity %.3f Ah".format(s.capacityAh) }
            El15Protocol.MODE_DCR -> { parts += "R %.1f mΩ".format(s.dcrMilliOhm); parts += "I2 %.3f A".format(s.dcrI2) }
            else -> {
                if (s.runtime > 0) parts += "Runtime ${formatRuntime(s.runtime)}"
                if (s.temperature != 0f) parts += "Temp %.1f°C".format(s.temperature)
            }
        }
        parts += "Fan ${s.fanSpeed}/${El15Protocol.FAN_SPEED_MAX}"
        parts += if (s.ready) "Ready" else "Idle"
        return parts.joinToString("   ")
    }

    private fun formatRuntime(seconds: Int): String {
        val h = seconds / 3600; val m = (seconds % 3600) / 60; val sec = seconds % 60
        return if (h > 0) "%d:%02d:%02d".format(h, m, sec) else "%02d:%02d".format(m, sec)
    }

    private fun updateControlsEnabled() = with(binding.monitor) {
        val connected = core.isConnected
        val busy = core.busy
        val idle = connected && !busy
        val deviceMode = idle && panel == Panel.DEVICE
        for (id in chipToMode.keys) findViewById<Chip>(id).isEnabled = idle
        chipRtest.isEnabled = idle
        chipBatt.isEnabled = idle
        setpointInput.isEnabled = deviceMode; setSetpointButton.isEnabled = deviceMode
        lockButton.isEnabled = deviceMode
        // Enabled while disconnected on purpose: tapping it opens the scan
        // dialog (see the click handler). Only a running test locks it.
        loadToggle.isEnabled = !busy
        // R-Test setup
        fuseInput.isEnabled = idle
        sweepInput.isEnabled = idle
        startAInput.isEnabled = idle
        peakAInput.isEnabled = idle
        // Stop must stay reachable while THIS engine runs, so gate on the other.
        startTestButton.isEnabled = connected && !core.batt.running
        tareButton.isEnabled = connected && !core.batt.running
        tareButton.setText(if (core.rtest.running) R.string.rt_stop else R.string.rt_tare)
        // Battery setup
        for (v in listOf<View>(cellsInput, ratedInput, battCurrentInput, cutoffInput)) {
            v.isEnabled = idle
        }
        for (i in 0 until chemChipGroup.childCount) chemChipGroup.getChildAt(i).isEnabled = idle
        for (i in 0 until cRateChipGroup.childCount) cRateChipGroup.getChildAt(i).isEnabled = idle
        startBattButton.isEnabled = connected && !core.rtest.running
    }

    private fun toast(msg: String) = android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_SHORT).show()
}
