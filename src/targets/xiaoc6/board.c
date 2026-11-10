/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* The XIAO ESP32-C6 board descriptor accessor (board.h). The instance itself is targets/xiaoc6/board.h. */
#include "board.h"

const Board_Descriptor *Board_Get(void)
{
    return &boardXiaoC6;
}
