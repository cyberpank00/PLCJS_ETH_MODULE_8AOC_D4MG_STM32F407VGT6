/**
  ******************************************************************************
  * @file    modbus_app.h
  * @brief   Modbus register-map adapter for the 8AOC analog current-output module.
  *
  *  Float values are IEEE-754 32-bit, transmitted as two 16-bit registers with
  *  the HIGH word first (big-endian word order: register[N] = bits 31..16).
  *  Multi-channel quantities are grouped by quantity: 8 consecutive registers
  *  (or 8 register pairs) are channels 0..7.
  *
  *  ---- Holding Registers (FC03/06/16) — compact per-channel block --------
  *      0..7    setpoint, int16 (RW): 0..32767 = scale_lo..scale_hi of the
  *                channel (negative clamps to scale_lo)
  *      8..15   scale low threshold, µA  (default 4000;  0..22000, < high)
  *      16..23  scale high threshold, µA (default 20000; ..22000, > low)
  *      24..31  enabled (0/1, default 1) — disabled = 0 mA, XTR111 off
  *      32..39  comms-loss mode: 0 HOLD (default), 1 SAFE (go to 40..47), 2 OFF
  *      40..47  comms-loss safe value, µA (default 0)
  *      48..55  setpoint, µA (RW): 0..22000 — alternative to 0..7, the last
  *                write through either view wins
  *    Setpoints are live; SAVE (HR117) also stores them as power-on values.
  *
  *  ---- Input Registers (FC04, read-only) — grouped by quantity ----------
  *      300..315 commanded output current, float32 mA ×8 (0 when the output
  *                is off; includes the comms-loss substitution)
  *      316..323 status flags ×8: bit0 enabled, bit1 output on, bit2 fault,
  *                bit3 comms-loss active; bits15..8 fault code
  *                (1 XTR111 EF: open loop / compliance / over-temp,
  *                 2 GPIO expander dead, 3 DAC dead, 4 loop rail off)
  *      324..331 DAC code ×8 (0..65535)
  *      332      loop rail on (0/1)         333 comms-loss active (0/1)
  *    Global:
  *      120 fw major, 121 fw minor, 122/123 uptime s (lo/hi),
  *      125 module id (0x08A0), 126 on-chip temperature (signed 0.1 °C)
  *      127 calibration lock bitmask (bit = channel)
  *
  *  ---- Holding Registers (FC03/06/16) — global -----------------------------
  *      100 comms-loss timeout ×100 ms (0 = off, default 50 = 5 s)
  *      101 LED mode (0/1/2)                   102 Modbus slave id
  *      103 Modbus TCP port
  *      104..107 static IP octets              108..111 netmask octets
  *      112..115 gateway octets                116 net mode (0 static/1 DHCP/2 LL)
  *      117 SAVE trigger (0xA5A5)              118 REBOOT (0xB00B) / BOOT (0xB007)
  *                                                 / KSZ8863 reset (0x8863)
  *                                                 / analog rail power-cycle (0xA0FF)
  *      119 FACTORY RESET trigger (0xDEAD)     130 on-chip temperature (RO)
  *      131 CAL COMMIT trigger (0xCA00|ch)     132 CAL ERASE ARM (0xC1A5)
  *      133 EF / liveness poll period ms (20..5000, default 100)
  *    Calibration coefficients (float32), base 540 + ch*4 (WRITE-ONCE):
  *      +0..1 gain,  +2..3 offset (mA):  I_cmd = gain·I_set + offset
  *      Writes are a live preview and are rejected once the channel slot is
  *      committed. Commit = write 0xCA00|ch to register 131.
  *    Nominal R_SET (float32 Ω): 620..621     Nominal V_REF (float32 V): 622..623
  ******************************************************************************
  */
#ifndef APPLICATION_MODBUS_APP_H
#define APPLICATION_MODBUS_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "nanomodbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Magic write triggers (FC06/FC10 to specific holding registers). */
#define MODBUS_TRIG_SAVE            0xA5A5u
#define MODBUS_TRIG_REBOOT          0xB00Bu
#define MODBUS_TRIG_FACTORY_RESET   0xDEADu
#define MODBUS_TRIG_BOOTLOADER      0xB007u
/* Hardware-reset the KSZ8863 Ethernet switch (operator recovery command). */
#define MODBUS_TRIG_SWITCH_RESET    0x8863u
/* Power-cycle the isolated analog rail (DAC + expander + loop supply). All
 * outputs drop to 0 mA for ~0.7 s. Operator recovery command. */
#define MODBUS_TRIG_ANALOG_CYCLE    0xA0FFu

/* Calibration commit: value = MB_CAL_COMMIT_BASE | ch, slot 0..7. */
#define MB_CAL_COMMIT_BASE          0xCA00u
#define MB_CAL_COMMIT_SLOT_MASK     0x00FFu

/* Emergency calibration-erase arming magic (two-factor: this + button). */
#define MODBUS_TRIG_CAL_ERASE_ARM   0xC1A5u

/* Window after arming during which a physical button confirm erases the
 * calibration sector, and the button hold required to confirm (ms). */
#define CAL_ERASE_ARM_WINDOW_MS     30000u
#define CAL_ERASE_CONFIRM_MS        3000u

/* No-init RAM cell shared with the bootloader. */
#define BOOT_REQUEST_FLAG_ADDR      0x2001FFF0u
#define BOOT_REQUEST_MAGIC          0xB007CAFEu

/* ---- Global holding registers ---- */
#define MB_HR_LOSS_TIMEOUT          100u
#define MB_HR_LED_MODE              101u
#define MB_HR_SLAVE_ID              102u
#define MB_HR_TCP_PORT              103u
#define MB_HR_IP_BASE               104u
#define MB_HR_NETMASK_BASE          108u
#define MB_HR_GATEWAY_BASE          112u
#define MB_HR_USE_DHCP              116u
#define MB_HR_TRIG_SAVE             117u
#define MB_HR_TRIG_REBOOT           118u
#define MB_HR_TRIG_FACTORY_RESET    119u
#define MB_HR_TEMPERATURE           130u
#define MB_HR_CAL_COMMIT            131u
#define MB_HR_CAL_ERASE_ARM         132u
#define MB_HR_POLL_MS               133u

#define MB_LOSS_TIMEOUT_MAX         6000u   /* ×100 ms = 10 min */

/* ---- Global input registers ---- */
#define MB_IR_FW_VER_MAJOR          120u
#define MB_IR_FW_VER_MINOR          121u
#define MB_IR_UPTIME_LO             122u
#define MB_IR_UPTIME_HI             123u
#define MB_IR_MODULE_ID             125u
#define MB_IR_TEMPERATURE           126u
#define MB_IR_CAL_LOCK              127u

#define MB_AO_CHANNELS              8u

/* ---- Compact per-channel block (holding): address = group*8 + ch ---- */
#define MB_HR_CH_BASE               0u
#define MB_HR_CH_GROUP_SETPOINT     0u    /* int16 scaled, RW */
#define MB_HR_CH_GROUP_SCALE_LO     1u    /* µA */
#define MB_HR_CH_GROUP_SCALE_HI     2u    /* µA */
#define MB_HR_CH_GROUP_ENABLED      3u
#define MB_HR_CH_GROUP_LOSS_MODE    4u
#define MB_HR_CH_GROUP_SAFE_UA      5u
#define MB_HR_CH_GROUP_SETPOINT_UA  6u    /* µA, RW */
#define MB_HR_CH_GROUPS             7u    /* registers 0..55 */

/* ---- Readings (input registers), grouped by quantity ---- */
#define MB_IR_AO_BASE               300u
#define MB_IR_AO_CURRENT            300u  /* float32 ×8 -> 300..315 */
#define MB_IR_AO_FLAGS              316u  /* u16 ×8     -> 316..323 */
#define MB_IR_AO_CODE               324u  /* u16 ×8     -> 324..331 */
#define MB_IR_AO_RAIL_ON            332u
#define MB_IR_AO_LOSS_ACTIVE        333u
#define MB_IR_AO_END                334u  /* first address past the block */

/* Status flag bits (registers 316..323). */
#define MB_AO_FLAG_ENABLED          0x0001u
#define MB_AO_FLAG_OUTPUT_ON        0x0002u
#define MB_AO_FLAG_FAULT            0x0004u
#define MB_AO_FLAG_LOSS             0x0008u

/* ---- Calibration coefficients (holding, float32) ---- */
#define MB_HR_AO_CAL_BASE           540u
#define MB_HR_AO_CAL_STRIDE         4u    /* gain, offset floats per channel */

/* ---- Nominal R_SET / V_REF (holding, float32) ---- */
#define MB_HR_RSET_BASE             620u  /* 620..621 */
#define MB_HR_VREF_BASE             622u  /* 622..623 */

/* Module ID (input register 125). */
#define MODULE_ID_08AOC             0x08A0u

/** Initialise the modbus register adapter. Must be called after settings_init(). */
void modbus_app_init(void);

/** Mark a successful modbus transaction (used to drive STAT_LED state). */
void modbus_app_notify_request(void);

/** Get the populated nmbs_callbacks structure for nmbs_server_create(). */
const nmbs_callbacks* modbus_app_get_callbacks(void);

/**
 * Returns 1 if an emergency calibration erase has been armed over Modbus and
 * the arming window (CAL_ERASE_ARM_WINDOW_MS) has not yet expired. Used by the
 * application loop to gate the physical button confirmation. Non-destructive.
 */
uint8_t modbus_app_cal_erase_armed(void);

/** Clear the armed calibration-erase state (e.g. after the action completes). */
void modbus_app_clear_cal_erase_arm(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_MODBUS_APP_H */
