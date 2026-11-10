/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_gpio.h — the ESP32 family's plain GPIO outputs, and the board pins built on them
 * (docs/hardware_esp32.md §12.4).
 *
 * What a board wires is data in its own targets/<board>/esp32_board.h, found on the board's include
 * path: which pin enables an RF switch and which selects the antenna, at what level, and the user LED.
 * A board without one of them leaves its macro undefined. */
#ifndef ESP32_GPIO_H
#define ESP32_GPIO_H

#include <stdint.h>

/* Make `pin` a software-driven output at `level` (0/1): the level first, then the driver, so the pin
 * never shows the other level. 0 = no such pin on this chip. */
int Esp32Gpio_Output(uint32_t pin, int level);

/* Drive an output pin to `level`. */
void Esp32Gpio_Set(uint32_t pin, int level);

/* Bring the board's pins up: the RF switch and the antenna it selects, and the LED off. */
void Esp32Board_PinsInit(void);

/* The board's user LED on (1) or off (0); nothing on a board without one. */
void Esp32Board_Led(int on);

#endif /* ESP32_GPIO_H */
