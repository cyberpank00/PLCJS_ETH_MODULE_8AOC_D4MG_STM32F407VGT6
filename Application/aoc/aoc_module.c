/**
  ******************************************************************************
  * @file    aoc_module.c
  * @brief   Eight-channel current-output logic (see aoc_module.h).
  ******************************************************************************
  */

#include "aoc_module.h"

#include <math.h>
#include <stddef.h>

#include "cmsis_os.h"
#include "calstore.h"
#include "dac80508.h"
#include "main.h"
#include "mcp23s17.h"
#include "settings.h"
#include "stm32f4xx_hal.h"

/* Rail bring-up: URB2424S start-up + DAC/expander power-on reset. */
#define AOC_RAIL_SETTLE_MS          150u
/* Rail off time during a power-cycle (lets the isolated caps discharge). */
#define AOC_RAIL_OFF_MS             500u
/* Consecutive failed liveness polls before an automatic power-cycle, and the
 * minimum spacing between automatic cycles. */
#define AOC_DEAD_POLLS_TO_CYCLE     5u
#define AOC_AUTO_CYCLE_MIN_MS       10000u

/* Channel status LED blink half-period on fault, ms. */
#define AOC_FAULT_BLINK_MS          100u

typedef struct {
    GPIO_TypeDef* port;
    uint16_t      pin;
} gpio_ref_t;

static const gpio_ref_t s_stat[AOC_CHANNEL_COUNT] = {
    { AO0_STAT_GPIO_Port, AO0_STAT_Pin }, { AO1_STAT_GPIO_Port, AO1_STAT_Pin },
    { AO2_STAT_GPIO_Port, AO2_STAT_Pin }, { AO3_STAT_GPIO_Port, AO3_STAT_Pin },
    { AO4_STAT_GPIO_Port, AO4_STAT_Pin }, { AO5_STAT_GPIO_Port, AO5_STAT_Pin },
    { AO6_STAT_GPIO_Port, AO6_STAT_Pin }, { AO7_STAT_GPIO_Port, AO7_STAT_Pin },
};

/* Runtime, derived from settings on aoc_module_apply_config(). */
static aoc_channel_status_t s_status[AOC_CHANNEL_COUNT];
static bool     s_enabled[AOC_CHANNEL_COUNT];
static uint8_t  s_loss_mode[AOC_CHANNEL_COUNT];
static uint16_t s_safe_ua[AOC_CHANNEL_COUNT];
static float    s_scale_lo[AOC_CHANNEL_COUNT];   /* mA */
static float    s_scale_hi[AOC_CHANNEL_COUNT];   /* mA */
static uint16_t s_setpoint_ua[AOC_CHANNEL_COUNT];
static uint16_t s_poll_ms       = SETTINGS_DEF_POLL_MS;
static uint32_t s_loss_timeout_ms = SETTINGS_DEF_LOSS_TIMEOUT * 100u;
static float    s_code_per_ma;                   /* DAC codes per mA (nominal) */

/* Module state. */
static volatile bool s_dirty         = true;    /* setpoints/config changed     */
static volatile bool s_cycle_request = false;
static bool     s_power_on           = false;
static bool     s_dac_ok             = false;
static bool     s_mcp_ok             = false;
static bool     s_loss_active        = false;
static uint8_t  s_ef_mask            = 0u;
static uint8_t  s_dead_polls         = 0u;
static uint32_t s_last_poll_tick     = 0u;
static uint32_t s_last_cycle_tick    = 0u;
static uint8_t  s_on_mask            = 0u;

/* Loss timer source (modbus_app.c). */
uint32_t modbus_app_last_request_tick(void);

/* LED blink state. */
static uint16_t s_blink_timer;
static uint8_t  s_blink_on;

static inline void stat_set(uint8_t ch, bool on)
{
    HAL_GPIO_WritePin(s_stat[ch].port, s_stat[ch].pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static inline void rail_set(bool on)
{
    HAL_GPIO_WritePin(PVD_CTRL_GPIO_Port, PVD_CTRL_Pin, on ? PVD_CTRL_ON_LEVEL : PVD_CTRL_OFF_LEVEL);
    s_power_on = on;
}

/* ---------------------------------------------------------------------------
 * Settings → runtime
 * ------------------------------------------------------------------------- */
static void load_settings(void)
{
    const settings_t* s = settings_get();

    s_poll_ms = s->poll_ms;
    if (s_poll_ms < SETTINGS_POLL_MS_MIN) { s_poll_ms = SETTINGS_POLL_MS_MIN; }
    if (s_poll_ms > SETTINGS_POLL_MS_MAX) { s_poll_ms = SETTINGS_POLL_MS_MAX; }
    s_loss_timeout_ms = (uint32_t)s->loss_timeout_x100ms * 100u;

    /* code = I[mA]/1000 · R_SET/10 / V_REF · 65535 */
    float rset = s->rset_nominal, vref = s->vref_nominal;
    if (!(rset > 1.0f))  { rset = SETTINGS_DEF_RSET; }
    if (!(vref > 0.1f))  { vref = SETTINGS_DEF_VREF; }
    s_code_per_ma = (rset / 10.0f / 1000.0f) / vref * 65535.0f;

    for (uint8_t ch = 0; ch < AOC_CHANNEL_COUNT; ch++) {
        s_enabled[ch]   = (s->ch_enabled[ch] != 0u);
        s_loss_mode[ch] = (s->ch_loss_mode[ch] <= SETTINGS_LOSS_OFF) ? s->ch_loss_mode[ch] : SETTINGS_LOSS_HOLD;
        s_safe_ua[ch]   = (s->ch_safe_ua[ch] <= SETTINGS_SCALE_MAX_UA) ? s->ch_safe_ua[ch] : 0u;

        float lo = (float)s->ch_scale_lo_ua[ch] / 1000.0f;
        float hi = (float)s->ch_scale_hi_ua[ch] / 1000.0f;
        if (!(hi > lo)) { lo = 4.0f; hi = 20.0f; }
        s_scale_lo[ch] = lo;
        s_scale_hi[ch] = hi;
    }
}

/* ---------------------------------------------------------------------------
 * Isolated side bring-up
 * ------------------------------------------------------------------------- */
static void analog_bringup(void)
{
    rail_set(true);
    osDelay(AOC_RAIL_SETTLE_MS);
    s_dac_ok = dac80508_init();     /* soft reset, ×1 gain, all outputs 0 */
    s_mcp_ok = mcp23s17_init();     /* all channels OFF */
    s_on_mask    = 0u;
    s_dead_polls = 0u;
    s_dirty      = true;            /* re-apply setpoints and ON mask */
}

static void power_cycle(void)
{
    /* Outputs are inherently 0 with the rail down; just mark the state. */
    rail_set(false);
    s_dac_ok = false;
    s_mcp_ok = false;
    for (uint8_t ch = 0; ch < AOC_CHANNEL_COUNT; ch++) {
        s_status[ch].fault      = true;
        s_status[ch].fault_code = AOC_FAULT_NO_POWER;
        s_status[ch].output_on  = false;
        s_status[ch].i_cmd_ma   = 0.0f;
    }
    osDelay(AOC_RAIL_OFF_MS);
    analog_bringup();
    s_last_cycle_tick = osKernelGetTickCount();
}

/* ---------------------------------------------------------------------------
 * Setpoint → DAC
 * ------------------------------------------------------------------------- */
static uint16_t ma_to_code(uint8_t ch, float i_set_ma)
{
    const float i_cmd = calstore_gain(ch, 0u) * i_set_ma + calstore_offset(ch, 0u);
    float code = i_cmd * s_code_per_ma;
    if (code < 0.0f)     { code = 0.0f; }
    if (code > 65535.0f) { code = 65535.0f; }
    return (uint16_t)lroundf(code);
}

/* Compute and push every channel's DAC code and the ON mask. */
static void apply_outputs(void)
{
    uint8_t on_mask = 0u;

    for (uint8_t ch = 0; ch < AOC_CHANNEL_COUNT; ch++) {
        aoc_channel_status_t* st = &s_status[ch];
        st->enabled     = s_enabled[ch];
        st->setpoint_ua = s_setpoint_ua[ch];
        st->loss_active = s_loss_active;

        bool  drive = s_enabled[ch] && s_power_on;
        float i_ma  = (float)s_setpoint_ua[ch] / 1000.0f;

        if (drive && s_loss_active) {
            switch (s_loss_mode[ch]) {
            case SETTINGS_LOSS_SAFE: i_ma  = (float)s_safe_ua[ch] / 1000.0f; break;
            case SETTINGS_LOSS_OFF:  drive = false;                          break;
            default:                 /* HOLD */                              break;
            }
        }

        st->output_on = drive;
        st->i_cmd_ma  = drive ? i_ma : 0.0f;
        st->dac_code  = drive ? ma_to_code(ch, i_ma) : 0u;
        if (drive) { on_mask |= (uint8_t)(1u << ch); }
    }

    if (s_dac_ok) {
        for (uint8_t ch = 0; ch < AOC_CHANNEL_COUNT; ch++) {
            dac80508_set_code(ch, s_status[ch].dac_code);
        }
    }
    if (s_mcp_ok) {
        mcp23s17_set_on_mask(on_mask);
    }
    s_on_mask = on_mask;
}

/* ---------------------------------------------------------------------------
 * Fault / liveness poll
 * ------------------------------------------------------------------------- */
static void poll_faults(void)
{
    uint8_t ef = 0u;
    s_mcp_ok = mcp23s17_read_faults(&ef);
    s_dac_ok = dac80508_alive();
    s_ef_mask = ef;

    for (uint8_t ch = 0; ch < AOC_CHANNEL_COUNT; ch++) {
        aoc_channel_status_t* st = &s_status[ch];
        uint8_t code = AOC_FAULT_NONE;
        if (!s_power_on)                 { code = AOC_FAULT_NO_POWER; }
        else if (!s_mcp_ok)              { code = AOC_FAULT_MCP; }
        else if (!s_dac_ok)              { code = AOC_FAULT_DAC; }
        else if (st->output_on && ((ef >> ch) & 1u)) { code = AOC_FAULT_EF; }
        st->fault      = (code != AOC_FAULT_NONE);
        st->fault_code = code;
    }

    if (!s_mcp_ok || !s_dac_ok) {
        if (s_dead_polls < 255u) { s_dead_polls++; }
    } else {
        s_dead_polls = 0u;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void aoc_module_init(void)
{
    const settings_t* s = settings_get();

    for (uint8_t ch = 0; ch < AOC_CHANNEL_COUNT; ch++) {
        stat_set(ch, false);
        s_setpoint_ua[ch] = (s->ch_setpoint_ua[ch] <= SETTINGS_SCALE_MAX_UA)
                                ? s->ch_setpoint_ua[ch] : 0u;   /* power-on value */
    }
    load_settings();
    analog_bringup();
    apply_outputs();
    poll_faults();
    s_last_poll_tick = osKernelGetTickCount();
    s_dirty = false;
}

void aoc_module_apply_config(void)
{
    load_settings();
    s_dirty = true;
}

void aoc_module_tick(void)
{
    const uint32_t now = osKernelGetTickCount();

    /* Operator-requested or automatic rail power-cycle. */
    const bool auto_cycle = (s_dead_polls >= AOC_DEAD_POLLS_TO_CYCLE) &&
                            ((now - s_last_cycle_tick) >= AOC_AUTO_CYCLE_MIN_MS);
    if (s_cycle_request || auto_cycle) {
        s_cycle_request = false;
        power_cycle();
    }

    /* Comms-loss timer: no valid request from any client for the timeout. */
    bool loss = false;
    if (s_loss_timeout_ms != 0u) {
        loss = (now - modbus_app_last_request_tick()) >= s_loss_timeout_ms;
    }
    if (loss != s_loss_active) {
        s_loss_active = loss;
        s_dirty = true;
    }

    if (s_dirty) {
        s_dirty = false;
        apply_outputs();
    }

    if ((now - s_last_poll_tick) >= s_poll_ms) {
        s_last_poll_tick = now;
        poll_faults();
    }
}

void aoc_module_led_tick(uint16_t period_ms)
{
    s_blink_timer = (uint16_t)(s_blink_timer + period_ms);
    if (s_blink_timer >= AOC_FAULT_BLINK_MS) {
        s_blink_timer = 0u;
        s_blink_on    = (uint8_t)(!s_blink_on);
    }

    for (uint8_t ch = 0; ch < AOC_CHANNEL_COUNT; ch++) {
        if (!s_enabled[ch]) {
            stat_set(ch, false);
        } else if (s_status[ch].fault) {
            stat_set(ch, s_blink_on != 0u);
        } else {
            stat_set(ch, s_status[ch].output_on);
        }
    }
}

const aoc_channel_status_t* aoc_module_get_status(uint8_t ch)
{
    return (ch < AOC_CHANNEL_COUNT) ? &s_status[ch] : NULL;
}

void aoc_module_set_setpoint_ua(uint8_t ch, uint16_t ua)
{
    if (ch >= AOC_CHANNEL_COUNT) { return; }
    if (ua > SETTINGS_SCALE_MAX_UA) { ua = SETTINGS_SCALE_MAX_UA; }
    s_setpoint_ua[ch] = ua;
    settings_get()->ch_setpoint_ua[ch] = ua;   /* persisted only by SAVE */
    s_dirty = true;
}

void aoc_module_set_setpoint_i16(uint8_t ch, int16_t v)
{
    if (ch >= AOC_CHANNEL_COUNT) { return; }
    float f = (float)v;
    if (f < 0.0f) { f = 0.0f; }
    const float ma = s_scale_lo[ch] + f / AOC_I16_FULL * (s_scale_hi[ch] - s_scale_lo[ch]);
    aoc_module_set_setpoint_ua(ch, (uint16_t)lroundf(ma * 1000.0f));
}

int16_t aoc_module_get_setpoint_i16(uint8_t ch)
{
    if (ch >= AOC_CHANNEL_COUNT) { return 0; }
    const float ma   = (float)s_setpoint_ua[ch] / 1000.0f;
    float v = (ma - s_scale_lo[ch]) / (s_scale_hi[ch] - s_scale_lo[ch]) * AOC_I16_FULL;
    if (v >  AOC_I16_FULL) { v =  AOC_I16_FULL; }
    if (v < -AOC_I16_FULL) { v = -AOC_I16_FULL; }
    return (int16_t)lroundf(v);
}

void aoc_module_request_power_cycle(void)
{
    s_cycle_request = true;
}

bool aoc_module_analog_ok(void)
{
    return s_power_on && s_dac_ok && s_mcp_ok;
}

uint8_t aoc_module_loop_power_on(void)
{
    return s_power_on ? 1u : 0u;
}

bool aoc_module_loss_active(void)
{
    return s_loss_active;
}
