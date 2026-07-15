package com.loadtester.el15

/**
 * Minimal command surface for an EL15 load, shared by the real BLE transport
 * ([El15BleManager]) and the built-in demo device ([El15Simulator]). Lets the
 * manual controls and the resistance test drive either one interchangeably.
 */
interface El15Controller {
    fun setMode(mode: Int)
    fun setSetpoint(value: Float)
    fun setLoad(on: Boolean)
    fun setLock()
}
