/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* The ConBee II board descriptor accessor (board.h). The instance itself is
 * targets/cb2/board.h. */
#include "board.h"

const Board_Descriptor *Board_Get(void)
{
    return &boardCb2;
}
