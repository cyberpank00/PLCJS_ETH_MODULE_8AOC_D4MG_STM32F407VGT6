/**
  ******************************************************************************
  * @file    settings.h
  * @brief   Persistent settings stored in internal Flash with CRC32 protection.
  *          8AOC variant: network + per-channel current-output configuration
  *          and power-on setpoints. Calibration lives in the write-once
  *          calstore, not here.
  ******************************************************************************
  */
#ifndef APPLICATION_SETTINGS_H
#define APPLICATION_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic and version --------------------------------------------------------- */
#define SETTINGS_MAGIC          0x08A04A57u
#define SETTINGS_VERSION        1u

#define SETTINGS_AO_CHANNELS    8u

/* Defaults ------------------------------------------------------------------ */
#define SETTINGS_DEF_POLL_MS       100u   /* EF / liveness poll period          */
#define SETTINGS_POLL_MS_MIN       20u
#define SETTINGS_POLL_MS_MAX       5000u

#define SETTINGS_DEF_LED_MODE       2u    /* STATE_MACHINE */

#define SETTINGS_DEF_SLAVE_ID       1u
#define SETTINGS_DEF_TCP_PORT       502u

/* Network mode stored in settings.use_dhcp (field name kept for parity with
 * the sibling modules): 0 = STATIC, 1 = DHCP, 2 = LINK-LOCAL (169.254/16). */
#define NET_MODE_STATIC            0u
#define NET_MODE_DHCP              1u
#define NET_MODE_LINKLOCAL         2u

#define SETTINGS_DEF_USE_DHCP       NET_MODE_LINKLOCAL

#define SETTINGS_DEF_IP0            192u
#define SETTINGS_DEF_IP1            168u
#define SETTINGS_DEF_IP2            1u
#define SETTINGS_DEF_IP3            10u

#define SETTINGS_DEF_MASK0          255u
#define SETTINGS_DEF_MASK1          255u
#define SETTINGS_DEF_MASK2          255u
#define SETTINGS_DEF_MASK3          0u

#define SETTINGS_DEF_GW0            192u
#define SETTINGS_DEF_GW1            168u
#define SETTINGS_DEF_GW2            1u
#define SETTINGS_DEF_GW3            1u

/* Per-channel defaults: enabled, 4–20 mA scale, power-on setpoint 0 mA. */
#define SETTINGS_DEF_CH_ENABLED     1u
#define SETTINGS_DEF_SCALE_LO_UA    4000u
#define SETTINGS_DEF_SCALE_HI_UA    20000u
#define SETTINGS_SCALE_MAX_UA       22000u   /* upper bound for thresholds/setpoints */
#define SETTINGS_DEF_SETPOINT_UA    0u

/* Comms-loss behaviour: mode per channel, one shared timeout. */
#define SETTINGS_LOSS_HOLD          0u    /* keep the last setpoint            */
#define SETTINGS_LOSS_SAFE          1u    /* go to ch_safe_ua                  */
#define SETTINGS_LOSS_OFF           2u    /* disable the output (0 mA, ON off)  */
#define SETTINGS_DEF_CH_LOSS_MODE   SETTINGS_LOSS_HOLD
#define SETTINGS_DEF_CH_SAFE_UA     0u
#define SETTINGS_DEF_LOSS_TIMEOUT   50u   /* ×100 ms = 5 s; 0 = disabled       */

/* Nominal XTR111 SET resistor (Ω) and DAC reference (V):
 *   I = 10 · V_DAC / R_SET,  V_DAC = code / 65535 · V_REF.
 * Starting estimates only — the per-channel calibration removes R_SET
 * tolerance and DAC/reference error. */
#define SETTINGS_DEF_RSET           1100.0f
#define SETTINGS_DEF_VREF           2.5f

/* LED mode codes ------------------------------------------------------------ */
typedef enum {
    LED_MODE_ALW_OFF       = 0,
    LED_MODE_ALW_ON        = 1,
    LED_MODE_STATE_MACHINE = 2,
} led_mode_t;

/**
 * Persistent settings structure. Layout is fixed and naturally aligned. Do not
 * reorder without bumping SETTINGS_VERSION.
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;

    uint16_t poll_ms;               /* EF / liveness poll period, ms          */
    uint16_t led_mode;              /* led_mode_t                             */

    uint16_t modbus_tcp_port;       /* default 502                            */
    uint8_t  modbus_slave_id;       /* default 1                              */
    uint8_t  use_dhcp;              /* net mode: 0=static,1=DHCP,2=link-local */

    uint8_t  ip[4];
    uint8_t  netmask[4];
    uint8_t  gateway[4];

    /* Per-channel configuration. */
    uint8_t  ch_enabled[SETTINGS_AO_CHANNELS];
    uint8_t  ch_loss_mode[SETTINGS_AO_CHANNELS];   /* SETTINGS_LOSS_*          */
    uint16_t ch_scale_lo_ua[SETTINGS_AO_CHANNELS]; /* setpoint 0     <-> lo µA */
    uint16_t ch_scale_hi_ua[SETTINGS_AO_CHANNELS]; /* setpoint 32767 <-> hi µA */
    uint16_t ch_safe_ua[SETTINGS_AO_CHANNELS];     /* comms-loss SAFE value    */
    uint16_t ch_setpoint_ua[SETTINGS_AO_CHANNELS]; /* power-on setpoint (saved by SAVE) */

    uint16_t loss_timeout_x100ms;   /* comms-loss timeout, 0 = disabled       */
    uint16_t reserved1;

    /* Nominal R_SET (Ω) and V_REF (V). */
    float    rset_nominal;
    float    vref_nominal;

    char     name[16];              /* device name, NUL-padded (discovery)    */

    uint32_t crc32;                 /* CRC32 over all preceding bytes         */
} settings_t;

#define SETTINGS_NAME_LEN   16u

/* API ----------------------------------------------------------------------- */

/**
 * Initialise the settings subsystem. Loads settings from Flash; if the stored
 * image is invalid the structure is filled with defaults.
 *
 * @return true if the stored image was valid, false if defaults were applied.
 */
bool settings_init(void);

/** Reload defaults into the in-memory settings (does not write to flash). */
void settings_reset_to_defaults(void);

/** Persist the current in-memory settings to internal Flash. */
bool settings_save(void);

/** Get a pointer to the live in-memory settings. */
settings_t* settings_get(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_SETTINGS_H */
