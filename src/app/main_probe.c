/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* main_probe.c - the probe image's entry hooks (src/app/app_main.h).
 *
 * There is no session here: the loop's transport duty is to publish the records, which
 * COM_TxPoll takes out of the record queue on every pass. */
#include "app_main.h"

#include "com.h"
#include "hal_diag.h"

#ifdef DS_MONITOR
#include "monitor_app.h"
#endif

void App_Boot(void)
{
    /* This is the bring-up and post-mortem image: the last record in the NVRAM
     * scratch row is the whole point, so staging is always on. */
#ifdef DS_SCRATCH_ROW
    HAL_DiagStage(1);
#endif
}

void App_DataOut(const uint8_t *buf, uint16_t len)
{
#ifdef DS_MONITOR
    /* A board with the bench register monitor (docs/hardware_mg21.md §10.5) takes its commands on
     * the same link; the flasher's reset frame still reaches the trap in main.c first. */
    Monitor_Feed(buf, len);
#else
    /* The link's OUT carries the flasher's reset frame and nothing else in this image; the trap in
     * main.c is its only consumer. */
    (void)buf;
    (void)len;
#endif
}

void App_Poll(uint32_t ms)
{
    (void)ms;
#ifdef DS_MONITOR
    Monitor_Poll();
#endif
    COM_TxPoll();
}
