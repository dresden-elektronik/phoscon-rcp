/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Device identity: MAC, serial, chip UID, read from the NVM user row at 0x804000 and the
 * deCONZ NVRAM basic set at 0x3FFE0.
 *  - Chip UID: 12 bytes at 0x80400A; deCONZ uses the first 8 as the MAC when NVRAM holds none.
 *  - deCONZ NVRAM basic set: 32 bytes at 0x3FFE0, the last flash page's last 32 bytes (magic 0xDE90 |
 *    id 1|2 | len 8 | mac[8] LE | reserved[18] | CRC), parsed by samr21_basic.c.
 *  - USB serial: 9 ASCII bytes at 0x804012.
 *  - Update flag: byte 0x4FFF (last byte of the row at 0x4F00); the vendor bootloader erases
 *    the first app page at 0x5000 while it is set, so the app clears it at every boot.
 * MAC is returned in deCONZ byte order (little-endian, byte 0 = LSB). */
#include <stdint.h>

#include "hal.h"
#include "hal_diag.h"
#include "dbg.h"
#include "samr21_basic.h"
#include "samr21_regs.h"

#define NVM_USER_ROW_ADDR 0x00804000u
/* The two EUI-64 layouts the search below chooses between (see Nvram_FactoryEui64). */
#define NVM_EUI_ALIGNED_OFFSET 0x08u
#define NVM_UID_OFFSET 0x0Au
#define NVM_UID_ADDR 0x0080400Au
#define NVM_SERIAL_ADDR 0x00804012u
#define NVM_SERIAL_LEN 9u

static int nvramAllEqual(const uint8_t *buf, uint16_t len, uint8_t v)
{
    uint16_t i;

    for (i = 0; i < len; ++i)
    {
        if (buf[i] != v)
            return 0;
    }

    return 1;
}

void Nvram_Uid(uint8_t *uid12)
{
    const uint8_t *src = (const uint8_t *)NVM_UID_ADDR;
    uint8_t i;

    for (i = 0; i < 12; ++i)
        uid12[i] = src[i];
}

/* 1 = MAC written to mac8 (deCONZ byte order). The basic set when one is there; the chip UID when the
 * page holds none (its magic erased), as the vendor firmware's fallback does; 0 for a set that is
 * there and fails its checks, which is a damaged address, not an absent one. The 32 bytes are copied
 * through a volatile pointer: a plain copy loop is a libc memcpy to GCC, and this image has none. */
int Nvram_MacValid(uint8_t *mac8)
{
    const volatile uint8_t *page = (const volatile uint8_t *)SAMR21_BASIC_ADDR;
    uint8_t set[SAMR21_BASIC_LEN];
    uint8_t uid[12];
    uint8_t i;

    if (mac8 == 0)
        return 0;

    for (i = 0u; i < SAMR21_BASIC_LEN; ++i)
        set[i] = page[i];

    if (set[0] == 0x90u && set[1] == 0xDEu)
        return Samr21Basic_Mac(set, mac8);

    Nvram_Uid(uid);

    if (nvramAllEqual(uid, 8, 0x00) || nvramAllEqual(uid, 8, 0xFF))
        return 0;

    for (i = 0u; i < 8u; ++i)
        mac8[i] = uid[i];

    return 1;
}

/* Factory-assigned IEEE EUI-64 for SPINEL_PROP_HWADDR: the NVM user row's IEEE-address
 * field at 0x80400A (samr21Nvm.h:23-24; nv_basic.c:71 `UID_NV_ADDR`), which survives a
 * chip erase because the auxiliary rows are not erased by one (samr21.txt:2809 §8.3.1).
 * The row holds it least significant byte first; Spinel HWADDR is MSB-first, so reverse it
 * (otPlatRadio.c:43-54). deCONZ's own stack wants the LE form and does not reverse.
 * Never fall back to the deCONZ NVRAM basic set the way Nvram_MacValid does: that is the
 * *host-set* address, HWADDR is by definition the factory one, and a host may program what
 * it reads here into its own stack. */
/* A factory EUI-64: I/G bit clear, not erased or zeroed, and not an all-ones or all-zero
 * OUI. The search below relies on an overlapping field failing one of these. */
static int nvramEuiPlausible(const uint8_t *eui8)
{
    uint8_t i;
    int anyFf = 0;
    int any = 0;

    if ((eui8[0] & 0x01u) != 0u)
        return 0;

    for (i = 0; i < 8u; ++i)
    {
        anyFf = anyFf || (eui8[i] == 0xFFu);
        any = any || (eui8[i] != 0x00u);
    }

    if (!any || (anyFf && eui8[0] == 0xFFu && eui8[1] == 0xFFu && eui8[2] == 0xFFu))
        return 0;

    return 1;
}

int Nvram_FactoryEui64(uint8_t *eui8)
{
    const uint8_t *row = (const uint8_t *)NVM_USER_ROW_ADDR;
    uint8_t cand[8];
    uint8_t i;
    uint8_t source;

    if (eui8 == 0)
        return 0;

    /* Two production layouts: (1) eight bytes at +0x0A least significant byte first,
   * reversed into display order (nv_basic.c:71; samr21Nvm.h:24), tried first; (2) eight
   * bytes at +0x08 already in display order, 8-byte aligned, 0xFF at +0x10..+0x11. Which
   * one won is published (DBG_FLASH_RSN_EUI_SOURCE). */
    for (i = 0; i < 8u; ++i)
        cand[i] = row[NVM_UID_OFFSET + 7u - i];

    source = (uint8_t)NVM_UID_OFFSET;

    if (!nvramEuiPlausible(cand))
    {
        for (i = 0; i < 8u; ++i)
            cand[i] = row[NVM_EUI_ALIGNED_OFFSET + i];

        source = (uint8_t)NVM_EUI_ALIGNED_OFFSET;

        if (!nvramEuiPlausible(cand))
        {
            Debug_Emit(DBG_COMP_FLASH, 0u, DBG_FLASH_RSN_EUI_SOURCE);
            return 0;
        }
    }
    /* Through a volatile pointer: an eight-byte copy loop is a libc memcpy to GCC, and this
   * image has no libc (docs/coding-rules.md §7). */
    {
        volatile uint8_t *dst = eui8;

        for (i = 0; i < 8u; ++i)
            dst[i] = cand[i];
    }
    Debug_Emit(DBG_COMP_FLASH, source, DBG_FLASH_RSN_EUI_SOURCE);
    return 1;
}

/* Publish the NVM user row verbatim: 32 records, the row's 16-bit words in order. The row
 * is 64 bytes at 0x804000: fuses at 0x00..0x03 (Table 8-3), then Dresden's fields. */
void Nvram_WitnessRow(void)
{
    const uint8_t *row = (const uint8_t *)NVM_USER_ROW_ADDR;
    uint8_t i;

    for (i = 0; i < 32u; ++i)
    {
        uint16_t word = (uint16_t)(row[2u * i] | ((uint16_t)row[2u * i + 1u] << 8));

        Debug_Emit(DBG_COMP_FLASH, word, DBG_FLASH_RSN_USERROW);
    }
}

/* ASCII serial from the NVM user row. Returns the byte count (0 if the row was never
 * programmed). */
uint8_t Nvram_SerialAscii(uint8_t *buf, uint8_t maxLen)
{
    const uint8_t *src = (const uint8_t *)NVM_SERIAL_ADDR;
    uint8_t i;
    uint8_t len = 0;
    int blank;

    for (i = 0; i < NVM_SERIAL_LEN; ++i)
    {
        if (i < maxLen)
            buf[i] = src[i];
    }

    blank = nvramAllEqual(src, NVM_SERIAL_LEN, 0xFF) || nvramAllEqual(src, NVM_SERIAL_LEN, 0x00);

    if (blank)
        return 0;

    for (i = 0; i < NVM_SERIAL_LEN; ++i)
    {
        if (i < maxLen && src[i] >= 0x20 && src[i] < 0x7F)
            ++len;
    }

    return len;
}

/* ---- Bootloader update flag ----
 * NVMCTRL sequence as the vendor bootloader's setUpdateFlag does it
 * (R21_GCF_BTL_USB_v2_07/src/de_btl/src/flashLoader.c): CTRLB stays at its reset value
 * (MANW), no page-buffer clear, 16-bit stores to the flash address load the page buffer
 * and auto-advance ADDR, one WRITE_PAGE per 64-byte page. Register facts in samr21_regs.h. */
#define NVRAM_FLAG_ROW_ADDR 0x00004F00u
#define NVRAM_ROW_SIZE 256u
#define NVRAM_PAGE_SIZE 64u

/* Bounded and WDT-fed: a lost READY must not hang the chip until the WDT reset, because a
 * reset mid-sequence would leave the flag set and erase the app page on the next boot. At
 * 1 MHz after POR tFRE 6 ms is ~6 000 polls, so 0x1000 cannot time out spuriously. */
#define NVM_READY_WAIT_LIMIT 0x1000u

static int nvmctrlWaitReadyBounded(void)
{
    uint32_t i;

    for (i = 0; i < NVM_READY_WAIT_LIMIT; ++i)
    {
        if ((SAMR21_NVMCTRL->INTFLAG & NVMCTRL_INTFLAG_READY) != 0u)
            return 1;

        Clock_BringupPump();
        Wdt_FeedPoll(i);
    }

    return 0;
}

/* Issue one NVMCTRL command and wait for it to FINISH. No read-while-write: a command
 * clears INTFLAG.READY for the whole window (tFRE 6 ms erase, tFPP 2.5 ms page program,
 * Table 42-33, samr21.txt:54661,:54663) and the AHB stalls (samr21.txt:17858,:17865); a
 * command written while READY is low is IGNORED (samr21.txt:17792-17793). 1 = completed,
 * 0 = a READY wait timed out (the caller must not keep programming). */
static int nvmctrlExec(uint16_t cmd)
{
    if (!nvmctrlWaitReadyBounded())
    {
        DBG(DBG_COMP_FLASH, DBG_FLASH_RSN_READY_BEFORE);
        return 0;
    }

    SAMR21_NVMCTRL->CTRLA = (uint16_t)(cmd | NVMCTRL_CTRLA_KEY);

    if (!nvmctrlWaitReadyBounded())
    {
        DBG(DBG_COMP_FLASH, DBG_FLASH_RSN_READY_AFTER);
        return 0;
    }

    return 1;
}

void Nvram_ClearUpdateFlag(void)
{
    uint16_t row[NVRAM_ROW_SIZE / 2];
    const uint16_t *src = (const uint16_t *)NVRAM_FLAG_ROW_ADDR;
    volatile uint16_t *dst = (volatile uint16_t *)NVRAM_FLAG_ROW_ADDR;
    uint32_t i;
    uint8_t page;

    for (i = 0; i < NVRAM_ROW_SIZE / 2; ++i)
        row[i] = src[i];

    /* The flag is byte 0x4FFF: the high byte of the last half-word. */
    if ((row[NVRAM_ROW_SIZE / 2 - 1] & 0xFF00u) == 0u)
        return;

    row[NVRAM_ROW_SIZE / 2 - 1] &= 0x00FFu;

    /* Ungate the NVMCTRL clocks (read-modify-write, other bits untouched); CTRLB stays at
   * its reset value, as the vendor bootloader does. */
    SAMR21_PM->AHBMASK |= PM_AHBMASK_NVMCTRL;
    SAMR21_PM->APBBMASK |= PM_APBBMASK_NVMCTRL;

    /* Erase the row, then commit the 256 bytes back one 64-byte page at a time, like the
   * bootloader. */
    SAMR21_NVMCTRL->ADDR = (uint32_t)(NVRAM_FLAG_ROW_ADDR >> 1);

    if (!nvmctrlExec(NVMCTRL_CTRLA_CMD_ER))
        return;

    for (page = 0; page < NVRAM_ROW_SIZE / NVRAM_PAGE_SIZE; ++page)
    {
        uint32_t base = page * (NVRAM_PAGE_SIZE / 2);

        for (i = 0; i < NVRAM_PAGE_SIZE / 2; ++i)
            dst[base + i] = row[base + i];

        if (!nvmctrlExec(NVMCTRL_CTRLA_CMD_WP))
            return;
    }

    /* Read the flag byte back; if it is still set, leave, never hang. */
    if (((const volatile uint8_t *)0x00004FFFu)[0] != 0x00u)
        return;

}

#ifdef DS_SCRATCH_ROW

/* ---- Flash scratchpad (post-mortem progress marker) ----
 * Row 0x3E000, first 64-byte page used; the erase granularity is the ROW, so every mark
 * saves and rewrites all 256 bytes. It must stay clear of the bootloader (0x3E00 itself is
 * live bootloader data) and of the deCONZ NVRAM at 0x3EF00; the vendor image ends at 0x405B
 * (vendor firmware 0x26780700), ours near 0xA400.
 * Layout (LE, one u16 per field): word0 = (STATUS_MAGIC << 8) | stage = 0xA000 | stage,
 * bytes [stage, 0xA0]; stages 1..0x40 are success milestones only (it must not double as
 * the debug mark, or the first benign record overwrites it). word1 = liveness in seconds
 * since boot. word2 = (STATUS_DBG_MAGIC << 8) | reason, word3 = component, word4 = line:
 * the last Debug_Emit record, so the row and the CDC stream agree. Best effort: a READY
 * timeout skips the write; never hangs.
 * Endurance: one row-erase per mark and the row is rated for 25k cycles (Table 42-31), so the
 * main loop marks once per 60 s, which reaches that within weeks: this is a bench image's
 * row, never an rcp image's (hal/hal_diag.h). */
#define STATUS_ROW_ADDR 0x0003E000u
#define STATUS_MAGIC 0x5AA0u
#define STATUS_DBG_MAGIC 0xB0u

/* Flash areas no write of ours may touch: the vendor GCF bootloader (spans 0x0..0x405B;
 * 0x5000 is the app origin) and the deCONZ NVRAM (last 4 KB). */
#define FLASH_BTL_END 0x00005000u
#define FLASH_NVRAM_BASE 0x0003EF00u

/* A negative array size is a C99 constraint error, so the build fails here if the scratch
 * row moves into the bootloader or the NVRAM. The linker scripts guard the update-flag row. */
typedef char statusRowClearOfBootloader[(STATUS_ROW_ADDR >= FLASH_BTL_END) ? 1 : -1];
typedef char statusRowClearOfNvram[(STATUS_ROW_ADDR + NVRAM_ROW_SIZE <= FLASH_NVRAM_BASE) ? 1 : -1];

/* ---- When a scratch write may run (defect H-K) ----
 * One row patch is ~16 ms of AHB stall (samr21.txt:17858,:17865), so never inside an IRQ:
 * handler mode (HAL enter/exit depth; IPSR would need asm) queues into a latest-wins pending
 * slot drained by Nvram_StatusFlush(), thread mode writes at once, and a request while a
 * patch is in flight is queued, never nested. One slot per status word, so a burst between
 * two drains costs one row cycle. */
/* Field index == row word index: the pending slots read back as the layout above. */
#define STATUS_FIELD_STAGE 0u
#define STATUS_FIELD_TICK 1u
#define STATUS_FIELD_DBG_REASON 2u
#define STATUS_FIELD_DBG_COMPONENT 3u
#define STATUS_FIELD_DBG_LINE 4u
#define STATUS_FIELD_COUNT 5u

static uint16_t statusPending[STATUS_FIELD_COUNT];
static uint8_t statusPendingMask; /* bit n: field n holds a value */
static uint8_t statusWriting;     /* a row patch is in flight */

#ifdef DS_LOOP_DIAG
/* Completed row patches, for loop_diag.c: a flash-stall theory is confirmed or killed by
 * the per-second delta, not by counting the Flush calls (which are mostly no-ops). */
static uint32_t statusPatchCount;

uint32_t Nvram_StatusPatches(void)
{
    return statusPatchCount;
}
#endif

/* Rewrite the scratch row for every field selected in `fields`: save the whole 256-byte row,
 * erase it once, write it back page by page (the vendor pattern above: one ERASE at the row
 * address plus four WP commits). Saving only the patched page would leave the rest erased.
 * 1 = every command completed. */
static int nvramStatusPatchRow(uint8_t fields)
{
    uint16_t row[NVRAM_ROW_SIZE / 2];
    const uint16_t *src = (const uint16_t *)STATUS_ROW_ADDR;
    volatile uint16_t *dst = (volatile uint16_t *)STATUS_ROW_ADDR;
    uint32_t i;
    uint8_t page;

    for (i = 0; i < NVRAM_ROW_SIZE / 2; ++i)
        row[i] = src[i];

    for (i = 0; i < STATUS_FIELD_COUNT; ++i)
    {
        if (fields & (uint8_t)(1u << i))
            row[i] = statusPending[i];
    }

    /* Ungate the NVMCTRL clocks (read-modify-write, like
   * Nvram_ClearUpdateFlag: if either gate were ever cleared, no command
   * would ever complete). */
    SAMR21_PM->AHBMASK |= PM_AHBMASK_NVMCTRL;
    SAMR21_PM->APBBMASK |= PM_APBBMASK_NVMCTRL;

    SAMR21_NVMCTRL->ADDR = (uint32_t)(STATUS_ROW_ADDR >> 1);

    if (!nvmctrlExec(NVMCTRL_CTRLA_CMD_ER))
        return 0;

    for (page = 0; page < NVRAM_ROW_SIZE / NVRAM_PAGE_SIZE; ++page)
    {
        uint32_t base = page * (NVRAM_PAGE_SIZE / 2);

        for (i = 0; i < NVRAM_PAGE_SIZE / 2; ++i)
            dst[base + i] = row[base + i];

        if (!nvmctrlExec(NVMCTRL_CTRLA_CMD_WP))
            return 0;
    }

#ifdef DS_LOOP_DIAG
    statusPatchCount++;
#endif
    return 1;
}

/* Drain the pending fields as ONE row cycle. The mask is taken before programming, so a
 * record arriving during the ~16 ms window survives to the next drain. */
void Nvram_StatusFlush(void)
{
    uint8_t fields;

    if (statusWriting != 0u)
        return;

    fields = statusPendingMask;

    if (fields == 0u)
        return;

    statusPendingMask = 0u;
    statusWriting = 1;
    (void)nvramStatusPatchRow(fields);
    statusWriting = 0;
}

/* Queue one field latest-wins; the write decision is separate so a multi-word event can
 * queue all of its words first and then spend ONE row cycle. */
static void statusQueue(uint8_t field, uint16_t val)
{
    statusPending[field] = val;
    statusPendingMask |= (uint8_t)(1u << field);
}

/* Vendor parity (MMIO trace): the released image issues 8 NVM commands before its USB
 * attach; ours issued 95, one scratch-row commit per debug record. While quiet, records and
 * stages queue in RAM only; Nvram_StatusQuiet(0) ends that and the caller flushes once. */
static int statusQuiet;

void Nvram_StatusQuiet(int on)
{
    statusQuiet = on;
}

static void statusCommit(void)
{
    if (statusQuiet != 0)
        return;

    if (statusWriting != 0u || HAL_InIrqContext() != 0)
    {
        return; /* handler mode: the loop drains it */
    }

    Nvram_StatusFlush();
}

static void statusRequest(uint8_t field, uint16_t val)
{
    statusQueue(field, val);
    statusCommit();
}

void Nvram_StatusStage(uint16_t stage)
{
    statusRequest(STATUS_FIELD_STAGE, (uint16_t)((STATUS_MAGIC << 8) | (stage & 0xFFu)));
}

/* The last Debug_Emit record, as three words (see the layout note). Every field is queued
 * before the flush decision, so one record is at most one row cycle. */
void Nvram_StatusDebug(uint16_t component, uint16_t line, uint16_t reason)
{
    statusQueue(STATUS_FIELD_DBG_COMPONENT, component);
    statusQueue(STATUS_FIELD_DBG_LINE, line);
    statusQueue(STATUS_FIELD_DBG_REASON, (uint16_t)((STATUS_DBG_MAGIC << 8) | (reason & 0xFFu)));
    statusCommit();
}

/* ms (monotonic Tick_GetMs) is stored as seconds: the liveness field is one 16-bit word. */
void Nvram_StatusTick(uint32_t ms)
{
    statusRequest(STATUS_FIELD_TICK, (uint16_t)(ms / 1000u));
}

#endif /* DS_SCRATCH_ROW */
