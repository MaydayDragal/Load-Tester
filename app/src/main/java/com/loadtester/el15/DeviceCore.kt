package com.loadtester.el15

import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.media.ToneGenerator
import java.io.File
import java.util.Locale

/**
 * Process-lifetime hub that owns the device session: BLE transport, demo
 * simulator, test engines, the live sample log, threshold alarms, and periodic
 * logging. Both [MainActivity] (UI) and [MonitorService] (foreground
 * notification, widget/tile updates) attach to this single instance, so a
 * running connection, sweep, or battery test survives the Activity being
 * destroyed — the UI is a view over the core, not its owner.
 *
 * All state is main-thread confined (the transports already deliver on main).
 */
class DeviceCore private constructor(private val app: Context) :
    El15BleManager.Listener, CircuitResistanceTester.Callback {

    /** Everything the UI renders; every callback arrives on the main thread. */
    interface Ui {
        fun coreStateChanged() {}
        fun coreDeviceFound(address: String, name: String) {}
        fun coreStatus(status: El15Status) {}
        fun coreTestProgress(text: String, percent: Int) {}
        fun coreTestComplete(recordId: String, summary: String) {}
        fun coreTestError(message: String) {}
        fun coreCalComplete(result: SweepCalibrator.CalResult) {}
        fun coreSessionProgress(text: String) {}
        fun coreSessionComplete(recordId: String, summary: String) {}
        fun coreSessionError(message: String) {}
    }

    val ble = El15BleManager(app).also { it.listener = this }
    var simulator: El15Simulator? = null
        private set
    var demoEmf = Prefs.demoEmf(app)
    var demoSeriesR = Prefs.demoR(app)

    val controller: El15Controller get() = simulator ?: ble
    private val activeController = object : El15Controller {
        override fun setMode(mode: Int) = controller.setMode(mode)
        override fun setSetpoint(value: Float) = controller.setSetpoint(value)
        override fun setLoad(on: Boolean) = controller.setLoad(on)
        override fun setLock() = controller.setLock()
    }

    val tester = CircuitResistanceTester(activeController, this)
    val calibrator = SweepCalibrator(activeController, calCallbackRelay())
    val session = LongTestEngine(activeController, sessionCallbackRelay())

    var lastStatus: El15Status? = null
        private set
    var lastProgressText: String = ""
        private set

    /** Rolling display buffer + optional full recording (survives UI death). */
    val waveLog = ArrayDeque<WaveformView.WPoint>()
    val recordLog = ArrayDeque<WaveformView.WPoint>()
    var recording = false
        private set

    private val listeners = LinkedHashSet<Ui>()
    private var tone: ToneGenerator? = null
    private var lastAlarmMs = 0L
    private var lastSnapshotMs = 0L

    val isConnected: Boolean get() = simulator != null || ble.state == El15BleManager.State.CONNECTED
    val isDemo: Boolean get() = simulator != null
    val busy: Boolean get() = tester.running || calibrator.running || session.running

    fun addUi(ui: Ui) { listeners.add(ui); }
    fun removeUi(ui: Ui) { listeners.remove(ui) }
    private fun each(block: (Ui) -> Unit) { listeners.toList().forEach(block) }

    // ---- Connection management ---------------------------------------------
    fun startSimulator() {
        // Tear down any real BLE link first — otherwise a connection that is
        // still CONNECTING/CONNECTED keeps polling and its frames interleave
        // with the simulator's through onStatus, corrupting the readouts.
        if (ble.state != El15BleManager.State.IDLE) {
            if (busy) ble.shutdownAndDisconnect() else ble.disconnect()
        }
        simulator?.stop()
        val sim = El15Simulator({ s -> onStatus(s) }, demoEmf, demoSeriesR)
        sim.pollIntervalMs = Prefs.pollMs(app)
        simulator = sim
        sim.start()
        MonitorService.ensureRunning(app)
        LiveWidget.refresh(app)
        each { it.coreStateChanged() }
    }

    fun applyDemoCircuit(emf: Float, r: Float) {
        demoEmf = emf; demoSeriesR = r
        simulator?.let { it.emf = emf; it.seriesR = r }
    }

    /** Stops whichever transport is active, delivering LOAD_OFF safely mid-test. */
    fun disconnect() {
        val wasBusy = busy
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
        LiveWidget.refresh(app)
    }

    fun stopEngines() {
        if (calibrator.running) calibrator.stop()
        if (tester.running) tester.stop()
        if (session.running) session.stop()
    }

    fun syncSettings() {
        val poll = Prefs.pollMs(app)
        ble.pollIntervalMs = poll
        simulator?.pollIntervalMs = poll
        tester.pollIntervalMs = poll
        tester.safetyFactor = Prefs.safetyFactor(app)
        calibrator.pollIntervalMs = poll
        calibrator.safetyFactor = Prefs.safetyFactor(app)
        session.pollIntervalMs = poll
        session.safetyFactor = Prefs.safetyFactor(app)
    }

    // ---- Recording / periodic snapshots --------------------------------------
    fun startRecording() { recordLog.clear(); recording = true }
    fun stopRecording() { recording = false }

    private fun snapshotFile(): File =
        File(File(app.filesDir, "logs").apply { mkdirs() }, "periodic-log.csv")

    fun snapshotLogFileIfAny(): File? = snapshotFile().takeIf { it.exists() && it.length() > 0 }

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
                "Load temperature %.1f °C exceeds the %.1f °C alarm threshold".format(s.temperature, highT)
            else -> null
        } ?: return
        lastAlarmMs = now
        beep()
        Notifications.alert(app, "EL15 alarm", message)
    }

    private fun beep() {
        try {
            if (tone == null) tone = ToneGenerator(AudioManager.STREAM_NOTIFICATION, 85)
            tone?.startTone(ToneGenerator.TONE_PROP_BEEP2, 400)
        } catch (ignored: Exception) {}
    }

    // ---- Transport listener ----------------------------------------------------
    override fun onStateChanged(state: El15BleManager.State, info: String) {
        if (simulator != null) return
        if (state != El15BleManager.State.CONNECTED && busy) {
            stopEngines()
            lastProgressText = "Stopped (disconnected)"
        }
        if (state == El15BleManager.State.CONNECTED) MonitorService.ensureRunning(app)
        MonitorService.refresh(app)
        LiveWidget.refresh(app)
        each { it.coreStateChanged() }
    }

    override fun onDeviceFound(device: android.bluetooth.BluetoothDevice, name: String) {
        foundDevices[device.address] = device
        each { it.coreDeviceFound(device.address, name) }
    }

    val foundDevices = LinkedHashMap<String, android.bluetooth.BluetoothDevice>()

    override fun onStatus(status: El15Status) {
        // Packet-inspector frames flow through; state consumers gate on valid.
        each { it.coreStatus(status) }
        if (!status.valid) return
        lastStatus = status
        if (tester.running) tester.onStatus(status)
        if (calibrator.running) calibrator.onStatus(status)
        if (session.running) session.onStatus(status)

        val curr = if (status.mode == El15Protocol.MODE_DCR) status.dcrI1 else status.current
        val pt = WaveformView.WPoint(System.currentTimeMillis(), status.voltage, curr,
            status.power, status.temperature, status.fanSpeed)
        waveLog.addLast(pt)
        while (waveLog.size > WAVE_CAP) waveLog.removeFirst()
        if (recording) {
            recordLog.addLast(pt)
            while (recordLog.size > RECORD_CAP) recordLog.removeFirst()
        }
        checkAlarms(status)
        maybeSnapshot(status)
        MonitorService.tick(app, status)
        LiveWidget.push(app, status)
    }

    override fun onLog(message: String) {}

    // ---- Resistance-test callbacks ----------------------------------------------
    override fun onTestProgress(step: Int, totalSteps: Int, targetCurrent: Float, voltage: Float, current: Float) {
        lastProgressText = "Step %d/%d  →  %.3f A set   %.3f V @ %.3f A"
            .format(step, totalSteps, targetCurrent, voltage, current)
        each { it.coreTestProgress(lastProgressText, step * 100 / totalSteps) }
    }

    override fun onTestComplete(result: CircuitResistanceTester.ResistanceResult) {
        val device = if (isDemo) "Demo (%.1f V, %.3f Ω)".format(demoEmf, demoSeriesR) else "EL15 (BLE)"
        val record = TestRecord.from(result, tester.steps, tester.settleMs, tester.collectMs,
            (tester.safetyFactor * 100).toInt(), device, System.currentTimeMillis())
        val repo = TestRepository(app)
        val saved = repo.save(record)
        repo.applyRetention(Prefs.retention(app))
        val ohm = formatOhm(result.resistanceOhm)
        lastProgressText = "Done — R = $ohm"
        if (saved) {
            Notifications.testDone(app, "Resistance test complete", "R = $ohm — saved to History",
                record.id, session = false)
            each { it.coreTestComplete(record.id, ohm) }
        } else {
            each { it.coreTestError("Could not archive the test result") }
        }
        MonitorService.refresh(app)
    }

    override fun onTestError(message: String) {
        lastProgressText = message
        Notifications.alert(app, "Test stopped", message)
        each { it.coreTestError(message) }
        MonitorService.refresh(app)
    }

    private fun calCallbackRelay() = object : SweepCalibrator.Callback {
        override fun onCalProgress(message: String) {
            lastProgressText = message
            each { it.coreTestProgress(message, -1) }
        }
        override fun onCalComplete(result: SweepCalibrator.CalResult) {
            lastProgressText = "Calibration done — recommended ${result.steps} steps"
            each { it.coreCalComplete(result) }
            MonitorService.refresh(app)
        }
        override fun onCalError(message: String) {
            lastProgressText = message
            each { it.coreTestError(message) }
            MonitorService.refresh(app)
        }
    }

    private fun sessionCallbackRelay() = object : LongTestEngine.Callback {
        override fun onSessionProgress(text: String) {
            lastProgressText = text
            each { it.coreSessionProgress(text) }
        }
        override fun onSessionComplete(record: SessionRecord) {
            val repo = SessionRepository(app)
            val saved = repo.save(record)
            repo.applyRetention(Prefs.retention(app))
            lastProgressText = "Done — ${record.headline()}"
            if (saved) {
                Notifications.testDone(app, "${record.typeName()} complete",
                    "${record.headline()} — saved to History", record.id, session = true)
                each { it.coreSessionComplete(record.id, record.headline()) }
            } else {
                each { it.coreSessionError("Could not archive the session") }
            }
            MonitorService.refresh(app)
        }
        override fun onSessionError(message: String) {
            lastProgressText = message
            Notifications.alert(app, "Test stopped", message)
            each { it.coreSessionError(message) }
            MonitorService.refresh(app)
        }
    }

    private fun formatOhm(ohm: Float): String = when {
        ohm <= 0f -> "n/a"
        ohm < 1f -> "%.1f mΩ".format(ohm * 1000f)
        else -> "%.3f Ω".format(ohm)
    }

    /** Emergency load-off from notification / widget / tile. */
    fun emergencyLoadOff() {
        val wasBusy = busy
        stopEngines()
        // On a real link, push LOAD_OFF straight onto the GATT ahead of any
        // queued writes; the queued setpoint(0) still follows as a backstop.
        if (simulator == null) ble.emergencyOff()
        controller.setLoad(false)
        controller.setSetpoint(0f)
        if (wasBusy) lastProgressText = "Stopped — emergency load OFF"
        // Engines were stopped without their normal completion callbacks;
        // broadcast a state change so any attached UI resets its Start/Stop
        // buttons instead of showing a phantom running test.
        each { it.coreStateChanged() }
        MonitorService.refresh(app)
        LiveWidget.refresh(app)
    }

    companion object {
        private const val WAVE_CAP = 600
        private const val RECORD_CAP = 200_000
        private const val ALARM_COOLDOWN_MS = 30_000L

        @Volatile private var instance: DeviceCore? = null
        fun get(context: Context): DeviceCore =
            instance ?: synchronized(this) {
                instance ?: DeviceCore(context.applicationContext).also { instance = it }
            }
        fun peek(): DeviceCore? = instance
    }
}
