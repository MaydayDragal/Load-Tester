package com.loadtester.el15

import android.content.Context
import android.media.AudioManager
import android.media.ToneGenerator
import java.io.File
import java.util.Locale

/**
 * Process-lifetime hub that owns the device session: BLE transport, demo
 * simulator, both test engines, the link guard, the live sample log, threshold
 * alarms, and periodic logging.
 *
 * This is the app's counterpart to the firmware's `main.cpp`: it owns the
 * objects, routes decoded status packets to the UI and whichever engine is
 * running, applies probe compensation once at the boundary, and arms the link
 * guard from the load's own reported state.
 *
 * Both [MainActivity] (UI) and [MonitorService] (foreground notification) attach
 * to this single instance, so a running connection, sweep, or battery test
 * survives the Activity being destroyed — the UI is a view over the core, not
 * its owner.
 *
 * All state is main-thread confined (the transports already deliver on main).
 */
class DeviceCore private constructor(private val app: Context) :
    El15BleManager.Listener,
    ResistanceTest.Callback,
    CapacityTest.Callback,
    LinkGuard.Listener {

    /** Everything the UI renders; every callback arrives on the main thread. */
    interface Ui {
        fun coreStateChanged() {}
        fun coreDeviceFound(address: String, name: String) {}
        fun coreStatus(status: El15Status) {}

        fun coreRTestProgress(
            elapsedS: Float, totalS: Float, target: Float,
            voltage: Float, current: Float, resistance: Float, rValid: Boolean,
        ) {}

        fun coreRTestComplete(result: ResistanceTest.Result) {}
        fun coreRTestError(message: String) {}

        fun coreBattProgress(
            v: Float, i: Float, ah: Float, wh: Float, temp: Float,
            elapsedS: Long, phase: Int,
        ) {}

        fun coreBattComplete(result: CapacityTest.Result) {}
        fun coreBattError(message: String) {}
        fun coreBattPause(paused: Boolean, reason: String?) {}

        fun coreGuardAlert(title: String, message: String, resolved: Boolean) {}
        fun coreGuardFailed(message: String) {}
    }

    val ble = El15BleManager(app).also { it.listener = this }

    var simulator: El15Simulator? = null
        private set

    var demoEmf = Prefs.demoEmf(app)
    var demoSeriesR = Prefs.demoR(app)

    val controller: El15Controller get() = simulator ?: ble

    /** Indirection so the engines follow a transport swap without being rebuilt. */
    private val activeController = object : El15Controller {
        override fun setMode(mode: Int) = controller.setMode(mode)
        override fun setSetpoint(value: Float) = controller.setSetpoint(value)
        override fun setLoad(on: Boolean) = controller.setLoad(on)
        override fun setLock() = controller.setLock()
    }

    val rtest = ResistanceTest(activeController, this)
    val batt = CapacityTest(activeController, this)
    val guard = LinkGuard(app, ble).also { it.listener = this }

    /**
     * True while an R-Test sweep is measuring the LEAD RESISTANCE (probes shorted
     * together) rather than a circuit. Its result is stored as the tare instead
     * of being shown as a measurement.
     */
    var tareRun = false
        private set

    var lastStatus: El15Status? = null
        private set

    var lastProgressText: String = ""
        private set

    /** The most recent finished results, for a result screen opened later. */
    var lastRTestResult: ResistanceTest.Result? = null
        private set
    var lastBattResult: CapacityTest.Result? = null
        private set

    /**
     * The report number each finished run has claimed, or null until it exports
     * something. Held HERE rather than in the result screen because that screen
     * is destroyed and rebuilt freely: a per-Activity reservation burned a fresh
     * number every time the user came back, and the CSV and the PDF of one test
     * ended up filed under different numbers (RTEST_002.pdf next to
     * RTEST_003.CSV). One run, one number, both formats.
     */
    private var rtestReportName: String? = null
    private var battReportName: String? = null

    /**
     * The base `RTEST_007.CSV` / `BATT_003.CSV` name for the last result of
     * [kind], claiming one on first use. Callers swap the extension for other
     * formats rather than asking for another name.
     */
    fun reportName(kind: String): String {
        val batt = kind == ResultActivity.KIND_BATT
        val existing = if (batt) battReportName else rtestReportName
        if (existing != null) return existing
        val name = Report.nextName(app, if (batt) "BATT" else "RTEST")
        if (batt) {
            battReportName = name
        } else {
            rtestReportName = name
        }
        return name
    }

    /** Rolling display buffer + optional full recording (survives UI death). */
    val waveLog = ArrayDeque<WaveformView.WPoint>()
    val recordLog = ArrayDeque<WaveformView.WPoint>()
    var recording = false
        private set

    private val listeners = LinkedHashSet<Ui>()
    private var tone: ToneGenerator? = null
    private var lastAlarmMs = 0L
    private var lastSnapshotMs = 0L

    val isConnected: Boolean
        get() = simulator != null || ble.state == El15BleManager.State.CONNECTED

    val isDemo: Boolean get() = simulator != null

    /** Engine mutual exclusion: only one engine may drive the load at a time. */
    val busy: Boolean get() = rtest.running || batt.running

    fun addUi(ui: Ui) { listeners.add(ui) }
    fun removeUi(ui: Ui) { listeners.remove(ui) }
    private fun each(block: (Ui) -> Unit) { listeners.toList().forEach(block) }

    val foundDevices = LinkedHashMap<String, android.bluetooth.BluetoothDevice>()

    // ---- Probe compensation ---------------------------------------------------
    /**
     * Put the lead drop back on a reading.
     *
     * The load senses voltage at its own terminals, so a 2-wire hook-up reads
     * short by I*R of the test leads. Once the lead resistance is known (measured
     * by the R-Test tare sweep, or typed into Settings) the app can put that drop
     * back on every reading. In 4-wire (Kelvin) the sense path carries no
     * current, so there is nothing to add: the reading already belongs to the
     * device under test.
     *
     * Applied ONCE here, before the packet fans out, so the screen and the
     * engines can never disagree about what the voltage is. Port of the
     * firmware's `main.cpp compensateProbe()`.
     */
    private fun compensateProbe(s: El15Status): El15Status {
        val fourWire = Prefs.fourWire(app)
        val tare = Prefs.tareOhm(app)
        if (!s.valid || fourWire || tare <= 0f) return s
        val c = s.copyOf()
        c.voltage = s.voltage + s.current * tare
        c.power = c.voltage * c.current   // keep W consistent with the corrected V
        return c
        // Deliberately NOT touched: `setpoint`. In CV mode that is a value the
        // device regulates at its own terminals — a command, not a measurement —
        // so showing it uncorrected next to a corrected reading is the honest
        // pairing, and the difference between them IS the lead drop.
    }

    // ---- Connection management ------------------------------------------------
    fun startSimulator() {
        // Tear down any real BLE link first — otherwise a connection that is
        // still CONNECTING/CONNECTED keeps polling and its frames interleave with
        // the simulator's through onStatus, corrupting the readouts.
        if (ble.state != El15BleManager.State.IDLE) {
            if (busy) ble.shutdownAndDisconnect() else ble.disconnect()
        }
        simulator?.stop()
        val sim = El15Simulator({ s -> onStatus(s) }, demoEmf, demoSeriesR)
        sim.pollIntervalMs = Prefs.pollMs(app)
        simulator = sim
        sim.start()
        MonitorService.ensureRunning(app)
        each { it.coreStateChanged() }
    }

    fun applyDemoCircuit(emf: Float, r: Float) {
        demoEmf = emf; demoSeriesR = r
        simulator?.let { it.emf = emf; it.seriesR = r }
    }

    /** Stops whichever transport is active, delivering LOAD_OFF safely mid-test. */
    fun disconnect() {
        val wasBusy = busy
        // A user-initiated disconnect is manual control: the guard must not
        // fight it by reconnecting underneath them.
        guard.standDown()
        stopEngines()
        if (simulator != null) {
            simulator?.stop()
            simulator = null
            each { it.coreStateChanged() }
        } else {
            if (wasBusy) ble.shutdownAndDisconnect() else ble.disconnect()
        }
        stopRecording()
        MonitorService.refresh(app)
    }

    fun stopEngines() {
        if (rtest.running) rtest.stop()
        if (batt.running) batt.stop("Stopped")
    }

    /** Push the current settings into the transports and engines. */
    fun syncSettings() {
        val poll = Prefs.pollMs(app)
        ble.pollIntervalMs = poll
        simulator?.pollIntervalMs = poll
        rtest.pollIntervalMs = poll
        rtest.safetyFactor = Prefs.safetyFactor(app)
        rtest.settleMs = Prefs.settleMs(app)
        // Probe wiring is read live for the R-test because its result screen
        // reports which was in force; the capacity test snapshots it at start().
        rtest.fourWire = Prefs.fourWire(app)
        rtest.tareOhm = Prefs.tareOhm(app)
    }

    // ---- Engine entry points --------------------------------------------------
    /**
     * Start a resistance sweep. [tare] runs it as a lead-resistance measurement
     * (probes shorted): the result is stored as the tare rather than reported as
     * a circuit measurement, and no tare is subtracted from it.
     */
    fun startRTest(fuseA: Float, sweepS: Long, startA: Float, maxA: Float, tare: Boolean) {
        if (busy) return
        tareRun = tare
        syncSettings()
        rtest.sweepSeconds = sweepS
        rtest.startCurrent = startA
        rtest.maxCurrent = maxA
        // A tare sweep measures the leads themselves, so it must subtract
        // nothing and must not be treated as 4-wire (which would mean "the leads
        // are already excluded" — the opposite of what is being measured).
        //
        // ONLY a tare sweep. Applied unconditionally, as it was, this overwrote
        // the probe wiring syncSettings() had just read from Settings, so every
        // ordinary sweep ran as untared 2-wire: a measured lead tare was never
        // taken off the slope, and a 4-wire run was labelled 2-wire in its own
        // report. The whole probe-wiring feature was dead for the one test built
        // around it.
        if (tare) {
            rtest.fourWire = false
            rtest.tareOhm = 0f
        }
        // Arm before any current flows: a link that drops between the command and
        // the first status packet must still be recoverable.
        guard.arm(Prefs.FLIGHT_RTEST)
        rtest.start(fuseA)
        each { it.coreStateChanged() }
    }

    fun startBattTest(
        chem: Int, cells: Int, cutoffV: Float, currentA: Float,
        ratedAh: Float, restS: Long,
    ) {
        if (busy) return
        syncSettings()
        batt.chemistry = chem
        batt.cells = cells
        batt.cutoffV = cutoffV
        batt.dischargeA = currentA
        batt.ratedAh = ratedAh
        batt.restS = restS
        // Snapshot the probe wiring so the report describes the run as it was
        // actually recorded, even if Settings change before a (re)save.
        batt.fourWire = Prefs.fourWire(app)
        batt.tareOhm = Prefs.tareOhm(app)
        guard.arm(Prefs.FLIGHT_CAPACITY)
        batt.start()
        each { it.coreStateChanged() }
    }

    // ---- Recording / periodic snapshots ---------------------------------------
    fun startRecording() { recordLog.clear(); recording = true }
    fun stopRecording() { recording = false }

    private fun snapshotFile(): File =
        File(File(app.filesDir, "logs").apply { mkdirs() }, "periodic-log.csv")

    fun snapshotLogFileIfAny(): File? =
        snapshotFile().takeIf { it.exists() && it.length() > 0 }

    fun clearSnapshotLog() { snapshotFile().delete() }

    private fun maybeSnapshot(s: El15Status) {
        val intervalMin = Prefs.snapshotMinutes(app)
        if (intervalMin <= 0) return
        val now = System.currentTimeMillis()
        if (now - lastSnapshotMs < intervalMin * 60_000L) return
        lastSnapshotMs = now
        try {
            val f = snapshotFile()
            if (!f.exists() || f.length() == 0L) {
                f.appendText("time_ms,voltage_V,current_A,power_W,temp_C,fan,mode\n")
            }
            f.appendText(
                "%d,%.4f,%.4f,%.4f,%.2f,%d,%s\n".format(
                    Locale.US, now, s.voltage, s.current, s.power,
                    s.temperature, s.fanSpeed, s.modeName)
            )
        } catch (ignored: Exception) {}
    }

    // ---- Alarms ----------------------------------------------------------------
    private fun checkAlarms(s: El15Status) {
        val lowV = Prefs.alarmLowV(app)
        val highT = Prefs.alarmHighTemp(app)
        val now = System.currentTimeMillis()
        if (now - lastAlarmMs < ALARM_COOLDOWN_MS) return
        val message = when {
            // Only while sinking current: the alarm is for sag under load, not
            // for a resting source that happens to sit below the threshold.
            lowV > 0f && s.loadOn && s.voltage in 0.05f..lowV ->
                "Voltage %.2f V is below the %.2f V alarm threshold".format(s.voltage, lowV)
            highT > 0f && s.temperature >= highT ->
                "Load temperature %.1f °C exceeds the %.1f °C alarm threshold"
                    .format(s.temperature, highT)
            else -> null
        } ?: return
        lastAlarmMs = now
        beep()
        Notifications.alert(app, "EL15 alarm", message)
    }

    /** Tap/confirm/alarm feedback — the phone's stand-in for the board's ES8311. */
    fun beep(tone: Int = ToneGenerator.TONE_PROP_BEEP2, ms: Int = 400) {
        try {
            if (this.tone == null) {
                this.tone = ToneGenerator(AudioManager.STREAM_NOTIFICATION, 85)
            }
            this.tone?.startTone(tone, ms)
        } catch (ignored: Exception) {}
    }

    // ---- Transport listener ----------------------------------------------------
    override fun onStateChanged(state: El15BleManager.State, info: String) {
        if (simulator != null) return
        guard.onConnState(state)
        if (state != El15BleManager.State.CONNECTED && busy) {
            // A dropped link mid-capacity is a reason to stop drawing current,
            // not a reason to throw away hours of measurement: pause and keep
            // every accumulator, so a successful reconnect can carry on. The
            // sweep has no such luxury — its fit depends on an unbroken ramp.
            if (batt.running && batt.pause("BLE link lost")) {
                lastProgressText = "Paused — link lost"
            } else {
                stopEngines()
                lastProgressText = "Stopped (disconnected)"
            }
        }
        if (state == El15BleManager.State.CONNECTED) MonitorService.ensureRunning(app)
        MonitorService.refresh(app)
        each { it.coreStateChanged() }
    }

    override fun onDeviceFound(device: android.bluetooth.BluetoothDevice, name: String) {
        foundDevices[device.address] = device
        each { it.coreDeviceFound(device.address, name) }
    }

    override fun onStatus(status: El15Status) {
        // Packet-inspector frames flow through raw; state consumers gate on valid.
        val c = compensateProbe(status)
        each { it.coreStatus(c) }
        if (!status.valid) return
        lastStatus = c

        // The R-test gets the RAW packet on purpose. Its fit already removes the
        // lead tare from the SLOPE, and a pre-corrected voltage would take the
        // same resistance off a second time — the sweep would report a DUT one
        // tare too low, and a low-milliohm result could land at zero. Every other
        // consumer wants the corrected reading.
        if (rtest.running) rtest.onStatus(status)
        if (batt.running) batt.onStatus(c)

        // Arm/disarm the link supervisor from the load's OWN reported state — the
        // device is the authority on whether current is flowing, not our
        // intentions.
        if (status.loadOn) {
            guard.arm(
                when {
                    batt.running -> Prefs.FLIGHT_CAPACITY
                    rtest.running -> Prefs.FLIGHT_RTEST
                    else -> Prefs.FLIGHT_MANUAL
                }
            )
        } else if (!rtest.running && !batt.running) {
            guard.disarm()
        }

        val curr = if (c.mode == El15Protocol.MODE_DCR) c.dcrI1 else c.current
        val pt = WaveformView.WPoint(
            System.currentTimeMillis(), c.voltage, curr, c.power, c.temperature, c.fanSpeed)
        waveLog.addLast(pt)
        while (waveLog.size > WAVE_CAP) waveLog.removeFirst()
        if (recording) {
            recordLog.addLast(pt)
            while (recordLog.size > RECORD_CAP) recordLog.removeFirst()
        }
        checkAlarms(c)
        maybeSnapshot(c)
        MonitorService.tick(app, c)
    }

    override fun onLog(message: String) {}

    // ---- Resistance-test callbacks ----------------------------------------------
    override fun onTestProgress(
        elapsedS: Float, totalS: Float, target: Float,
        voltage: Float, current: Float, resistance: Float, rValid: Boolean,
    ) {
        lastProgressText = if (rValid) {
            "%.0f/%.0f s  ·  %.3f A set  ·  %.3f V @ %.3f A  ·  R = %s"
                .format(elapsedS, totalS, target, voltage, current, formatOhm(resistance))
        } else {
            "%.0f/%.0f s  ·  %.3f A set  ·  %.3f V @ %.3f A"
                .format(elapsedS, totalS, target, voltage, current)
        }
        each { it.coreRTestProgress(elapsedS, totalS, target, voltage, current, resistance, rValid) }
    }

    override fun onTestComplete(result: ResistanceTest.Result) {
        guard.disarm()
        if (tareRun) {
            // A tare sweep measures the leads: store the raw slope as the lead
            // resistance rather than reporting it as a circuit measurement.
            Prefs.setTareOhm(app, result.rawResistanceOhm)
            lastProgressText = "Lead tare stored — ${formatOhm(result.rawResistanceOhm)}"
            tareRun = false
        } else {
            lastRTestResult = result
            rtestReportName = null   // a new run files under a new number
            lastProgressText = "Done — R = ${formatOhm(result.resistanceOhm)}"
        }
        beep(ToneGenerator.TONE_PROP_ACK, 250)
        Notifications.testDone(
            app, "Resistance test complete", lastProgressText, ResultActivity.KIND_RTEST)
        each { it.coreRTestComplete(result) }
        MonitorService.refresh(app)
    }

    override fun onTestError(message: String) {
        guard.disarm()
        tareRun = false
        lastProgressText = message
        beep(ToneGenerator.TONE_PROP_NACK, 400)
        Notifications.alert(app, "Test stopped", message)
        each { it.coreRTestError(message) }
        MonitorService.refresh(app)
    }

    // ---- Capacity-test callbacks -------------------------------------------------
    override fun onCapacityProgress(
        v: Float, i: Float, ah: Float, wh: Float, temp: Float,
        elapsedS: Long, phase: Int,
    ) {
        lastProgressText = when (phase) {
            2 -> "Resting — %.3f Ah  ·  %.2f Wh".format(ah, wh)
            3 -> "Paused (${batt.pauseReason}) — %.3f Ah".format(ah)
            else -> "%s  ·  %.3f V @ %.3f A  ·  %.3f Ah  ·  %.2f Wh"
                .format(formatDuration(elapsedS), v, i, ah, wh)
        }
        each { it.coreBattProgress(v, i, ah, wh, temp, elapsedS, phase) }
    }

    override fun onCapacityComplete(result: CapacityTest.Result) {
        guard.disarm()
        lastBattResult = result
        battReportName = null
        lastProgressText = "Done — %.3f Ah · %.2f Wh".format(result.capacityAh, result.energyWh)
        beep(ToneGenerator.TONE_PROP_ACK, 250)
        Notifications.testDone(app, "Capacity test complete",
            "${result.stopReason} — $lastProgressText", ResultActivity.KIND_BATT)
        each { it.coreBattComplete(result) }
        MonitorService.refresh(app)
    }

    override fun onCapacityError(message: String) {
        guard.disarm()
        lastProgressText = message
        beep(ToneGenerator.TONE_PROP_NACK, 400)
        Notifications.alert(app, "Test stopped", message)
        each { it.coreBattError(message) }
        MonitorService.refresh(app)
    }

    override fun onCapacityPause(paused: Boolean, reason: String?) {
        each { it.coreBattPause(paused, reason) }
        MonitorService.refresh(app)
    }

    // ---- Link-guard callbacks -----------------------------------------------------
    override fun onGuardAlert(title: String, message: String, resolved: Boolean) {
        lastProgressText = message
        Notifications.alert(app, title, message)
        each { it.coreGuardAlert(title, message, resolved) }
        MonitorService.refresh(app)
    }

    override fun onGuardFailed(message: String) {
        lastProgressText = message
        Notifications.alert(app, "CANNOT REACH THE LOAD", message)
        each { it.coreGuardFailed(message) }
        MonitorService.refresh(app)
    }

    override fun onGuardAlarm() = beep(ToneGenerator.TONE_CDMA_ABBR_ALERT, 600)

    /**
     * Offer recovery if the last session died with the load energised. Called
     * once when the UI first attaches.
     */
    fun checkCrashRecovery() {
        if (busy || isConnected) return
        guard.checkCrashRecovery()
    }

    // ---- Emergency stop --------------------------------------------------------
    /**
     * Kill the load NOW, from any screen or state — the app's stand-in for the
     * board's BOOT-button hardware e-stop. Reachable from the notification and
     * from a pinned control on every screen.
     */
    fun emergencyLoadOff() {
        val wasBusy = busy
        stopEngines()
        // On a real link, push LOAD_OFF straight onto the GATT ahead of any
        // queued writes; the queued setpoint(0) still follows as a backstop.
        if (simulator == null) ble.emergencyOff()
        controller.setLoad(false)
        controller.setSetpoint(0f)
        if (wasBusy) lastProgressText = "Stopped — emergency load OFF"
        beep(ToneGenerator.TONE_CDMA_ABBR_ALERT, 500)
        // Engines were stopped without their normal completion callbacks;
        // broadcast a state change so any attached UI resets its Start/Stop
        // buttons instead of showing a phantom running test.
        each { it.coreStateChanged() }
        MonitorService.refresh(app)
    }

    // ---- Formatting helpers ------------------------------------------------------
    companion object {
        private const val WAVE_CAP = 600
        private const val RECORD_CAP = 200_000
        private const val ALARM_COOLDOWN_MS = 30_000L

        fun formatOhm(ohm: Float): String = when {
            ohm <= 0f -> "n/a"
            ohm < 1f -> "%.1f mΩ".format(ohm * 1000f)
            else -> "%.3f Ω".format(ohm)
        }

        fun formatDuration(seconds: Long): String {
            val h = seconds / 3600
            val m = (seconds % 3600) / 60
            val s = seconds % 60
            return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%d:%02d".format(m, s)
        }

        @Volatile private var instance: DeviceCore? = null

        fun get(context: Context): DeviceCore =
            instance ?: synchronized(this) {
                instance ?: DeviceCore(context.applicationContext).also { instance = it }
            }

        fun peek(): DeviceCore? = instance
    }
}
