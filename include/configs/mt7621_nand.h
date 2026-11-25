/* SPDX-License-Identifier:	GPL-2.0+ */
/*
 * Copyright (C) 2018 MediaTek Incorporation. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include "mt7621-common.h"

/* SPL */
#define CONFIG_SPL_MAX_SIZE		0x20000
#define CONFIG_SYS_UBOOT_START		CONFIG_SYS_TEXT_BASE
#define CONFIG_SYS_NAND_U_BOOT_OFFS	CONFIG_SPL_MAX_SIZE

/* Serial Port */
#if defined(CONFIG_SPL_BUILD) && defined(CONFIG_SPL_SERIAL_SUPPORT)
#define CONFIG_SYS_NS16550_COM1		0xbe000c00
#endif

/* NAND */
#define CONFIG_SYS_MAX_NAND_DEVICE	1
#define CONFIG_SYS_NAND_MAX_CHIPS	1
#define CONFIG_SYS_NAND_ONFI_DETECTION
#define CONFIG_SPL_NAND_IDENT
#define CONFIG_SPL_NAND_BASE

#ifdef CONFIG_ENABLE_NAND_NMBM
#define CONFIG_SPL_NAND_INIT
#define CONFIG_SPL_NAND_DRIVERS
#endif

/*
 * MAX3 可写环境变量
 * 本树是 2018.09 口径：冗余环境是 #define 而不是 Kconfig 符号
 * （env/Kconfig 里没有 ENV_OFFSET_REDUND / SYS_REDUNDAND_ENVIRONMENT），
 * 所以只能在这里定义；CONFIG_ENV_OFFSET / CONFIG_ENV_SIZE 走 defconfig。
 *
 * 主副本 0x60000（mtd0 第 3 块），冗余 0x80000（第 5 块），
 * 都在 U-Boot 自身 0x20000-0x5FFFF 之外，自升级不会互相踩。
 */
#define CONFIG_SYS_REDUNDAND_ENVIRONMENT
#define CONFIG_ENV_OFFSET_REDUND	0x80000

/* Network */
#define CONFIG_IPADDR			192.168.1.1
#define CONFIG_SERVERIP			192.168.1.2
#define CONFIG_NETMASK			255.255.255.0

#endif  /* __CONFIG_H */
