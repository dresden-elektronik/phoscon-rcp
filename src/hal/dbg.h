/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Binary debug interface (docs/coding-rules.md §5). No debug strings: every
 * diagnostic is one 8-byte little-endian record -
 *   u16 magic 0x0000, u16 component DBG_COMP_*, u16 line, u16 reason.
 * On CDC bulk IN, whole records back to back, no header/length/checksum; the
 * stream is self-synchronising only by its fixed size. Host string table:
 * tools/dbg.py. */
#ifndef DBG_H
#define DBG_H

#include <stdint.h>

typedef enum
{
    DBG_COMP_CORE = 1,
    DBG_COMP_STARTUP,
    DBG_COMP_CLOCK,
    DBG_COMP_USB,
    DBG_COMP_CDC,
    DBG_COMP_FLASH,
    DBG_COMP_TIMER,
    DBG_COMP_APP,
    DBG_COMP_RF,
    DBG_COMP_DBGQ,   /* the record queue: overflow markers (DBG_DBGQ_RSN_*) */
    DBG_COMP_TIME,   /* the bring-up stopwatch: when, not what went wrong */
    DBG_COMP_WITNESS, /* a hardware register read back */
    DBG_COMP_UART,   /* the SERCOM USART host link (samr21_uart.c) */
    DBG_COMP_RADIO,  /* a radio back-end that says what it will not do (hal/mg21/own/mg21_radio.c) */
    DBG_COMP_WDT,    /* the ConBee III watchdog bring-up (hal/mg21/mg21_wdt.c) */
    DBG_COMP_MON     /* the bench register monitor's replies (app/monitor_app.c) */
} Dbg_Component;

/* Core reasons. 2..8 name the ARMv8-M vector that was taken (hal/mg21/mg21_panic.c), so a fault is
 * reported as itself rather than as "an exception". 9 = a fault recorded by a previous run, replayed
 * at the next boot from SRAM the soft reset left intact. */
enum
{
    DBG_CORE_RSN_EXCEPTION = 1,
    DBG_CORE_RSN_NMI,
    DBG_CORE_RSN_HARDFAULT,
    DBG_CORE_RSN_MEMFAULT,
    DBG_CORE_RSN_BUSFAULT,
    DBG_CORE_RSN_USAGEFAULT,
    DBG_CORE_RSN_SVCALL,
    DBG_CORE_RSN_DEBUGMONITOR,
    DBG_CORE_RSN_REPLAYED
};

/* Startup reasons. */
enum
{
    DBG_STARTUP_RSN_BOOT = 1,
    DBG_STARTUP_RSN_DATA_COPY_FAIL
};

/* Clock reasons (BTL-mirrored stages; at a helper line = settled, at a wait line
 * = bounded wait timed out). */
enum
{
    DBG_CLOCK_RSN_48MHZ_READY = 1,
    DBG_CLOCK_RSN_DFLL_NOLOCK = 2,
    DBG_CLOCK_RSN_GCLK_SWRST = 3,
    DBG_CLOCK_RSN_GEN1 = 4,
    DBG_CLOCK_RSN_GEN2 = 5,
    DBG_CLOCK_RSN_GEN3 = 6,
    DBG_CLOCK_RSN_GEN4 = 7,
    DBG_CLOCK_RSN_DFLL_ON = 8,
    DBG_CLOCK_RSN_CPU_48MHZ = 9,
    DBG_CLOCK_RSN_WDT_ON = 10,
    DBG_CLOCK_RSN_CH_USB = 11,
    DBG_CLOCK_RSN_DFLL_REF = 12,
    /* MG21 clock bring-up (hal/mg21/mg21_clock.c). The part starts on FSRCO and this board runs on
     * HFXO; the two regimes have to be told apart because every divisor on the board is computed from
     * whichever one it ended in. Numbered from 40 on purpose: 13..31 of this component is the
     * SAMR21 clock module's own local vocabulary (samr21_clock.c), and reason codes are
     * module-local, so a ConBee III code must not land in that band (docs/coding-rules.md §5). */
    DBG_CLOCK_RSN_HFXO_ON = 40,    /* HFXO reported RDY after the on-demand bypass */
    DBG_CLOCK_RSN_HFXO_NO_RDY,     /* the bounded wait ran out: no crystal, so no 38.4 MHz */
    DBG_CLOCK_RSN_SYSCLK_HFXO,     /* SYSCLKCTRL read back naming HFXO */
    DBG_CLOCK_RSN_SYSCLK_FSRCO,    /* SYSCLKCTRL read back naming FSRCO: the 20 MHz fallback */
    DBG_CLOCK_RSN_GROUP_CLK,       /* EM01GRPACLKCTRL and EM23GRPACLKCTRL written */
    DBG_CLOCK_RSN_TICK_CORE,       /* the millisecond came from the processor clock */
    DBG_CLOCK_RSN_TICK_DEAD,       /* no SysTick edge seen: the board has no clock at all */
    DBG_CLOCK_RSN_CYCLES_OFF,      /* DWT's cycle counter does not count: no cycle-level stopwatch */
    DBG_CLOCK_RSN_DONE,            /* clock bring-up finished; the line field is the regime */
    /* Guard code, kept after DONE so no stable code is renumbered: the millisecond assumes the core
     * runs at SYSCLK, which holds only while HCLKPRESC is 0 (the board's design point). If a
     * neighbour sets it, the line field carries the HCLKPRESC value and the tick is wrong. */
    DBG_CLOCK_RSN_HCLK_PRESC,
    /* ATmega256RFR2 clock bring-up (hal/avr/avr_clock.c), from 60 for the same reason the MG21's start
     * at 40. 60 = CLKPR read back after the prescaler was cleared, line = its value: 0 is the board's
     * 16 MHz, anything else is the divided rate the tick and the link were computed for. */
    DBG_CLOCK_RSN_AVR_PRESCALER = 60
};

/* Watchdog reasons (hal/mg21/mg21_wdt.c): 1 = armed on the clock it was configured for, 2 = armed on
 * the always-on 1 kHz oscillator because LFRCO never reported ready, 3 = a bounded wait on the
 * clock-select crossing or on a feed still synchronising ran out. */
enum
{
    DBG_WDT_RSN_ARMED = 1,
    DBG_WDT_RSN_CLOCK_FALLBACK,
    DBG_WDT_RSN_SYNC_TIMEOUT,
    /* The ATmega256RFR2 watchdog (hal/avr/avr_wdt.c): 4 = the timed WDTCSR write did not take, line =
     * the value read back, and the image runs unprotected. ARMED carries WDTCSR there. */
    DBG_WDT_RSN_AVR_REFUSED
};

/* Radio reasons of the ConBee III back-end (hal/mg21/own/mg21_radio.c, docs/hardware_mg21.md §10.5).
 * 1 = Radio_Init refused; the line field says why (MG21_RADIO_REFUSE_*). 2 = an entry this back-end
 * does not implement was called; the line field names it (MG21_RADIO_CALL_*). 3 = the radio is up;
 * line = the sequencer image's words. 4 = the receive buffer overflowed and was recovered; line =
 * how many times so far. 5 = a receive record that is not a frame; line = its length octet, and the
 * buffer was cleared. 6 = the state machine did not reach OFF within its bound; line = the state it
 * was in. 7 = a witness of RAC.STATUS's state field (line). */
enum
{
    DBG_RADIO_RSN_REFUSED = 1,
    DBG_RADIO_RSN_UNSUPPORTED,
    DBG_RADIO_RSN_UP,
    DBG_RADIO_RSN_RX_OVERFLOW,
    DBG_RADIO_RSN_RX_BAD_RECORD,
    DBG_RADIO_RSN_OFF_TIMEOUT,
    DBG_RADIO_RSN_STATE
};

/* USB reasons. */
enum
{
    DBG_USB_RSN_ENUMERATED = 1,
    DBG_USB_RSN_BUS_RESET,
    DBG_USB_RSN_SETUP_UNHANDLED,
    /* SETUP already in the EP0 OUT bank when its latch was seen; clearing the
   * latch to read it lets a concurrent SETUP overwrite the buffer (29.6.2.6).
   * Code 7: the USBC bridge keeps 4/5/6 for its sync timeouts. */
    DBG_USB_RSN_SETUP_LATCHED = 7,
    /* Published once at SET_CONFIGURATION; line field = the count. */
    DBG_USB_RSN_SETUP_COUNT
};

/* CDC reasons: 1 = an armed data-IN record lost to a bus reset/unconfigure
 * (samr21_usb_device.c), one marker per record; 2 = the same for a CDC120
 * notification, newest state wins. */
enum
{
    DBG_CDC_RSN_IN_DISCARDED = 1,
    DBG_CDC_RSN_NOTIF_DISCARDED = 2
};

/* UART reasons: the SERCOM USART host link (hal/samr21/samr21_uart.c). 1 = the link is
 * up; 2..5 = a bounded sync wait ran out, each naming the register it was waiting on (a
 * stuck SYNCBUSY means the clock routing has no edges). 6 = the line rate cannot be
 * sampled at 16x from the board's 48 MHz core clock (datasheet Table 23-2). 7 and 8 are
 * one marker per octet the receiver could not keep - counted the way the CDC link counts
 * a discarded record, by counting markers. */
enum
{
    DBG_UART_RSN_INIT_DONE = 1,
    DBG_UART_RSN_GCLK_SYNC,
    DBG_UART_RSN_SWRST,
    DBG_UART_RSN_CTRLB,
    DBG_UART_RSN_ENABLE,
    DBG_UART_RSN_BAUD_REFUSED,
    DBG_UART_RSN_RX_LOST,     /* STATUS.BUFOVF: the octet was not delivered */
    DBG_UART_RSN_RX_FRAME_ERR, /* STATUS.FERR: a stop bit read low */
    /* The ConBee III link (hal/mg21/mg21_uart.c). 9 = a bounded wait on the block's own enable
     * status never settled; 10 = the receiver still reported data pending after a bounded clear;
     * 11 = the clock the link is supposed to run on was never named, so no divisor can be
     * computed for it. */
    DBG_UART_RSN_MG21_ENABLE = 9,
    DBG_UART_RSN_MG21_RX_CLEAR,
    DBG_UART_RSN_MG21_NO_LINK_CLOCK
};

/* Flash reasons: an NVMCTRL READY handshake broke. 1 = READY never seen before
 * the command (controller never idle); 2 = READY never came back after it
 * (tFRE/tFPP window). Line field = the wait. */
enum
{
    DBG_FLASH_RSN_READY_BEFORE = 1,
    DBG_FLASH_RSN_READY_AFTER,
    /* Which user-row offset the factory EUI-64 came from: 0x0A (deCONZ/dresden
   * layout), 0x08 (newer aligned), 0 = neither. */
    DBG_FLASH_RSN_EUI_SOURCE,
    /* One 16-bit word of the NVM user row 0x804000..0x80403F, ascending, 32 per
   * dump. */
    DBG_FLASH_RSN_USERROW,
    /* The ConBee III's factory EUI-64, read once per boot from the user data page's manufacturing
   * token (hal/mg21/mg21_nvram.c, docs/hardware_mg21.md §10.2); line field = 1 an address, 0 none. */
    DBG_FLASH_RSN_MG21_IDENTITY,
    /* The ConBee I / RaspBee I factory EUI-64, read once per boot from the EEPROM's NV basic set
   * (hal/avr/avr_nvram.c, docs/hardware_avr.md §11.3); line field = 1 an address, 0 none. */
    DBG_FLASH_RSN_AVR_IDENTITY
};

/* App reasons. FLASHER_RESET: a host GCF "reset into the bootloader" frame was
 * matched, or the pending software reset fired. FRAMES_RX / _FILTERED: data-path
 * counters published every 5 s (app/rcp_app.c); line field = a count. */
enum
{
    DBG_APP_RSN_HELLO = 1,
    DBG_APP_RSN_FLASHER_RESET,
    DBG_APP_RSN_FRAMES_RX,
    DBG_APP_RSN_FRAMES_FILTERED,
    DBG_APP_RSN_SCAN_REFUSED,   /* a dwell the scan machine would not begin; the value is the mask */
    DBG_APP_RSN_SCAN_RX_DROP,   /* frames suppressed while a dwell held the air (cumulative) */
    DBG_APP_RSN_SCAN_STALE,     /* readings a finished dwell could not use: refused, unconfirmed or
                                   mid-reception (docs/hardware_samr21.md §6) */
    DBG_APP_RSN_SCAN_RESTORE,   /* a dwell could not put the host's receive state back; the ordinary
                                   apply path retries it next pass */
    DBG_APP_RSN_SCAN_SAMPLES,   /* a finished dwell's usable readings, so "how much did it measure"
                                   is a record rather than an inference */
    DBG_APP_RSN_SCAN_MS,         /* how long that dwell actually ran, in milliseconds of the board's own
                                   clock: the only way to tell a period from a pace */
    DBG_APP_RSN_LINK_INIT_FAIL   /* COM_Init reported the host link could not be armed (hal/com.h); the
                                   link then never reports present, and this record is the only sign of it */
};

/* RF / SERCOM4-SPI reasons: a bounded sync wait ran out, each naming the register
 * the code gave up on (a stuck SYNCBUSY means a dead clock source; writes are
 * ignored while synchronising). 6..8 are the three wake proofs - each names the
 * claim about the transceiver that did not survive a read-back. */
enum
{
    DBG_RF_RSN_GCLK_SYNC = 1,
    DBG_RF_RSN_SWRST,
    DBG_RF_RSN_CTRLB,
    DBG_RF_RSN_ENABLE,
    /* Rf_WakeAndForceClkm() returned 0 in Clock_Init: GCLKIN may still carry the
   * previous firmware's rate. */
    DBG_RF_RSN_WAKE_FAIL,
    /* Proof 1: PART_NUM (0x1C) never read 0x0B - nothing the following SPI
   * traffic "wrote" can be attributed to it (at86rf233.txt:2780, :2844-2854). */
    DBG_RF_RSN_PROOF_NO_IDENTITY,
    /* Proof 2: TRX_STATUS did not hold one state over >= 4 consecutive reads
   * (Table 7-3 :5328-5396). */
    DBG_RF_RSN_PROOF_NO_STATE,
    /* Proof 3: the TRX_CTRL_0 read-back was not the value written (9.6.4 note 1,
   * at86rf233.txt:13881-13895). */
    DBG_RF_RSN_PROOF_NO_SETTING,
    /* The radio data path (hal/radio.h, samr21_radio.c). */
    DBG_RF_RSN_RADIO_INIT_FAIL, /* Rf_HwInit / PART_NUM / TRX_OFF did not come up */
    DBG_RF_RSN_SPI_BAUD_FAIL,   /* the post-CPU-switch bit rate was refused */
    DBG_RF_RSN_RX_CRC_BAD,      /* frame delivered with an invalid FCS (RX_ON) */
    DBG_RF_RSN_RX_DROPPED,      /* RX ring full: newest frame dropped and counted */
    DBG_RF_RSN_RX_MODE,         /* mode applied; line = TRX_STATUS read back (0x06
                                 * RX_ON, 0x16 RX_AACK_ON, 0x11 BUSY_RX_AACK, 0x08
                                 * TRX_OFF) */
    DBG_RF_RSN_RX_MODE_FAIL,    /* a mode switch did not reach a receive state */
    DBG_RF_RSN_RX_FCS_DROPPED,  /* invalid-FCS frame dropped because the mode
                                 * filters; line = mode (0/1/2) */
    DBG_RF_RSN_RX_PROM_BIT,     /* retired: the receive path no longer uses RX_AACK_ON */
    DBG_RF_RSN_ED_BAD_STATE,      /* a measurement was asked for in a state that cannot measure;
                                 * line = the TRX_STATUS that refused it */
    DBG_RF_RSN_ED_TIMEOUT,        /* IRQ_4 (CCA_ED_DONE) never arrived for a measurement request */
    DBG_RF_RSN_ED_NO_MEASURE,     /* IRQ_4 arrived and the level was untouched: the datasheet's
                                 * warning, reported as it is; line = the register value */
    DBG_RF_RSN_TX_TOO_LONG,     /* PSDU + FCS does not fit the frame buffer */
    DBG_RF_RSN_TX_TIMEOUT,      /* never left the state we asked for, or BUSY_TX */
    DBG_RF_RSN_TX_CCA_FAIL,     /* clear-channel assessment saw a busy channel */
    DBG_RF_RSN_TX_NO_ACK,       /* ARET: not acknowledged after its retries
                                 * (TRAC_STATUS = NO_ACK) */
    /* Retired radio probe (removed with app/probes.c): the acceptance
   * verdict without a console. The sequence number is the byte after the 2-byte
   * FCF, so PROBE_TX (seq<<8 | len) and PROBE_RX_HEAD (psdu[2]<<8 | psdu[3]) name
   * the same frame at both ends. */
    DBG_RF_RSN_PROBE_TX,
    DBG_RF_RSN_PROBE_TX_FAIL, /* line = the RADIO_TX_* status */
    DBG_RF_RSN_PROBE_RX,      /* line = len | lqi<<8 */
    DBG_RF_RSN_PROBE_RX_RSSI, /* line = RSSI in dBm, low byte (two's complement) */
    DBG_RF_RSN_PROBE_RX_HEAD, /* line = psdu[2]<<8 | psdu[3] */
    /* The radio vector's statistics, published by Radio_Witnesses (the vector itself emits
     * nothing). Counts are 16-bit and wrap. */
    DBG_RF_RSN_ISR_FRAMES,        /* frames read at TRX_END */
    DBG_RF_RSN_ISR_ACKS,          /* Acks composed and started */
    DBG_RF_RSN_ISR_ACKS_PENDING,  /* of those, with Frame Pending set */
    DBG_RF_RSN_ISR_ACKS_LATE,     /* Acks abandoned: past RADIO_ACK_LATEST_US */
    DBG_RF_RSN_ISR_ACK_NO_ROOM,   /* Acks withheld because the RX ring was full */
    DBG_RF_RSN_ISR_SPI_FAIL,      /* the Ack's state change, buffer write or TX_START failed */
    DBG_RF_RSN_ISR_ACK_STUCK,     /* an Ack never ended and the loop recovered RX */
    DBG_RF_RSN_ISR_ACK_CYCLES,    /* line = cycles from the TRX_END stamp to TX_START */
    DBG_RF_RSN_ISR_READ_CYCLES,   /* line = cycles of the last Frame Buffer read */
    DBG_RF_RSN_ISR_CRC_BAD,       /* frames whose FCS failed (delivered only in FULL) */
    DBG_RF_RSN_ISR_READ_FAIL,     /* a Frame Buffer read failed or its PHR exceeded 127 */
    DBG_RF_RSN_ISR_MIC_CYCLES,    /* line = cycles from the RX_END stamp to the Enhanced Ack's MIC in the buffer */
};

/* Bring-up stopwatch records (component DBG_COMP_TIME): no fault, only a when.
 * The line field is not a source line but elapsed processor cycles since
 * Clock_BringupClockStart(), in 4096-cycle units (0.512 ms at 8 MHz, 85 us at
 * 48 MHz, saturating at 65 535 units). Cycles because during bring-up the CPU
 * clock is the thing being brought up. (samr21_clock.c, Clock_Stamp.) */
enum
{
    DBG_TIME_RSN_ENTRY = 1,
    DBG_TIME_RSN_WDT_PROBE,  /* the watchdog's clock channel read out */
    DBG_TIME_RSN_WAKE,       /* RF233 woken, CLKM forced */
    DBG_TIME_RSN_REF_CHAIN,  /* PC16 -> GCLKIN -> gen1 -> ch0 */
    DBG_TIME_RSN_DFLL_DONE,  /* closed-loop lock, or the USBCRM fallback */
    DBG_TIME_RSN_CPU_SWITCH, /* GEN0 <- DFLL48M */
    DBG_TIME_RSN_USB_CLOCK,  /* GEN5 <- DFLL48M, GCLK_USB <- gen5 */
    DBG_TIME_RSN_CLOCKS_EXIT,
    DBG_TIME_RSN_LINK_ENTER,
    DBG_TIME_RSN_LINK_EXIT, /* the host link came up (hal/com.h COM_Init) */
    DBG_TIME_RSN_HAL_EXIT, /* HAL_Init returned, IRQs unlocked */
    DBG_TIME_RSN_MAIN_FIRST,
    DBG_TIME_RSN_CONFIGURED, /* SET_CONFIGURATION committed on the wire */
    /* The USB bring-up breakdown: its three steps. */
    DBG_TIME_RSN_USBC_DESCS, /* identity read from NVM */
    DBG_TIME_RSN_USBC_HAL,   /* pads, PM gate, PADCAL, DESCADD */
    DBG_TIME_RSN_USBC_DRV,   /* SWRST, ENABLE, EP0, attach */
    /* The usbd_init breakdown: its four HAL calls (samr21_usbd_hal.c). */
    DBG_TIME_RSN_USBC_SWRST,  /* CTRLA.SWRST + sync + DESCADD */
    DBG_TIME_RSN_USBC_ENABLE, /* ENABLE, INTENSET, EPINTENSET, ISER */
    DBG_TIME_RSN_USBC_ATTACH, /* DETACH cleared (also usbd_ep0_init) */
    /* Stopwatch honesty check: a fixed 4096-iteration loop of core-register reads
   * and RAM adds. A stage number is a lower bound (the pump credits one SysTick
   * period per pump); if this reports ~4096 periods the big stage numbers are
   * wrap artifacts. */
    DBG_TIME_RSN_CALIB,
    /* Retired SOF probe: (SOF_20 - SOF_START) /
   * SOF_PROBE_FRAMES = Hz / 1000 (48 000 cycles/frame at 48 MHz, 8 000 at 8 MHz);
   * SOF_MISS = frame budget ran out. */
    DBG_TIME_RSN_SOF_START, /* first acknowledged frame: baseline stamped */
    DBG_TIME_RSN_SOF_END,   /* the wait's other end: line = cycles again */
    /* Here the line field is host frames the hardware counted (FNUM.FNUM, 11-bit)
   * between START and END, not cycles. */
    DBG_TIME_RSN_SOF_FRAMES,
    /* Line field = host frames our vector acknowledged (INTFLAG.SOF seen and
   * cleared) over the same window. */
    DBG_TIME_RSN_SOF_FLAGS,
    DBG_TIME_RSN_SOF_MISS, /* bounded poll budget, no frames */
    /* Retired attach probe: four connect events, fixed tick spacing apart. */
    DBG_TIME_RSN_ATTACH_DETACH1,
    DBG_TIME_RSN_ATTACH_1,
    DBG_TIME_RSN_ATTACH_DETACH2,
    DBG_TIME_RSN_ATTACH_2,
    /* Tick_GetMs() truncated to 16 bits, at the same instant as SOF_END. */
    DBG_TIME_RSN_TICK_MS,
    /* Cadence probe (app/main.c): line field = raw SysTick LOAD counter (periods),
   * not cycles/4096. */
    DBG_TIME_RSN_CADENCE,
    /* Clock-probe step marker: line field = the step number. */
    DBG_TIME_RSN_PROBE_MARK
};

/* Register witnesses (component DBG_COMP_WITNESS): each reason names one register
 * and the line field is that register's 16-bit value - not a source line. Bit
 * positions are in each reason's comment. */
enum
{
    DBG_WITNESS_RSN_OSC8M = 1,   /* SYSCTRL.OSC8M: PRESC[9:8], ONDEMAND 7, RUNSTDBY 6,
                                * ENABLE 1 (samr21.txt:34009ff) */
    DBG_WITNESS_RSN_PCLKSR,      /* SYSCTRL.PCLKSR: OSC8MRDY 3, DFLLRDY 4, DFLLLCKF 6,
                                * DFLLLCKC 7 (the lock bits are unreliable in USBCRM,
                                * errata 11938) */
    DBG_WITNESS_RSN_DFLLCTRL,    /* DFLLCTRL: MODE 2 (1 = closed loop), ENABLE 1,
                                * USBCRM 5, RUNSTDBY 6, ONDEMAND 7, CCDIS 8, BPLCKC 10 */
    DBG_WITNESS_RSN_DFLLVAL,     /* DFLLVAL: FINE[9:0], COARSE[15:10] - a regulating
                                * loop sits mid-range, not at a rail */
    DBG_WITNESS_RSN_DFLLMUL,     /* DFLLMUL.MUL[15:0]: 1500 with a 32 kHz reference
                                * (CLKM 8 MHz / GENDIV 250) lands on 48 MHz */
    DBG_WITNESS_RSN_DFLLMUL_HI,  /* DFLLMUL[31:16]: FSTEP[7:0]|CSTEP[5:0]<<8; does not
                                * fit the 16-bit MUL record above */
    DBG_WITNESS_RSN_GCLK_STATUS, /* GCLK.STATUS: SYNCBUSY 7 */
    DBG_WITNESS_RSN_GCLK_CTRL,   /* GCLK.CTRL: SWRST 0 */
    DBG_WITNESS_RSN_USB_INTEN,   /* USBC.INTEN (INTENSET): SOF 2, RST 3, SUSP 4,
                                * WAKEUP 5, UPRSM 6, EORSM 7, DDISC 11, DCONN 12
                                * (samr21.txt:36826ff) */
    DBG_WITNESS_RSN_USB_INTFLAG, /* USBC.INTFLAG, same bit positions */
    DBG_WITNESS_RSN_USB_STATUS,  /* USBC.STATUS: LINESTATE[1:0], SPEED[3:2]
                                * (0x0 low, 0x1 full; Table 29-13, samr21.txt:37425-37437) */
    DBG_WITNESS_RSN_USB_FNUM,    /* USBC.FNUM: FNUM[10:5] at 13:8, FNUM[4:0] at 7:3,
                                * MFNUM 2:0 (Table 29-16, samr21.txt:37456-37466) */
    /* gen0 is GCLK_MAIN (CPU; 13.6.2.3, samr21.txt:5094-5100), gen1 the GCLKIN
   * divider feeding GCLK_DFLL48M_REF. A generator state does not fit one value -
   * SRC at GENCTRL[12:8], GENEN 16, DIVSEL 20, DIV at GENDIV[23:8] - so each takes
   * two records: control compacted into bits 0-8 (SRC 0-3, GENEN 4, IDC 5, DIVSEL 6,
   * RUNSTDBY 7, OE 8) and DIV[15:0] alone. */
    DBG_WITNESS_RSN_GEN0_CTRL,
    DBG_WITNESS_RSN_GEN0_DIV,
    DBG_WITNESS_RSN_GEN1_CTRL,
    DBG_WITNESS_RSN_GEN1_DIV,
    /* PM.CPUSEL.CPUDIV[2:0]; CPU clock = gen0 / 2^CPUDIV (samr21.txt:6631, :6769). */
    DBG_WITNESS_RSN_PM_CPUSEL,
    /* GCLK CLKCTRL for channel 0 (GCLK_DFLL48M_REF) and channel 6 (GCLK_USB):
   * ID[5:0], GEN[11:8], CLKEN 14, WRTLOCK 15 (samr21_regs.h:103-106). */
    DBG_WITNESS_RSN_CLKCTRL_REF,
    DBG_WITNESS_RSN_CLKCTRL_USB,
    /* SysTick LOAD[23:0] (a read value, not an assumption) and CTRL, whose
   * CLKSOURCE bit 2 says the counter follows the processor clock. */
    DBG_WITNESS_RSN_SYSTICK_LOAD,
    DBG_WITNESS_RSN_SYSTICK_CTRL,
    /* Core clock against the host SOF (1 kHz): line field = processor clocks per
   * host frame / 16, so f_CPU = value x 16 x 1000 Hz and 48 MHz reads 3000. */
    DBG_WITNESS_RSN_SOF_CYCLES,
    /* Tick_GetMs() truncated to 16 bits; two witness sets give a delta in app ms. */
    DBG_WITNESS_RSN_TICK_MS,
    /* Generator 5 (USB), read like gen0/gen1: its source and divider beside gen0's
   * say whether CPU and USB share a clock. */
    DBG_WITNESS_RSN_GEN5_CTRL,
    DBG_WITNESS_RSN_GEN5_DIV,
    /* The SERCOM USART host link (samr21_uart.c). CTRLA splits in two because its pad
     * routing sits above the 16-bit line field: CTRLA is SAMPR[15:13], MODE[4:2],
     * ENABLE 1, SWRST 0; CTRLA_PADS is TXPO[1:0] at 1:0 and RXPO[1:0] at 5:4. CTRLB is
     * RXEN 17, TXEN 16, SBMODE 7, CHSIZE[2:0] at 2:0. INTENSET is DRE 0, TXC 1, RXC 2,
     * ERROR 7. STATUS is PERR 0, FERR 1, BUFOVF 2, CTS 3. BAUD is the arithmetic 16-bit
     * register value (datasheet 24.8). */
    DBG_WITNESS_RSN_UART_CTRLA,
    DBG_WITNESS_RSN_UART_CTRLA_PADS,
    DBG_WITNESS_RSN_UART_CTRLB,
    /* CTRLB's two enable bits sit at 17:16, above the 16-bit line field, so CTRLB alone reads
     * 0 on a working link: CTRLB_PADS is (CTRLB >> 16), RXEN at 1 and TXEN at 0. */
    DBG_WITNESS_RSN_UART_CTRLB_PADS,
    DBG_WITNESS_RSN_UART_BAUD,
    DBG_WITNESS_RSN_UART_INTENSET,
    DBG_WITNESS_RSN_UART_STATUS,
    /* The ConBee III bring-up read-backs. A 32-bit register whose interesting bits sit above bit 15
     * travels as two records, the low half under this reason and the high half under the _HI one,
     * the way the SAMR21's CTRLA and CTRLB do. */
    DBG_WITNESS_RSN_MG21_SYSCLKCTRL,   /* CLKSEL[2:0], PCLKPRESC 10, HCLKPRESC[13:12] */
    DBG_WITNESS_RSN_MG21_EM01GRPACLK,  /* USART0 and GPIO's clock source */
    DBG_WITNESS_RSN_MG21_EM23GRPACLK,  /* the low-frequency group clock SysTick can take */
    DBG_WITNESS_RSN_MG21_HFXO_STATUS,  /* RDY 0, ENS 16, HWREQ 17 */
    DBG_WITNESS_RSN_MG21_HFXO_STATUS_HI, /* the same register's high half */
    DBG_WITNESS_RSN_MG21_HFXO_CTRL,    /* FORCEEN 0, DISONDEMAND 1 */
    DBG_WITNESS_RSN_MG21_LFRCO_STATUS, /* RDY 0 */
    DBG_WITNESS_RSN_MG21_LFRCO_STATUS_HI, /* ENS 0 of the high half: enabled state */
    DBG_WITNESS_RSN_MG21_CMU_STATUS,   /* the oscillator ready bits CMU reports */
    DBG_WITNESS_RSN_MG21_WDOG_CFG,     /* PERSEL[19:16] and the run/EM4 bits */
    DBG_WITNESS_RSN_MG21_WDOG_CFG_HI,
    DBG_WITNESS_RSN_MG21_WDOG_EN,      /* EN 0 */
    DBG_WITNESS_RSN_MG21_WDOG_CLKSEL,  /* CMU_WDOG0CLKCTRL: what the watchdog counts */
    DBG_WITNESS_RSN_MG21_SYSTICK_LOAD, /* the reload actually written */
    DBG_WITNESS_RSN_MG21_SYSTICK_CTRL, /* ENABLE 0, TICKINT 1, CLKSOURCE 2 */
    DBG_WITNESS_RSN_MG21_TICK_PERIOD,  /* SysTick periods per millisecond, or 1 for a whole one */
    DBG_WITNESS_RSN_MG21_CYCCNT,       /* DWT CYCCNT low half: proof the cycle counter runs */
    DBG_WITNESS_RSN_MG21_CHIPREV,      /* SYSCFG CHIPREV: which revision this board is */
    DBG_WITNESS_RSN_MG21_SYSTIC_CFG,   /* SYSCFG CFGSYSTIC: SYSTICEXTCLKEN 0, the SysTick gate */
    DBG_WITNESS_RSN_MG21_USART_CTRL,   /* OVS[6:5] and the mode bits */
    DBG_WITNESS_RSN_MG21_USART_FRAME,  /* DATABITS[3:0], PARITY[9:8], STOPBITS[13:12] */
    DBG_WITNESS_RSN_MG21_USART_CLKDIV, /* CLKDIV[15:0] */
    DBG_WITNESS_RSN_MG21_USART_CLKDIV_HI, /* CLKDIV[22:16]: the integer part of the divisor */
    DBG_WITNESS_RSN_MG21_USART_EN,     /* EN 0 */
    DBG_WITNESS_RSN_MG21_USART_STATUS, /* RXENS 0, TXENS 1, RXDATAV 7, TXIDLE 13 */
    DBG_WITNESS_RSN_MG21_USART_IEN,    /* TXBL 1, RXDATAV 2, RXOF 4, FERR 9 */
    DBG_WITNESS_RSN_MG21_GPIO_ROUTEEN, /* TXPEN 4, RXPEN 2 */
    DBG_WITNESS_RSN_MG21_GPIO_TXROUTE, /* port [1:0], pin [19:16] */
    DBG_WITNESS_RSN_MG21_GPIO_RXROUTE,
    DBG_WITNESS_RSN_MG21_GPIO_MODEL,   /* a port's pin modes, four bits each, pins 0..7 */
    DBG_WITNESS_RSN_MG21_GPIO_TXROUTE_HI, /* the same route word's pin field, [3:0] of the half */
    DBG_WITNESS_RSN_MG21_GPIO_RXROUTE_HI,
    DBG_WITNESS_RSN_MG21_UART_VECTORS, /* link vector entries since boot (16-bit, wraps) */
    DBG_WITNESS_RSN_MG21_UART_RX_OF,   /* octets the receiver dropped as it overflowed */
    DBG_WITNESS_RSN_MG21_UART_RX_FERR, /* octets delivered as framed wrong */
    DBG_WITNESS_RSN_MG21_FAULT_COMPONENT, /* what a previous run panicked in */
    DBG_WITNESS_RSN_MG21_FAULT_DETAIL,    /* the reason it panicked with */
    DBG_WITNESS_RSN_MG21_USART_RATE,   /* the line rate the divisor actually produces, low half */
    DBG_WITNESS_RSN_MG21_USART_RATE_HI,
    /* The ConBee III live link report, repeated while the image runs (hal/mg21/mg21_uart.c): the
     * heartbeat record first, then the live state of the receive path. */
    DBG_WITNESS_RSN_MG21_HEARTBEAT,       /* seconds since boot (16-bit): a live report follows */
    DBG_WITNESS_RSN_MG21_UART_RX_VECTORS, /* receive vector entries since boot (16-bit, wraps) */
    DBG_WITNESS_RSN_MG21_UART_RX_OCTETS,  /* octets handed to the link since boot (16-bit, wraps) */
    DBG_WITNESS_RSN_MG21_UART_RX_LAST,    /* RXDATAX of the last octet read: data [8:0], FERR 15 */
    DBG_WITNESS_RSN_MG21_USART_IF,        /* TXC 0, TXBL 1, RXDATAV 2, RXOF 4, FERR 9 */
    DBG_WITNESS_RSN_MG21_GPIO_DIN,        /* the link port's input levels: RX on PB0 idles at 1 */
    DBG_WITNESS_RSN_MG21_NVIC_ISER0,      /* enabled IRQs 0..15: USART0 RX 11, TX 12 */
    /* The ATmega256RFR2 bring-up (hal/avr/), one 8-bit register per record unless named a pair. */
    DBG_WITNESS_RSN_AVR_MCUSR,      /* the reset cause as the bootloader left it: PORF 0, EXTRF 1,
                                     * BORF 2, WDRF 3, JTRF 4; cleared after the read */
    DBG_WITNESS_RSN_AVR_MCUCR,      /* IVSEL 1: 0 = the vectors are this image's */
    DBG_WITNESS_RSN_AVR_CLKPR,      /* CLKPS[3:0]: 0 = undivided */
    DBG_WITNESS_RSN_AVR_SMCR,       /* SE 0, SM[3:1]: 0 = idle */
    DBG_WITNESS_RSN_AVR_WDTCSR,     /* WDE 3, WDP[5,2:0] */
    DBG_WITNESS_RSN_AVR_TCCR0A,     /* WGM01 1: CTC */
    DBG_WITNESS_RSN_AVR_TCCR0B,     /* CS0[2:0]: 3 = clk/64 */
    DBG_WITNESS_RSN_AVR_OCR0A,      /* the millisecond's compare value */
    DBG_WITNESS_RSN_AVR_TIMSK0,     /* OCIE0A 1 */
    DBG_WITNESS_RSN_AVR_UCSR0A,     /* U2X0 1, and the receiver's FE0 4, DOR0 3 */
    DBG_WITNESS_RSN_AVR_UCSR0B,     /* TXEN0 3, RXEN0 4, UDRIE0 5, RXCIE0 7 */
    DBG_WITNESS_RSN_AVR_UCSR0C,     /* UCSZ0[2:1] = 3: eight data bits */
    DBG_WITNESS_RSN_AVR_UBRR0,      /* UBRR0H:UBRR0L, the divisor actually written */
    DBG_WITNESS_RSN_AVR_USART_RATE, /* the line rate the divisor produces, low half */
    DBG_WITNESS_RSN_AVR_USART_RATE_HI,
    DBG_WITNESS_RSN_AVR_UART_VECTORS, /* link vector entries since boot (16-bit, wraps) */
    DBG_WITNESS_RSN_AVR_UART_RX_OF,   /* octets the receiver lost to an overrun (DOR0) */
    DBG_WITNESS_RSN_AVR_UART_RX_FERR, /* octets dropped as framed wrong (FE0) */
    DBG_WITNESS_RSN_AVR_FAULT_COMPONENT, /* what a previous run panicked in */
    DBG_WITNESS_RSN_AVR_FAULT_DETAIL,    /* the reason it panicked with */
    DBG_WITNESS_RSN_SAMR21_RCAUSE        /* PM.RCAUSE, the last reset's cause as the bootloader left
                                          * it: POR 0, BOD12 1, BOD33 2, EXT 4, WDT 5, SYST 6 */
};

/* Register monitor replies (DBG_COMP_MON, app/monitor_app.c, docs/hardware_mg21.md §10.5). One
 * reply is BEGIN (line = the request's tag), then per word VALUE_LO and VALUE_HI, or per trace sample
 * VALUE_LO, VALUE_HI, CYCLES_LO, CYCLES_HI, then END (line = Mon_Status | samples << 8). DROPPED
 * reports a command that arrived while one was pending (line = the running count). */
enum
{
    DBG_MON_RSN_BEGIN = 1,
    DBG_MON_RSN_VALUE_LO,
    DBG_MON_RSN_VALUE_HI,
    DBG_MON_RSN_CYCLES_LO,
    DBG_MON_RSN_CYCLES_HI,
    DBG_MON_RSN_END,
    DBG_MON_RSN_DROPPED
};

/* One record's size in the queue and on the wire; exported because the transport
 * accounts for whole records when a link event discards an armed packet. */
#define DBG_FRAME_SIZE 8u

/* Longest string in the tree - the USB device identity, not a debug string (coding-rules.md §5).
 * usb/usb_desc.c asserts its tables against this so an oversized identity fails
 * the build; 40 = our iManufacturer (USB 2.0 ch. 9.6.2). */
#define DBG_STRING_MAX 40u

#define DBG_MAGIC 0x0000u

/* An extra, board-owned destination for every record (the SAMR21 board writes
 * the post-mortem scratch row); Debug_Emit is a no-op until one is installed. */
typedef void (*Dbg_Sink)(uint16_t component, uint16_t line, uint16_t reason);
void Debug_SetSink(Dbg_Sink fn);

/* Queued in the caller's context queue, then handed to the sink. Safe from any
 * context, never blocks, never touches flash, never allocates. */
void Debug_Emit(uint16_t component, uint16_t line, uint16_t reason);

/* ---- Record queue (the CDC bulk IN transport drains it) ---- */
/* Two fixed rings, one per producer context (thread / handler), so an IRQ and a
 * main-loop record never share a slot; each ring keeps one slot free so a drain
 * can reserve its batch. Overflow drops the OLDEST and counts it
 * (DBG_COMP_DBGQ / DBG_DBGQ_RSN_OVERFLOW, line field = records dropped). */
/* 64 slots x 2 rings x DBG_FRAME_SIZE = 1 KiB of .bss out of 32 KiB. */
#define DBG_QUEUE_SLOTS 64u

enum
{
    DBG_DBGQ_RSN_OVERFLOW = 1,
    /* The records that follow are a re-run of a set already offered
   * (Debug_ShadowReplay); line field = records about to be replayed. */
    DBG_DBGQ_RSN_REPLAY
};

/* ---- Bring-up shadow (an attaching host can still see the boot) ---- */
/* Between SET_CONFIGURATION and the tty appearing, macOS's ACM driver reads and
 * discards, so the boot set is also kept here and re-queued on a timer. Hold
 * spans bring-up (main() starts it before HAL_Init); it is a copy, not a second
 * source of truth. The replay goes only into the queue, never through the sink. */
void Debug_ShadowHold(int on);
uint16_t Debug_ShadowReplay(void);

/* Drain whole frames into dst (thread-mode/main-loop only). Returns the bytes
 * written, a multiple of 8, 0 when empty; consumes what it writes. */
uint16_t Debug_QueueTake(uint8_t *dst, uint16_t maxBytes);

/* Records lost to overflow so far (saturating). */
uint16_t Debug_QueueDrops(void);

#define DBG(comp, reason) Debug_Emit((comp), (uint16_t)__LINE__, (reason))

#endif
