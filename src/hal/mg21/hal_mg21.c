/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Board glue for a ConBee III target: startup hand-off, HAL lifecycle (hal/hal.h), and the reset
 * that hands the board to the bootloader's window.

The host link is the board's (hal/com.h, named by targets/cb3/board.cmake), so this file names no
link. The one thing here that the SAMR21 glue does not do is the reset: on the ConBee II a software
reset lands in the bootloader window because the GCF bootloader owns that flag at 0x4FFF; on this
board the decision is a record in SRAM that the deCONZ bootloader reads after the jump, and an
application has to write it itself (docs/hardware_mg21.md §10.7). */
#include <stdint.h>

#include "board.h"
#include "com.h"
#include "dbg.h"
#include "hal.h"
#include "mg21_clock.h"
#include "mg21_panic.h"
#include "mg21_radio.h"
#include "mg21_regs.h"
#include "mg21_uart.h"
#include "mg21_reset_record.h"
#include "mg21_wdt.h"

/* Linker symbols (targets/cb3/efr32mg21.ld). */
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];
extern uint32_t _data_load;
extern uint32_t __isr_vector[];   /* the vector table's own address, where the linker put it */

extern int main(void);

/* Handler-mode depth: a vector can be preempted by another vector, so this counts rather than
 * flags (hal/hal.h). */
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

/* ARMv8-M leaves the memory, bus and usage faults undisabled-but-unrouted: their vectors exist and
 * are not enabled until a SHCSR bit says so, and an unenabled one escalates into a HardFault that
 * says less than the fault it swallowed. */
#define MG21_FAULT_ENABLE (MG21_SCB_SHCSR_MEMFAULTENA | MG21_SCB_SHCSR_BUSFAULTENA             \
                           | MG21_SCB_SHCSR_USGFAULTENA)

void HAL_PreMain(void)
{
    uint32_t *dst;
    const uint32_t *src;

    /* The bootloader enters this image from its SystemInit2, straight after a reset and with
     * PRIMASK clear, so nothing is masked until this says so. Every vector this image enables during
     * bring-up stays masked until HAL_Init unlocks last. */
    HAL_IrqLock();

    /* VTOR points at the vector table where the linker actually placed it, not at a descriptor
     * constant: if the two ever disagree, the core would fetch vectors from the wrong address and
     * every exception would misroute. The table is padded to its own size in startup_mg21.S so this
     * address is aligned to it (see the VECT asserts in efr32mg21.ld). */
    *(volatile uint32_t *)MG21_SCB_VTOR = (uint32_t)&__isr_vector;
    *(volatile uint32_t *)MG21_SCB_SHCSR = MG21_FAULT_ENABLE;
    HAL_Dsb();

    /* The bootloader's fast path runs before its own peripherals are brought up, so it leaves no
     * link live; enable and pending state are cleared for every peripheral IRQ anyway, so that this
     * board's bring-up enables exactly the ones it drives and owes nothing to the jump. */
    *(volatile uint32_t *)MG21_NVIC_ICER0 = 0xFFFFFFFFu;
    *(volatile uint32_t *)MG21_NVIC_ICER1 = 0xFFFFFFFFu;
    *(volatile uint32_t *)MG21_NVIC_ICPR0 = 0xFFFFFFFFu;
    *(volatile uint32_t *)MG21_NVIC_ICPR1 = 0xFFFFFFFFu;
    HAL_Dsb();

    dst = _sbss;

    while (dst < _ebss)
        *dst++ = 0u;

    dst = _sdata;
    src = (const uint32_t *)&_data_load;

    while (dst < _edata)
        *dst++ = *src++;

    DBG(DBG_COMP_STARTUP, DBG_STARTUP_RSN_BOOT);
    Panic_ReportPrevious();
    main();
}

int HAL_Init(void)
{
    int regime;

    regime = Clock_Init();

    if (regime == MG21_CLOCK_DOWN)
    {
        /* No board clock: nothing here can keep a deadline, and a link opened without one cannot be
         * polled. The image idles in EM1 - wfi, and SLEEPDEEP is never set, so not EM2 - where the
         * inherited watchdog still runs (its EM2RUN bit only stops it in EM2 and below). It is not fed
         * on this path, so it resets the board within its period, and that reset is the report. There
         * is no deep-sleep black hole: the board never enters EM2. */
        DBG(DBG_COMP_APP, DBG_APP_RSN_HELLO);
        return 0;
    }

    (void)Wdt_Init();
    if (!COM_Init())
        DBG(DBG_COMP_APP, DBG_APP_RSN_LINK_INIT_FAIL);

    Clock_WitnessReport();

    Clock_TickCountedByIrq(1);
    HAL_IrqUnlock();

    return regime;
}

/* HAL_SystemReset on this board is a reset into the bootloader's normal path: the record is cleared,
 * so the next boot verifies the application, sends its ID packet unsolicited and opens its 1 s CLI
 * window before it starts the application again. That is the path the vendor firmware's own reset
 * takes, and GCFFlasher accepts the unsolicited ID while it waits on its reset command.
 *
 * The reset itself is the watchdog's, as the vendor firmware's is: interrupts masked, the watchdog
 * no longer fed, and its period (2 s, mg21_wdt.c) ends the image. On the bench every SYSRESETREQ
 * this image issued - with the stay request 0x205 and with the record cleared alike - left the stick
 * silent, answering no ID request, until a power cycle, while the vendor firmware's watchdog reset
 * reaches a bootloader that answers (docs/hardware_mg21.md §10.7). SYSRESETREQ remains only for a watchdog
 * that is not counting. This is what the flasher trap in app/main.c and the panic path reach for. */
void HAL_SystemReset(void)
{
    uint16_t *record = (uint16_t *)MG21_RESET_RECORD_ADDR;

    HAL_IrqLock();
    ResetRecord_Clear(record);

    /* The bootloader assumes the state a reset leaves; hand back what this image changed rather than
     * trusting the soft reset to restore it: the radio first (Mg21Radio_Release, mg21_radio.h), then
     * the link and the clocks (mg21_clock.c Clock_Release). */
    Mg21Radio_Release();
    Uart_Release();
    Clock_Release();
    HAL_Dsb();

    if (MG21_WDOG0->EN != 0u)
    {
        for (;;)
        {
        }
    }

    /* DSB: the AIRCR store has to commit before the reset event. */
    *(volatile uint32_t *)MG21_SCB_AIRCR = MG21_AIRCR_VECTKEY | MG21_AIRCR_SYSRESETREQ;
    HAL_Dsb();

    for (;;)
    {
    }
}
