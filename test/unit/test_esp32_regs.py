#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_esp32_regs.py - the ESP32 register transcription against ESP-IDF's own headers.

src/hal/esp32/esp32_regs.h (the family's layouts) and src/hal/esp32/<chip>/esp32_chip.h (the chip's
numbers) are this tree's transcription (docs/hardware_esp32.md §12). A wrong offset names another register
and a wrong bit writes a neighbouring field, and neither shows in an arithmetic suite, so every value is
compared with the same fact in ESP-IDF's components/soc/<chip>: block bases from reg_base.h, offsets as a
register's address less its block's base, fields as ESP-IDF's masks and shifts, interrupt sources as the
interrupt matrix's map-register offsets divided by four. Every ESP32_* name is either compared or listed
in NOT_COMPARED with where its value comes from instead; a canary with one wrong value proves the gate can
fail.

ESP-IDF is reference material outside the tree: the suite reads it from ESP_IDF_PATH, or from refs/
esp-idf-v5.4.1 where a maintainer's checkout keeps it, and is skipped (exit 77) without it. Run by ctest as
`esp32_regs`.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKIP = 77
CHIP = "esp32c6"
OURS = [os.path.join(REPO, "src", "hal", "esp32", "esp32_regs.h"),
        os.path.join(REPO, "src", "hal", "esp32", "c6", "esp32_chip.h")]

# Where the remaining names come from, and why ESP-IDF's headers do not say.
NOT_COMPARED = {
    "ESP32_REGS_H": "include guard",
    "ESP32_CHIP_H": "include guard",
    "ESP32_REG": "the accessor",
    "ESP32_CHIP_NAME": "a label",
    "ESP32_CHIP_ID": "esptool's IMAGE_CHIP_ID for the part; the guard and the bench read agree",
    "ESP32_XTAL_HZ": "TRM 13.3 and esptool's crystal read on the bench board",
    "ESP32_SYSTIMER_TICKS_PER_US": "TRM 13.3: XTAL_CLK / 2.5 = 16 MHz",
    "ESP32_PLIC_THRESH_ENABLED": "this tree's choice of threshold (TRM 1.6.3.2)",
    "ESP32_CPU_HZ_MAX": "the datasheet's 160 MHz, read back on the bench board",
    "ESP32_PCR_CPU_HS_DIV_MAX_SPEED": "ESP-IDF clk_tree_ll.h's divider rule: CPU_HS_DIV_NUM = divider / 3 - 1",
}

# ours -> an expression over ESP-IDF's names (evaluated with REG_TIMG_BASE(i) at i = 0).
COMPARE = {
    "ESP32_PLIC_BASE": "DR_REG_PLIC_MX_BASE",
    "ESP32_TIMG0_BASE": "DR_REG_TIMERGROUP0_BASE",
    "ESP32_SYSTIMER_BASE": "DR_REG_SYSTIMER_BASE",
    "ESP32_USJ_BASE": "DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_INTMTX_BASE": "DR_REG_INTERRUPT_MATRIX_BASE",
    "ESP32_LP_CLKRST_BASE": "DR_REG_LP_CLKRST_BASE",
    "ESP32_EFUSE_BASE": "DR_REG_EFUSE_BASE",
    "ESP32_LP_AON_BASE": "DR_REG_LP_AON_BASE",
    "ESP32_LP_WDT_BASE": "DR_REG_LP_WDT_BASE",
    "ESP32_SRC_ZB_MAC": "(INTMTX_CORE0_ZB_MAC_INTR_MAP_REG - DR_REG_INTERRUPT_MATRIX_BASE) // 4",
    "ESP32_SRC_USJ": "(INTMTX_CORE0_USB_INTR_MAP_REG - DR_REG_INTERRUPT_MATRIX_BASE) // 4",
    "ESP32_SRC_SYSTIMER_TARGET0": "(INTMTX_CORE0_SYSTIMER_TARGET0_INTR_MAP_REG - DR_REG_INTERRUPT_MATRIX_BASE) // 4",
    "ESP32_EFUSE_MAC0": "EFUSE_RD_MAC_SPI_SYS_0_REG - DR_REG_EFUSE_BASE",
    "ESP32_EFUSE_MAC1": "EFUSE_RD_MAC_SPI_SYS_1_REG - DR_REG_EFUSE_BASE",
    "ESP32_LP_CLKRST_RESET_CAUSE": "LP_CLKRST_RESET_CAUSE_REG - DR_REG_LP_CLKRST_BASE",
    "ESP32_LP_AON_SYS_CFG": "LP_AON_SYS_CFG_REG - DR_REG_LP_AON_BASE",
    "ESP32_LP_AON_HPSYS_SW_RESET": "LP_AON_HPSYS_SW_RESET",
    "ESP32_SRAM_LOW": "SOC_DRAM_LOW",
    "ESP32_APP_RAM_END": "BOOTLOADER_IRAM_LOADER_SEG_START",
    "ESP32_WDT_WKEY": "LP_WDT_WKEY_VALUE",
    "ESP32_SYSTIMER_CONF": "SYSTIMER_CONF_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_CONF_TARGET0_WORK_EN": "SYSTIMER_TARGET0_WORK_EN",
    "ESP32_SYSTIMER_CONF_UNIT0_CORE0_STALL_EN": "SYSTIMER_TIMER_UNIT0_CORE0_STALL_EN",
    "ESP32_SYSTIMER_CONF_UNIT0_WORK_EN": "SYSTIMER_TIMER_UNIT0_WORK_EN",
    "ESP32_SYSTIMER_CONF_CLK_EN": "SYSTIMER_CLK_EN",
    "ESP32_SYSTIMER_UNIT0_OP": "SYSTIMER_UNIT0_OP_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_UNIT0_OP_VALUE_VALID": "SYSTIMER_TIMER_UNIT0_VALUE_VALID",
    "ESP32_SYSTIMER_UNIT0_OP_UPDATE": "SYSTIMER_TIMER_UNIT0_UPDATE",
    "ESP32_SYSTIMER_TARGET0_HI": "SYSTIMER_TARGET0_HI_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_TARGET0_LO": "SYSTIMER_TARGET0_LO_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_TARGET0_CONF": "SYSTIMER_TARGET0_CONF_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MASK": "SYSTIMER_TARGET0_PERIOD_V",
    "ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MODE": "SYSTIMER_TARGET0_PERIOD_MODE",
    "ESP32_SYSTIMER_TARGET0_CONF_UNIT1": "SYSTIMER_TARGET0_TIMER_UNIT_SEL",
    "ESP32_SYSTIMER_UNIT0_VALUE_HI": "SYSTIMER_UNIT0_VALUE_HI_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_UNIT0_VALUE_LO": "SYSTIMER_UNIT0_VALUE_LO_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_COMP0_LOAD": "SYSTIMER_COMP0_LOAD_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_COMP0_LOAD_GO": "SYSTIMER_TIMER_COMP0_LOAD",
    "ESP32_SYSTIMER_INT_ENA": "SYSTIMER_INT_ENA_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_INT_CLR": "SYSTIMER_INT_CLR_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_INT_ST": "SYSTIMER_INT_ST_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_INT_TARGET0": "SYSTIMER_TARGET0_INT_ENA",
    "ESP32_TIMG_WDTCONFIG0": "TIMG_WDTCONFIG0_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_TIMG_WDTCONFIG0_FLASHBOOT_MOD_EN": "TIMG_WDT_FLASHBOOT_MOD_EN",
    "ESP32_TIMG_WDTCONFIG0_SYS_RESET_LENGTH_SHIFT": "TIMG_WDT_SYS_RESET_LENGTH_S",
    "ESP32_TIMG_WDTCONFIG0_CPU_RESET_LENGTH_SHIFT": "TIMG_WDT_CPU_RESET_LENGTH_S",
    "ESP32_TIMG_WDTCONFIG0_CONF_UPDATE_EN": "TIMG_WDT_CONF_UPDATE_EN",
    "ESP32_TIMG_WDTCONFIG0_STG0_SHIFT": "TIMG_WDT_STG0_S",
    "ESP32_TIMG_WDTCONFIG0_EN": "TIMG_WDT_EN",
    "ESP32_TIMG_WDTCONFIG1": "TIMG_WDTCONFIG1_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_TIMG_WDTCONFIG1_DIVCNT_RST": "TIMG_WDT_DIVCNT_RST",
    "ESP32_TIMG_WDTCONFIG1_CLK_PRESCALE_SHIFT": "TIMG_WDT_CLK_PRESCALE_S",
    "ESP32_TIMG_WDTCONFIG2": "TIMG_WDTCONFIG2_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_TIMG_WDTFEED": "TIMG_WDTFEED_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_TIMG_WDTWPROTECT": "TIMG_WDTWPROTECT_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_WDT_STAGE_OFF": "WDT_STAGE_ACTION_OFF",
    "ESP32_WDT_STAGE_RESET_SYSTEM": "WDT_STAGE_ACTION_RESET_SYSTEM",
    "ESP32_LP_WDT_CONFIG0": "LP_WDT_CONFIG0_REG - DR_REG_LP_WDT_BASE",
    "ESP32_LP_WDT_CONFIG0_FLASHBOOT_MOD_EN": "LP_WDT_WDT_FLASHBOOT_MOD_EN",
    "ESP32_LP_WDT_CONFIG0_EN": "LP_WDT_WDT_EN",
    "ESP32_LP_WDT_FEED": "LP_WDT_FEED_REG - DR_REG_LP_WDT_BASE",
    "ESP32_LP_WDT_WPROTECT": "LP_WDT_WPROTECT_REG - DR_REG_LP_WDT_BASE",
    "ESP32_LP_WDT_SWD_CONFIG": "LP_WDT_SWD_CONFIG_REG - DR_REG_LP_WDT_BASE",
    "ESP32_LP_WDT_SWD_CONFIG_AUTO_FEED_EN": "LP_WDT_SWD_AUTO_FEED_EN",
    "ESP32_LP_WDT_SWD_WPROTECT": "LP_WDT_SWD_WPROTECT_REG - DR_REG_LP_WDT_BASE",
    "ESP32_USJ_EP1": "USB_SERIAL_JTAG_EP1_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_EP1_CONF": "USB_SERIAL_JTAG_EP1_CONF_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_EP1_CONF_WR_DONE": "USB_SERIAL_JTAG_WR_DONE",
    "ESP32_USJ_EP1_CONF_IN_EP_DATA_FREE": "USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE",
    "ESP32_USJ_EP1_CONF_OUT_EP_DATA_AVAIL": "USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL",
    "ESP32_USJ_INT_RAW": "USB_SERIAL_JTAG_INT_RAW_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_INT_ST": "USB_SERIAL_JTAG_INT_ST_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_INT_ENA": "USB_SERIAL_JTAG_INT_ENA_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_INT_CLR": "USB_SERIAL_JTAG_INT_CLR_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_INT_OUT_RECV_PKT": "USB_SERIAL_JTAG_SERIAL_OUT_RECV_PKT_INT_ENA",
    "ESP32_USJ_INT_IN_EMPTY": "USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT_ENA",
    "ESP32_USJ_FRAM_NUM": "USB_SERIAL_JTAG_FRAM_NUM_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_IO_MUX_BASE": "DR_REG_IO_MUX_BASE",
    "ESP32_GPIO_BASE": "DR_REG_GPIO_BASE",
    "ESP32_PCR_BASE": "DR_REG_PCR_BASE",
    "ESP32_GPIO_PINS": "SOC_GPIO_PIN_COUNT",
    "ESP32_IO_MUX_GPIO0": "IO_MUX_GPIO0_REG - DR_REG_IO_MUX_BASE",
    "ESP32_PLL_HZ": "CLK_LL_PLL_480M_FREQ_MHZ * 1000000",
    "ESP32_PCR_SYSCLK_CONF": "PCR_SYSCLK_CONF_REG - DR_REG_PCR_BASE",
    "ESP32_PCR_SYSCLK_LS_DIV_SHIFT": "PCR_LS_DIV_NUM_S",
    "ESP32_PCR_SYSCLK_HS_DIV_SHIFT": "PCR_HS_DIV_NUM_S",
    "ESP32_PCR_SYSCLK_SOC_CLK_SEL_SHIFT": "PCR_SOC_CLK_SEL_S",
    "ESP32_PCR_SYSCLK_SOC_CLK_PLL": "SOC_CPU_CLK_SRC_PLL",
    "ESP32_PCR_CPU_FREQ_CONF": "PCR_CPU_FREQ_CONF_REG - DR_REG_PCR_BASE",
    "ESP32_PCR_CPU_LS_DIV_SHIFT": "PCR_CPU_LS_DIV_NUM_S",
    "ESP32_PCR_CPU_HS_DIV_SHIFT": "PCR_CPU_HS_DIV_NUM_S",
    "ESP32_PCR_CPU_HS_120M_FORCE": "PCR_CPU_HS_120M_FORCE",
    "ESP32_PCR_CPU_HS_DIV_MASK": "PCR_CPU_HS_DIV_NUM_M",
    "ESP32_PCR_AHB_FREQ_CONF": "PCR_AHB_FREQ_CONF_REG - DR_REG_PCR_BASE",
    "ESP32_GPIO_OUT": "GPIO_OUT_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_OUT_W1TS": "GPIO_OUT_W1TS_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_OUT_W1TC": "GPIO_OUT_W1TC_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_ENABLE": "GPIO_ENABLE_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_ENABLE_W1TS": "GPIO_ENABLE_W1TS_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_ENABLE_W1TC": "GPIO_ENABLE_W1TC_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_FUNC0_OUT_SEL_CFG": "GPIO_FUNC0_OUT_SEL_CFG_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_SIG_GPIO_OUT": "SIG_GPIO_OUT_IDX",
    "ESP32_IO_MUX_MCU_SEL_SHIFT": "MCU_SEL_S",
    "ESP32_IO_MUX_MCU_SEL_MASK": "MCU_SEL_V << MCU_SEL_S",
    "ESP32_IO_MUX_FUNC_GPIO": "PIN_FUNC_GPIO",
    "ESP32_IO_MUX_FUN_IE": "1 << FUN_IE_S",
    "ESP32_MODEM_SYSCON_BASE": "DR_REG_MODEM_SYSCON_BASE",
    "ESP32_MODEM_LPCON_BASE": "DR_REG_MODEM_LPCON_BASE",
    "ESP32_MODEM_SYSCON_CLK_CONF": "MODEM_SYSCON_CLK_CONF_REG - DR_REG_MODEM_SYSCON_BASE",
    "ESP32_MODEM_SYSCON_CLK_ETM_EN": "MODEM_SYSCON_CLK_ETM_EN",
    "ESP32_MODEM_SYSCON_CLK_ZB_APB_EN": "MODEM_SYSCON_CLK_ZB_APB_EN",
    "ESP32_MODEM_SYSCON_CLK_ZB_MAC_EN": "MODEM_SYSCON_CLK_ZB_MAC_EN",
    "ESP32_MODEM_SYSCON_CLK_CONF1": "MODEM_SYSCON_CLK_CONF1_REG - DR_REG_MODEM_SYSCON_BASE",
    "ESP32_MODEM_SYSCON_CLK_FE_80M_EN": "MODEM_SYSCON_CLK_FE_80M_EN",
    "ESP32_MODEM_SYSCON_CLK_FE_160M_EN": "MODEM_SYSCON_CLK_FE_160M_EN",
    "ESP32_MODEM_SYSCON_CLK_FE_CAL_160M_EN": "MODEM_SYSCON_CLK_FE_CAL_160M_EN",
    "ESP32_MODEM_SYSCON_CLK_FE_APB_EN": "MODEM_SYSCON_CLK_FE_APB_EN",
    "ESP32_MODEM_SYSCON_CLK_BT_APB_EN": "MODEM_SYSCON_CLK_BT_APB_EN",
    "ESP32_MODEM_SYSCON_CLK_BT_EN": "MODEM_SYSCON_CLK_BT_EN",
    "ESP32_MODEM_LPCON_CLK_CONF": "MODEM_LPCON_CLK_CONF_REG - DR_REG_MODEM_LPCON_BASE",
    "ESP32_MODEM_LPCON_CLK_COEX_EN": "MODEM_LPCON_CLK_COEX_EN",
    "ESP32_MODEM_LPCON_CLK_I2C_MST_EN": "MODEM_LPCON_CLK_I2C_MST_EN",
    "ESP32_PLIC_ENABLE": "PLIC_MXINT_ENABLE_REG - DR_REG_PLIC_MX_BASE",
    "ESP32_PLIC_TYPE": "PLIC_MXINT_TYPE_REG - DR_REG_PLIC_MX_BASE",
    "ESP32_PLIC_CLEAR": "PLIC_MXINT_CLEAR_REG - DR_REG_PLIC_MX_BASE",
    "ESP32_PLIC_PRI0": "PLIC_MXINT0_PRI_REG - DR_REG_PLIC_MX_BASE",
    "ESP32_PLIC_THRESH": "PLIC_MXINT_THRESH_REG - DR_REG_PLIC_MX_BASE",
}

DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)(\(\w+\))?[ \t]+(.+?)\s*(?:/\*.*|//.*)?$", re.M)


def idf_root():
    for root in (os.environ.get("ESP_IDF_PATH"), os.path.join(REPO, "refs", "esp-idf-v5.4.1")):
        if root and os.path.isdir(os.path.join(root, "components", "soc", CHIP)):
            return root
    return None


def collect(paths):
    objects, functions = {}, {}
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        for name, param, body in DEFINE.findall(text):
            if param:
                functions.setdefault(name, (param.strip("()"), body))
            else:
                objects.setdefault(name, body)
        for name, value in re.findall(r"^\s*(\w+)\s*=\s*(\d+)\s*,", text, re.M):
            objects.setdefault(name, value)
    return objects, functions


def evaluate(expr, objects, functions, depth=0):
    if depth > 40:
        raise ValueError(f"too deep: {expr}")
    expr = re.sub(r"\bBIT\((\d+)\)", r"(1 << \1)", expr)
    previous = None
    while previous != expr:                    # until no function-like macro is left, nested ones included
        previous = expr
        for name, (param, body) in functions.items():
            expr = re.sub(r"\b%s\((\w+)\)" % name,
                          lambda m, p=param, b=body: "(" + re.sub(r"\b%s\b" % p, m.group(1), b) + ")", expr)
    expr = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)[uUlL]+\b", r"\1", expr)

    def name_value(m):
        word = m.group(0)
        if word not in objects:
            raise KeyError(word)
        return "(" + str(evaluate(objects[word], objects, functions, depth + 1)) + ")"

    expr = re.sub(r"\b[A-Za-z_]\w*\b", name_value, expr)
    return int(eval(expr, {"__builtins__": {}}))


def compare(ours, idf, idf_fn, names):
    bad = []
    for name in names:
        try:
            want = evaluate(COMPARE[name], idf, idf_fn)
        except KeyError as e:
            bad.append(f"{name}: ESP-IDF has no {e}")
            continue
        got = ours[name]
        if got != want:
            bad.append(f"{name}: 0x{got:x}, ESP-IDF says 0x{want:x} ({COMPARE[name]})")
    return bad


def main():
    root = idf_root()
    if root is None:
        print("esp32_regs: no ESP-IDF checkout (ESP_IDF_PATH or refs/esp-idf-v5.4.1): skipped")
        return SKIP
    soc = os.path.join(root, "components", "soc", CHIP)
    # The first definition of a name wins: soc.h's REG_TIMG_BASE(i), not the struct-based one a register
    # header may define after it. The watchdog key is in the LP watchdog's low-level driver.
    idf_paths = [os.path.join(soc, "include", "soc", "soc.h"),
                 os.path.join(root, "components", "hal", "include", "hal", "wdt_types.h"),
                 os.path.join(root, "components", "hal", CHIP, "include", "hal", "lpwdt_ll.h"),
                 os.path.join(root, "components", "hal", CHIP, "include", "hal", "clk_tree_ll.h"),
                 os.path.join(soc, "include", "soc", "soc_caps.h"),
                 os.path.join(soc, "include", "soc", "clk_tree_defs.h"),
                 os.path.join(soc, "include", "soc", "gpio_sig_map.h")]
    idf_paths += [os.path.join(soc, "register", "soc", f) for f in sorted(os.listdir(os.path.join(soc, "register", "soc")))
                  if f.endswith("_reg.h") or f == "reg_base.h"]
    modem = os.path.join(soc, "include", "modem")
    idf_paths += [os.path.join(modem, f) for f in sorted(os.listdir(modem)) if f.endswith(".h")]
    idf, idf_fn = collect(idf_paths)
    ld_dir = os.path.join(root, "components", "bootloader", "subproject", "main", "ld", CHIP)
    ld = os.path.join(ld_dir, "bootloader.memory.ld.in")      # its name from v5.5 on
    if not os.path.exists(ld):
        ld = os.path.join(ld_dir, "bootloader.ld")
    with open(ld) as f:
        m = re.search(r"ASSERT\(bootloader_iram_loader_seg_start == (0x[0-9A-Fa-f]+)", f.read())
    idf["BOOTLOADER_IRAM_LOADER_SEG_START"] = m.group(1)

    our_defs, our_fn = collect(OURS)
    ours = {}
    unlisted = []
    for name in our_defs:
        if not name.startswith("ESP32_") or name in NOT_COMPARED:
            continue
        if name not in COMPARE:
            unlisted.append(name)
            continue
        ours[name] = evaluate(our_defs[name], our_defs, our_fn)

    bad = compare(ours, idf, idf_fn, sorted(ours))
    bad += [f"{name}: neither compared nor listed in NOT_COMPARED" for name in unlisted]
    bad += [f"{name}: compared but not defined by the headers" for name in COMPARE if name not in ours]

    canary = dict(ours)
    canary["ESP32_SYSTIMER_INT_CLR"] ^= 4
    if not compare(canary, idf, idf_fn, ["ESP32_SYSTIMER_INT_CLR"]):
        bad.append("canary: a wrong offset passed the comparison")

    for line in bad:
        print("* FAIL* " + line)
    print(f"esp32_regs: {len(ours)} values compared with ESP-IDF ({os.path.relpath(root, REPO)}), "
          f"{len(bad)} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
