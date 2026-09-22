/**
  ******************************************************************************
  * @file    calstore.h
  * @brief   Write-once calibration store for the 8AOC module.
  *
  * Per-channel 2-point linear calibration (I_cmd = gain·I_target + offset) is
  * kept in a dedicated internal-Flash sector (Sector 11) that the firmware
  * NEVER erases during normal operation — not on settings save, not on factory
  * reset. Each of the 8 channel slots can be committed
  * exactly once: once a slot is programmed it cannot be rewritten, because
  * internal Flash can only clear bits (1 → 0) without a full sector erase.
  *
  * The live (working) coefficients live in RAM here. Modbus writes update the
  * live values as a preview (rejected if the slot is already locked); a
  * separate commit persists the live values into the slot and locks it.
  *
  * The only way to un-lock is calstore_erase(), which erases the whole sector
  * and reverts every slot to the neutral defaults (gain = 1.0, offset = 0.0).
  * That path is intended for a protected, physically-confirmed service action.
  ******************************************************************************
  */
#ifndef APPLICATION_CALSTORE_H
#define APPLICATION_CALSTORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CALSTORE_CHANNELS   8u
#define CALSTORE_GCLASSES   1u    /* single range per channel; gclass is always 0 */
#define CALSTORE_SLOTS      (CALSTORE_CHANNELS * CALSTORE_GCLASSES)   /* 8 */

/** Slot index used by the Modbus commit trigger and the lock mask. */
#define CALSTORE_SLOT(ch, gclass)   ((uint8_t)((ch) * CALSTORE_GCLASSES + (gclass)))

/** Load committed slots from Flash into the live coefficients. Un-committed
 *  (channel, gain class) pairs are initialised to the neutral defaults. */
void calstore_init(void);

/** True if the (channel, gain class) slot has been committed (write-locked). */
bool calstore_is_locked(uint8_t ch, uint8_t gclass);

/** Bitmask of locked slots: bit CALSTORE_SLOT(ch, gclass) set = locked. */
uint32_t calstore_lock_mask(void);

/** Live gain / offset used by the acquisition path. NAN-safe defaults. */
float calstore_gain(uint8_t ch, uint8_t gclass);
float calstore_offset(uint8_t ch, uint8_t gclass);

/** Update a live coefficient (preview). Returns false if the slot is locked
 *  or the arguments are out of range. Does NOT touch Flash. */
bool calstore_set_gain(uint8_t ch, uint8_t gclass, float value);
bool calstore_set_offset(uint8_t ch, uint8_t gclass, float value);

/** Persist the live gain/offset of (channel, gain class) into its Flash slot
 *  and lock it. Returns false if already locked, out of range, or on Flash
 *  error. One-shot: a locked slot cannot be re-committed. */
bool calstore_commit(uint8_t ch, uint8_t gclass);

/** Emergency service action: erase the calibration sector, unlock every slot
 *  and revert the live coefficients to defaults. Returns false on Flash error. */
bool calstore_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CALSTORE_H */
