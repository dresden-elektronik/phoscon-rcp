/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_gpio.c — a pin as a plain output, through the IO_MUX and the GPIO matrix (TRM ch. 7).

A pin reaches GPIO_OUT when its IO_MUX function is GPIO and its output-matrix selection is the plain GPIO
signal; GPIO_ENABLE then drives it. The set and clear registers change one pin without a read-modify-
write, so an interrupt that drives another pin cannot be undone by this one. The pins themselves are the
board's (esp32_board.h). */
#include <stdint.h>

#include "dbg.h"
#include "esp32_board.h"
#include "esp32_gpio.h"
#include "esp32_regs.h"

#define GPIO(offset) ESP32_REG(ESP32_GPIO_BASE, (offset))
#define IO_MUX(offset) ESP32_REG(ESP32_IO_MUX_BASE, (offset))

void Esp32Gpio_Set(uint32_t pin, int level)
{
    if (level != 0)
        GPIO(ESP32_GPIO_OUT_W1TS) = 1u << pin;
    else
        GPIO(ESP32_GPIO_OUT_W1TC) = 1u << pin;
}

int Esp32Gpio_Output(uint32_t pin, int level)
{
    uint32_t mux;

    if (pin >= ESP32_GPIO_PINS)
        return 0;

    Esp32Gpio_Set(pin, level);
    GPIO(ESP32_GPIO_FUNC0_OUT_SEL_CFG + 4u * pin) = ESP32_GPIO_SIG_GPIO_OUT;

    mux = IO_MUX(ESP32_IO_MUX_GPIO0 + 4u * pin);
    mux &= ~ESP32_IO_MUX_MCU_SEL_MASK;
    mux |= ESP32_IO_MUX_FUNC_GPIO << ESP32_IO_MUX_MCU_SEL_SHIFT;
    IO_MUX(ESP32_IO_MUX_GPIO0 + 4u * pin) = mux;

    GPIO(ESP32_GPIO_ENABLE_W1TS) = 1u << pin;
    return 1;
}

void Esp32Board_PinsInit(void)
{
#ifdef ESP32_BOARD_RF_SWITCH_EN
    (void)Esp32Gpio_Output(ESP32_BOARD_RF_SWITCH_EN, ESP32_BOARD_RF_SWITCH_EN_LEVEL);
#endif
#ifdef ESP32_BOARD_ANTENNA_SEL
    (void)Esp32Gpio_Output(ESP32_BOARD_ANTENNA_SEL, ESP32_BOARD_ANTENNA_SEL_LEVEL);
#endif
#ifdef ESP32_BOARD_LED
    (void)Esp32Gpio_Output(ESP32_BOARD_LED, !ESP32_BOARD_LED_ON_LEVEL);
#endif
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)GPIO(ESP32_GPIO_OUT), DBG_WITNESS_RSN_ESP32_GPIO_OUT);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)GPIO(ESP32_GPIO_ENABLE), DBG_WITNESS_RSN_ESP32_GPIO_ENABLE);
}

void Esp32Board_Led(int on)
{
#ifdef ESP32_BOARD_LED
    Esp32Gpio_Set(ESP32_BOARD_LED, (on != 0) ? ESP32_BOARD_LED_ON_LEVEL : !ESP32_BOARD_LED_ON_LEVEL);
#else
    (void)on;
#endif
}
