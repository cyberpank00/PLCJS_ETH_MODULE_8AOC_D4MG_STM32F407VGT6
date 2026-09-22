/**
  ******************************************************************************
  * @file    aoc_module.h
  * @brief   Eight-channel 0–20 mA current-output logic on top of the DAC80508
  *          (setpoints) and MCP23S17 (XTR111 enable / error flags).
  *
  *  Per channel:
  *    - setpoint in µA (from the scaled int16 register or the direct µA one),
  *    - enable, comms-loss mode (HOLD / SAFE / OFF) with a safe value,
  *    - per-channel write-once calibration  I_cmd = gain·I_set + offset,
  *    - DAC code = I_cmd · R_SET / 10 / V_REF · 65535   (XTR111: I = 10·V/R_SET),
  *    - fault from the XTR111 EF line (open loop / out of compliance / over-
  *      temperature) polled through the expander,
  *    - status LED: solid = enabled & output driven, off = disabled,
  *      fast blink = fault.
  *
  *  Module level:
  *    - the isolated 24 V loop rail (PVD_CTRL) also powers the DAC and the
  *      expander, so the bring-up order is rail ON → settle → configure both.
  *      When either device stops answering the module power-cycles the rail
  *      and re-configures (rate limited); the same can be triggered by an
  *      operator over Modbus (HR118 = 0xA0FF),
  *    - comms loss: no valid Modbus request from any client for the configured
  *      timeout applies each channel's loss mode until a request arrives.
  *
  *  All SPI traffic stays on the AOC task: Modbus writes only update RAM and
  *  raise a "dirty" flag that the task services within one tick (10 ms).
  ******************************************************************************
  */
#ifndef APPLICATION_AOC_MODULE_H
#define APPLICATION_AOC_MODULE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOC_CHANNEL_COUNT           8u

/* Fault codes reported in aoc_channel_status_t.fault_code. */
#define AOC_FAULT_NONE              0u
#define AOC_FAULT_EF                1u   /* XTR111 error flag: open loop / compliance / over-temp */
#define AOC_FAULT_MCP               2u   /* GPIO expander not responding                          */
#define AOC_FAULT_DAC               3u   /* DAC not responding                                     */
#define AOC_FAULT_NO_POWER          4u   /* isolated loop rail is off (power-cycle in progress)    */

/* int16 setpoint view (holding registers 0..7). */
#define AOC_I16_FULL                32767.0f

typedef struct {
    bool     enabled;
    bool     output_on;     /* ON line asserted (enabled, powered, not OFF by loss) */
    bool     fault;
    bool     loss_active;   /* comms-loss action currently applied              */
    uint8_t  fault_code;    /* AOC_FAULT_*                                       */
    uint16_t setpoint_ua;   /* operator setpoint (survives loss, restored after)  */
    float    i_cmd_ma;      /* current actually commanded to the DAC, mA (0 when off) */
    uint16_t dac_code;
} aoc_channel_status_t;

/** Power the isolated rail, configure DAC + expander, apply power-on setpoints. */
void aoc_module_init(void);

/** Re-read settings (scales, enables, loss config, nominals) and re-apply. */
void aoc_module_apply_config(void);

/** Periodic service (call every 10 ms from the AOC task): applies pending
 *  setpoints, runs the comms-loss timer, polls EF / liveness at poll_ms,
 *  performs a pending or automatic rail power-cycle. */
void aoc_module_tick(void);

/** Drive the channel status LEDs; call from a fast (e.g. 10 ms) tick. */
void aoc_module_led_tick(uint16_t period_ms);

/** Read-only access to the latest per-channel status (NULL if out of range). */
const aoc_channel_status_t* aoc_module_get_status(uint8_t ch);

/** Set the operator setpoint in µA (clamped to SETTINGS_SCALE_MAX_UA). Also
 *  mirrored into settings so SAVE persists it as the power-on value. */
void aoc_module_set_setpoint_ua(uint8_t ch, uint16_t ua);

/** Set the setpoint from the scaled int16 view: 0..32767 = scale_lo..scale_hi
 *  (negative clamps to scale_lo). */
void aoc_module_set_setpoint_i16(uint8_t ch, int16_t v);

/** Scaled int16 view of the operator setpoint. */
int16_t aoc_module_get_setpoint_i16(uint8_t ch);

/** Request a power-cycle of the isolated rail (performed by the task). */
void aoc_module_request_power_cycle(void);

/** True while the isolated rail is commanded on and both devices answer. */
bool aoc_module_analog_ok(void);

/** Rail state (1 = on). */
uint8_t aoc_module_loop_power_on(void);

/** True while the comms-loss action is applied. */
bool aoc_module_loss_active(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_AOC_MODULE_H */
