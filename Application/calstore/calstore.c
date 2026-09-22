/**
  ******************************************************************************
  * @file    calstore.c
  * @brief   Write-once calibration store (see calstore.h).
  *
  * Flash layout — Sector 11 of the STM32F407VG, 128 KiB starting at
  * 0x080E0000. Only the first 8 * sizeof(cal_slot_t) bytes are used; the slot
  * for (channel, gain class) is at fixed index CALSTORE_SLOT(ch, gclass). A
  * slot is "empty" while its magic reads 0xFFFFFFFF (erased Flash) and
  * "locked" once a valid record with the correct magic and CRC is programmed.
  ******************************************************************************
  */

#include "calstore.h"

#include <math.h>
#include <string.h>

#include "stm32f4xx_hal.h"

/* ---------------------------------------------------------------------------
 * Storage location
 * ------------------------------------------------------------------------- */
#define CALSTORE_FLASH_SECTOR   FLASH_SECTOR_11
#define CALSTORE_FLASH_ADDR     0x080E0000u
/* 8AOC (8 channels, one slot each). Distinct from the 4RTD/8AIC magics. */
#define CALSTORE_SLOT_MAGIC     0xCA11B0A0u

/* Persistent per-slot record. Fixed 20-byte layout, word-aligned so it can be
 * programmed with 32-bit Flash writes. Do not reorder. */
typedef struct {
    uint32_t magic;     /* 0xFFFFFFFF = empty; CALSTORE_SLOT_MAGIC = written */
    uint8_t  channel;
    uint8_t  gclass;
    uint16_t reserved;
    float    gain;
    float    offset;
    uint32_t crc32;     /* CRC32 over the preceding 16 bytes */
} cal_slot_t;

/* ---------------------------------------------------------------------------
 * Live (RAM) state
 * ------------------------------------------------------------------------- */
static float s_gain[CALSTORE_CHANNELS][CALSTORE_GCLASSES];
static float s_offset[CALSTORE_CHANNELS][CALSTORE_GCLASSES];
static bool  s_locked[CALSTORE_CHANNELS][CALSTORE_GCLASSES];

/* ---------------------------------------------------------------------------
 * CRC32 (IEEE 802.3, software) — same polynomial as settings.c.
 * ------------------------------------------------------------------------- */
static uint32_t calstore_crc32(const void* data, uint32_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint32_t k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t slot_crc(const cal_slot_t* s)
{
    return calstore_crc32(s, (uint32_t)((const uint8_t*)&s->crc32 - (const uint8_t*)s));
}

static const cal_slot_t* slot_at(uint8_t index)
{
    return &((const cal_slot_t*)CALSTORE_FLASH_ADDR)[index];
}

static bool slot_valid(const cal_slot_t* nv, uint8_t ch, uint8_t gclass)
{
    return nv->magic == CALSTORE_SLOT_MAGIC &&
           nv->channel == ch && nv->gclass == gclass &&
           nv->crc32 == slot_crc(nv);
}

static void set_defaults(uint8_t ch, uint8_t gclass)
{
    s_gain[ch][gclass]   = 1.0f;
    s_offset[ch][gclass] = 0.0f;
    s_locked[ch][gclass] = false;
}

/* ---------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */
void calstore_init(void)
{
    for (uint8_t ch = 0; ch < CALSTORE_CHANNELS; ch++) {
        for (uint8_t r = 0; r < CALSTORE_GCLASSES; r++) {
            const cal_slot_t* nv = slot_at((uint8_t)(ch * CALSTORE_GCLASSES + r));
            if (slot_valid(nv, ch, r)) {
                s_gain[ch][r]   = nv->gain;
                s_offset[ch][r] = nv->offset;
                s_locked[ch][r] = true;
            } else {
                set_defaults(ch, r);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Accessors
 * ------------------------------------------------------------------------- */
bool calstore_is_locked(uint8_t ch, uint8_t gclass)
{
    if (ch >= CALSTORE_CHANNELS || gclass >= CALSTORE_GCLASSES) { return false; }
    return s_locked[ch][gclass];
}

uint32_t calstore_lock_mask(void)
{
    uint32_t m = 0u;
    for (uint8_t ch = 0; ch < CALSTORE_CHANNELS; ch++) {
        for (uint8_t r = 0; r < CALSTORE_GCLASSES; r++) {
            if (s_locked[ch][r]) { m |= (1u << CALSTORE_SLOT(ch, r)); }
        }
    }
    return m;
}

float calstore_gain(uint8_t ch, uint8_t gclass)
{
    if (ch >= CALSTORE_CHANNELS || gclass >= CALSTORE_GCLASSES) { return 1.0f; }
    return s_gain[ch][gclass];
}

float calstore_offset(uint8_t ch, uint8_t gclass)
{
    if (ch >= CALSTORE_CHANNELS || gclass >= CALSTORE_GCLASSES) { return 0.0f; }
    return s_offset[ch][gclass];
}

bool calstore_set_gain(uint8_t ch, uint8_t gclass, float value)
{
    if (ch >= CALSTORE_CHANNELS || gclass >= CALSTORE_GCLASSES) { return false; }
    if (s_locked[ch][gclass]) { return false; }
    s_gain[ch][gclass] = value;
    return true;
}

bool calstore_set_offset(uint8_t ch, uint8_t gclass, float value)
{
    if (ch >= CALSTORE_CHANNELS || gclass >= CALSTORE_GCLASSES) { return false; }
    if (s_locked[ch][gclass]) { return false; }
    s_offset[ch][gclass] = value;
    return true;
}

/* ---------------------------------------------------------------------------
 * Commit — program a single slot, then lock it.
 * ------------------------------------------------------------------------- */
bool calstore_commit(uint8_t ch, uint8_t gclass)
{
    if (ch >= CALSTORE_CHANNELS || gclass >= CALSTORE_GCLASSES) { return false; }
    if (s_locked[ch][gclass]) { return false; }

    const uint8_t index = CALSTORE_SLOT(ch, gclass);
    const cal_slot_t* nv = slot_at(index);

    /* The slot must be fully erased before programming (write-once guard). */
    const uint32_t* w = (const uint32_t*)nv;
    for (uint32_t i = 0; i < sizeof(cal_slot_t) / 4u; i++) {
        if (w[i] != 0xFFFFFFFFu) { return false; }
    }

    cal_slot_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic   = CALSTORE_SLOT_MAGIC;
    rec.channel = ch;
    rec.gclass  = gclass;
    rec.gain    = s_gain[ch][gclass];
    rec.offset  = s_offset[ch][gclass];
    rec.crc32   = slot_crc(&rec);

    if (HAL_FLASH_Unlock() != HAL_OK) { return false; }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    const uint32_t* src = (const uint32_t*)&rec;
    uint32_t addr = CALSTORE_FLASH_ADDR + (uint32_t)index * sizeof(cal_slot_t);
    bool ok = true;
    for (uint32_t i = 0; i < sizeof(cal_slot_t) / 4u; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, src[i]) != HAL_OK) {
            ok = false;
            break;
        }
        addr += 4u;
    }
    HAL_FLASH_Lock();

    if (!ok || !slot_valid(slot_at(index), ch, gclass)) { return false; }

    s_locked[ch][gclass] = true;
    return true;
}

/* ---------------------------------------------------------------------------
 * Erase — protected service action. Erases the whole sector; blocks the CPU
 * for ~1-2 s. Caller is responsible for the watchdog and any indication.
 * ------------------------------------------------------------------------- */
bool calstore_erase(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK) { return false; }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    FLASH_EraseInitTypeDef erase = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .Banks        = FLASH_BANK_1,
        .Sector       = CALSTORE_FLASH_SECTOR,
        .NbSectors    = 1u,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
    };
    uint32_t sector_error = 0u;
    const HAL_StatusTypeDef hs = HAL_FLASHEx_Erase(&erase, &sector_error);
    HAL_FLASH_Lock();

    for (uint8_t ch = 0; ch < CALSTORE_CHANNELS; ch++) {
        for (uint8_t r = 0; r < CALSTORE_GCLASSES; r++) {
            set_defaults(ch, r);
        }
    }
    return hs == HAL_OK;
}
