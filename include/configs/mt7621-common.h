/* SPDX-License-Identifier:	GPL-2.0+ */
/*
 * Copyright (C) 2018 MediaTek Incorporation. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef __CONFIG_MT7621_COMMON_H
#define __CONFIG_MT7621_COMMON_H

#define CONFIG_SYS_HZ			1000
#define CONFIG_SYS_MIPS_TIMER_FREQ	880000000

#define CONFIG_SYS_MONITOR_BASE		CONFIG_SYS_TEXT_BASE

#define CONFIG_SYS_MALLOC_LEN		0x100000
#define CONFIG_SYS_BOOTPARAMS_LEN	0x20000

#define CONFIG_SYS_SDRAM_BASE		0x80000000

#if defined(CONFIG_MT7621_DRAM_DDR2_512M_W9751G6KB_A02_1066MHZ_LEGACY) || \
	defined(CONFIG_MT7621_DRAM_DDR2_512M_W9751G6KB_A02_1066MHZ) || \
	defined(CONFIG_MT7621_DRAM_DDR2_512M_LEGACY) ||\
	defined(CONFIG_MT7621_DRAM_DDR2_512M)
#define CONFIG_SYS_LOAD_ADDR	0x82000000
#else
#define CONFIG_SYS_LOAD_ADDR	0x84000000
#endif

#define CONFIG_VERY_BIG_RAM
#define CONFIG_MAX_MEM_MAPPED		0x1c000000

#define CONFIG_SYS_MIPS_CACHE_MODE	CONF_CM_CACHABLE_COW

#define CONFIG_SYS_BOOTM_LEN		0x2000000

/* SPL */
#define CONFIG_SPL_BSS_START_ADDR	0xbe108000
#define CONFIG_SPL_BSS_MAX_SIZE		0x2000

/* Serial Port */
#if defined(CONFIG_SPL_BUILD) && defined(CONFIG_SPL_SERIAL_SUPPORT)
#define CONFIG_SYS_NS16550_REG_SIZE	-4
#define CONFIG_CONS_INDEX		1
#define CONFIG_SYS_NS16550_CLK		50000000
#define CONFIG_SYS_NS16550_MEM32
#endif
#define CONFIG_SYS_BAUDRATE_TABLE	{ 9600, 19200, 38400, 57600, 115200, \
					  230400, 460800, 921600 }

/*
 * MAX3 板级按键 / 指示灯
 * reset 键实测在 gpio14（低有效：松手=1，按住=0），
 * 由 customize.sh 原逻辑写在这里，我们手工固化为 14。
 */
#ifndef __CONFIG_MT7621_RESET_LED
#define __CONFIG_MT7621_RESET_LED
#define MT7621_BUTTON_RESET		14
#endif

/*
 * MAX3 三颗指示灯，引脚与正式系统 DTB /gpio-leds 完全一致：
 *   STATUS1 = 11 -> sys_led:green   （DTB 节点 gpio-mode，default-on）
 *   STATUS2 =  3 -> lte_led:green   （DTB 节点 gpio-signal4，默认灭）
 *   STATUS3 =  4 -> nr_led:green    （DTB 节点 gpio-signal2，默认灭）
 *
 * 三颗全为 ACTIVE_HIGH（高电平亮），与正式系统实测一致：
 *   gpio-11 "out hi" 亮，gpio-3/4 "out lo" 灭。
 * 厂商自定义驱动 td-gpio-leds，不是标准 gpio-leds，故 U-Boot 里不走 DM LED
 * 框架（CONFIG_BOARD_MT7621_NAND_TEMPLATE 分支本身也会跳过 led_default_state），
 * 直接用裸 GPIO。
 *
 * pinmux 已就位，无需改 PAD_xxx_CFG：
 *   drivers/pinctrl/pinctrl-mt7621.c 中 pin3~4 属 "i2c" 组、pin11 属 "uart2" 组，
 *   模板 DTS 的 state_default 把 "i2c","uart2","uart3","pcie" 全部设为 gpio 功能。
 */
#define MT7621_LED_STATUS1		11	/* sys_led:green */
#define MT7621_LED_STATUS2		3	/* lte_led:green */
#define MT7621_LED_STATUS3		4	/* nr_led:green */
#define MT7621_LED_ACTIVE_HIGH		1	/* 1 = 高电平亮 */

/*
 * MBB（5G 模组 IC5980）共享内存：物理 0x00A00000，大小 0x1000。
 * +0x60 是硬件 ID，由原厂 U-Boot 写入；Linux 侧 bsp_get_hardware_id()
 * 直接 lw 0x60($v0) 取值，无任何兜底。不写则 g_product_id 为上电随机值。
 *
 * 非缓存别名按 MIPS KSEG1 口径取 0xA0000000 + phys = 0xA0A00000，
 * 与原厂反汇编 lui $v0,0xa0a0 / sw $a1,0x60($v0) 一致。
 * （早期文档里写的 0xB0A00000 是把 KSEG1 基址算错了，会落到物理 0x10A00000，
 *   已在本次修正。）
 */
#define MT7621_MBB_SMEM_PHYS		0x00A00000
#define MT7621_MBB_SMEM_CKSEG1		0xA0A00000
#define MT7621_MBB_HWID_OFF		0x60

#endif  /* __CONFIG_MT7621_COMMON_H */
