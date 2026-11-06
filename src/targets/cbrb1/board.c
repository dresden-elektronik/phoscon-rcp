/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* The ConBee I / RaspBee I board descriptor accessor (board.h). The instance itself is
 * targets/cbrb1/board.h. */
#include "board.h"

const Board_Descriptor *Board_Get(void)
{
    return &boardCbrb1;
}
