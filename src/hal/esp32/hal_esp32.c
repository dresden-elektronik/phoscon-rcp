/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Board glue for an ESP32 target: the startup's hand-off, the HAL lifecycle (hal/hal.h) and the reset.

The board's ESP-IDF bootloader stays on the flash and starts this image as it would an ESP-IDF app
(docs/hardware_esp32.md §12.3): it has loaded the RAM segments and mapped the flash ones, so HAL_PreMain
only clears .bss. A reset is the HP system's software reset; the ROM and the bootloader then run again
and start this image, unless the CDC line state asked the ROM for download mode, which is how esptool
re-flashes a running board. There is no bootloader window to reset into and no GCF flasher, so the
flasher trap's request lands here as an ordinary reset (docs/build-and-flash.md §13). */
#include <stdint.h>

#include "com.h"
#include "dbg.h"
#include "esp32_clock.h"
#include "esp32_gpio.h"
#ifdef DS_ESP32_PHY_PROBE
#include "esp32_phy.h"
#endif
#include "esp32_panic.h"
#include "esp32_regs.h"
#include "esp32_wdt.h"
#include "hal.h"

extern uint32_t _sbss[];
extern uint32_t _ebss[];

extern int main(void);

/* mstatus as the bootloader handed over, stored by the startup before .bss is cleared. */
__attribute__((section(".data"))) uint32_t esp32BootMstatus = 0xFFFFFFFFu;

/* Handler-mode depth (hal/hal.h). Interrupts do not nest on this layer, so it is 0 or 1, but it is kept
 * a depth so the contract reads the same on every board. */
static uint8_t halIrqDepth;

void HAL_IrqEnter(void)
{
    halIrqDepth++;
}

void HAL_IrqExit(void)
{
    if (halIrqDepth != 0u)
        halIrqDepth--;
}

int HAL_InIrqContext(void)
{
    return halIrqDepth != 0u;
}

void HAL_PreMain(void)
{
    uint32_t *dst;

    HAL_IrqLock();

    dst = _sbss;

    while (dst < _ebss)
        *dst++ = 0u;

    DBG(DBG_COMP_STARTUP, DBG_STARTUP_RSN_BOOT);
    main();
}

/* The reports of what came before this boot - a panic's mark, the reset cause, the state the bootloader
 * handed over - are emitted here, once the loop holds the boot set for replay: a host on this USB link is
 * rarely attached at the moment of a reset, and a record emitted before the hold is gone by the time it
 * opens the port. */
int HAL_Init(void)
{
    Panic_ReportPrevious();
    Debug_Emit(DBG_COMP_WITNESS,
               (uint16_t)(ESP32_REG(ESP32_LP_CLKRST_BASE, ESP32_LP_CLKRST_RESET_CAUSE) & 0x1Fu),
               DBG_WITNESS_RSN_ESP32_RESET_CAUSE);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)esp32BootMstatus, DBG_WITNESS_RSN_ESP32_MSTATUS);

    (void)Wdt_Init();
    Esp32Board_PinsInit();
    (void)Esp32Clock_CpuToMax();

    if (!Clock_Init())
    {
        /* No millisecond: nothing here can keep a deadline, so the watchdog, unfed, is the report. */
        DBG(DBG_COMP_APP, DBG_APP_RSN_HELLO);
        return 0;
    }

    if (!COM_Init())
        DBG(DBG_COMP_APP, DBG_APP_RSN_LINK_INIT_FAIL);

    Clock_WitnessReport();
    Clock_TickCountedByIrq(1);
    HAL_IrqUnlock();

#ifdef DS_ESP32_PHY_PROBE
    /* With the millisecond running: the calibration takes tens of milliseconds, and its duration is one
     * of the records. */
    (void)Esp32Phy_Init();
#endif

    return 1;
}

void HAL_SystemReset(void)
{
    HAL_IrqLock();
    ESP32_REG(ESP32_LP_AON_BASE, ESP32_LP_AON_SYS_CFG) |= ESP32_LP_AON_HPSYS_SW_RESET;
    HAL_Dsb();

    for (;;)
    {
    }
}
