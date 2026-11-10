/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_phy.c — the RF bring-up, around Espressif's libphy.a and libbtbb.a (esp32_phy.h).

The sequence is the one ESP-IDF's esp_phy component runs before its 802.15.4 driver touches the MAC,
written from scratch after reading ESP-IDF v5.4.1's esp_phy and modem_clock sources (docs/hardware_esp32.md
§12.5):

  1. the modem clocks: for the PHY the analog I2C master and the front end's common and private clocks,
     for the 802.15.4 MAC its APB and MAC clocks, the shared BT/802.15.4 baseband, the event matrix and
     the coexistence block - the gates ESP-IDF's modem_clock.c opens for its PHY and IEEE802154 modules;
  2. the ROM's CPU frequency: the ROM's delays count CPU cycles, and the bootloader set them for the
     clock it left, not for the one esp32_clock.c switched to;
  3. register_chipv7_phy with the chip's default init table and a full calibration into a buffer this
     file owns: the archive's entry point;
  4. bt_bb_v2_init_cmplx: the baseband the 802.15.4 MAC shares with Bluetooth.

The calibration is not kept: this firmware is stateless across a reset, so every boot calibrates in full.
The init table is ESP-IDF's, evaluated by the fetch tool into the cache (its include path is set on this
translation unit only by hal/esp32/phy/phy.cmake). */
#include <stdint.h>

#include "dbg.h"
#include "esp32_clock.h"
#include "esp32_phy.h"
#include "esp32_regs.h"
#include "hal.h"

#define PHY_INIT_DATA_LEN 128u
#define PHY_CAL_DATA_LEN 1904u
#define PHY_RF_CAL_FULL 2
#define PHY_MAC_LEN 6u
/* What register_chipv7_phy answers for a calibration record that failed its check - on this firmware every
 * time, the buffer being empty - after which it calibrates in full (ESP-IDF esp_private/phy.h). */
#define PHY_CAL_DATA_CHECK_FAIL 1

/* The archive's types, as large as ESP-IDF declares them; their content is the archive's business. */
typedef struct
{
    uint8_t params[PHY_INIT_DATA_LEN];
} Esp32Phy_InitData;

typedef struct
{
    uint8_t version[4];
    uint8_t mac[PHY_MAC_LEN];
    uint8_t opaque[PHY_CAL_DATA_LEN - 4u - PHY_MAC_LEN];
} Esp32Phy_CalData;

typedef char esp32PhyCalDataSize[(sizeof(Esp32Phy_CalData) == PHY_CAL_DATA_LEN) ? 1 : -1];

/* The entry points, in the archives and the mask ROM. */
int register_chipv7_phy(const Esp32Phy_InitData *initData, Esp32Phy_CalData *calData, int calMode);
void bt_bb_v2_init_cmplx(int printVersion);
void ets_update_cpu_frequency(uint32_t ticksPerUs);

static const Esp32Phy_InitData phyInitData = {{
#include ESP32_PHY_INIT_DATA_INC
}};

static Esp32Phy_CalData phyCal;

static void phyClocksOn(void)
{
    ESP32_REG(ESP32_MODEM_LPCON_BASE, ESP32_MODEM_LPCON_CLK_CONF) |=
        ESP32_MODEM_LPCON_CLK_I2C_MST_EN | ESP32_MODEM_LPCON_CLK_COEX_EN;
    ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_CLK_CONF1) |=
        ESP32_MODEM_SYSCON_CLK_FE_APB_EN | ESP32_MODEM_SYSCON_CLK_FE_80M_EN | ESP32_MODEM_SYSCON_CLK_FE_160M_EN
        | ESP32_MODEM_SYSCON_CLK_FE_CAL_160M_EN | ESP32_MODEM_SYSCON_CLK_BT_APB_EN | ESP32_MODEM_SYSCON_CLK_BT_EN;
    ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_CLK_CONF) |=
        ESP32_MODEM_SYSCON_CLK_ETM_EN | ESP32_MODEM_SYSCON_CLK_ZB_APB_EN | ESP32_MODEM_SYSCON_CLK_ZB_MAC_EN;
    HAL_Dsb();
}

/* The station MAC the calibration record carries: the base MAC, most significant octet first. */
static void phyStationMac(uint8_t mac[PHY_MAC_LEN])
{
    uint32_t mac0 = ESP32_REG(ESP32_EFUSE_BASE, ESP32_EFUSE_MAC0);
    uint32_t mac1 = ESP32_REG(ESP32_EFUSE_BASE, ESP32_EFUSE_MAC1);

    mac[0] = (uint8_t)(mac1 >> 8);
    mac[1] = (uint8_t)mac1;
    mac[2] = (uint8_t)(mac0 >> 24);
    mac[3] = (uint8_t)(mac0 >> 16);
    mac[4] = (uint8_t)(mac0 >> 8);
    mac[5] = (uint8_t)mac0;
}

static uint32_t phyNowMs(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);
    return ms;
}

int Esp32Phy_Init(void)
{
    uint32_t t0;
    uint32_t t1;
    int result;

    phyClocksOn();
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_CLK_CONF) >> 16),
               DBG_WITNESS_RSN_ESP32_MODEM_CLK);
    Debug_Emit(DBG_COMP_WITNESS,
               (uint16_t)(ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_CLK_CONF1) >> 13),
               DBG_WITNESS_RSN_ESP32_MODEM_CLK1);

    ets_update_cpu_frequency(Esp32Clock_CpuHz() / 1000000u);
    phyStationMac(phyCal.mac);

    t0 = phyNowMs();
    result = register_chipv7_phy(&phyInitData, &phyCal, PHY_RF_CAL_FULL);
    t1 = phyNowMs();
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)result, DBG_WITNESS_RSN_ESP32_PHY_RESULT);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(t1 - t0), DBG_WITNESS_RSN_ESP32_PHY_MS);

    bt_bb_v2_init_cmplx(0);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(phyNowMs() - t1), DBG_WITNESS_RSN_ESP32_BTBB_MS);

    return result == 0 || result == PHY_CAL_DATA_CHECK_FAIL;
}
