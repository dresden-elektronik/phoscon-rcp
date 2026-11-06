/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* The RaspBee II board descriptor accessor (board.h). The instance itself is
 * targets/rb2/board.h. */
#include "board.h"

const Board_Descriptor *Board_Get(void)
{
    return &boardRb2;
}
