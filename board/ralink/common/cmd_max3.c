// SPDX-License-Identifier: GPL-2.0+
/*
 * MAX3 (CPE MAX@3) MT7621AT + 512MiB NAND 专用命令集
 *
 * 设计边界（三条铁律，任何代码路径不得越界）：
 *   1) 绝不触碰 mtd0 的块 0（0x00000-0x1FFFF）：那是原厂 SPL，改坏即变砖；
 *   2) U-Boot 自身只占 0x20000-0x5FFFF（2 个 128KiB 擦除块）；
 *   3) 环境变量放 0x60000（主）/ 0x80000（冗余），都在 mtd0 第 3/4 块内。
 *
 * 提供的命令：
 *   max3autoboot       启动入口（替代原厂 mtkautoboot），按键窗口 + 双槽回退 + 全挂进网页救援
 *   max3slot  a|b|auto 设置启动槽并 saveenv
 *   max3boot  [a|b|auto]
 *                      按槽启动（不 saveenv），调试用
 *   max3rescue         网口救援：循环 TFTP 取 max3.cmd 执行（免拆机改槽/刷 boot/刷 U-Boot）
 *   max3uboot          从内存里已 TFTP 好镜像自升级 U-Boot（只写 0x20000 起 2 块）
 *   max3dload          以 upgrade_mode=dload 启动 b 槽（原厂 dload 内核，实验性）
 */

#include <common.h>
#include <command.h>
#include <environment.h>
#include <image.h>
#include <asm-generic/gpio.h>
#include <stdarg.h>

/* ---------------- 物理布局 ---------------- */
#define M3_NAND_ERASE		0x20000UL
#define M3_UBOOT_OFF		0x20000UL
#define M3_UBOOT_LEN		0x40000UL	/* 2 块 = 256KiB 上限 */
#define M3_SLOT_A_OFF		0x100000UL	/* mtd1 boot   */
#define M3_SLOT_B_OFF		0xB00000UL	/* mtd2 bootbk */
#define M3_SLOT_MAX		0xA00000UL	/* 单槽 10MiB */

/* ---------------- 内存窗口（DDR 0x80000000 + 256MiB） ---------------- */
#define M3_CMD_ADDR		0x83800000UL	/* max3.cmd 落点，64KiB 足够 */
#define M3_CMD_LIMIT		0x10000UL
#define M3_LOAD_ADDR		0x84000000UL	/* 镜像/Bootm 落点 */
#define M3_VFY_ADDR		0x86800000UL	/* 回读校验落点 */

/* ---------------- 按键窗口 ---------------- */
#define M3_KEY_STEP_MS		100
#define M3_KEY_WEB_STEPS	10		/* >=1.0s 松手 -> 网页救援 */
#define M3_KEY_TFTP_STEPS	30		/* >=3.0s 仍按 -> 网口救援 */

#define M3_CMD_MAGIC		"MAX3CMD1"

/* 只在当前板上生效的产物；换板子必须改这里 */
#ifndef M3_ENABLE
#define M3_ENABLE 1
#endif

static int m3_run(const char *fmt, ...)
{
	char buf[256];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	printf("MAX3: %s\n", buf);
	return run_command(buf, 0);
}

static int m3_key_down(void)
{
#ifdef MT7621_BUTTON_RESET
	return gpio_get_value(MT7621_BUTTON_RESET) == 0;
#else
	return 0;
#endif
}

/* 在串里取 key=value 的 value，成功返回 0 */
static int m3_field(const char *buf, const char *key, char *out, int len)
{
	const char *p = buf;
	int klen = strlen(key);

	while ((p = strstr(p, key))) {
		/* 要求 key 前面是行首或空白，避免误命中 */
		if (p != buf && p[-1] != '\n' && p[-1] != ' ' && p[-1] != '\t') {
			p += klen;
			continue;
		}
		p += klen;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '=') {
			int n = 0;
			p++;
			while (*p && *p != '\n' && *p != '\r' && *p != ' '
			       && *p != '\t' && n < len - 1)
				out[n++] = *p++;
			out[n] = 0;
			return 0;
		}
	}
	return -1;
}

/*
 * 把 M3_LOAD_ADDR 起 size 字节写到 NAND off：
 *   补 0xFF 到页边界 -> 擦 erase_len -> 写 -> 回读逐字节比对
 * 补 0xFF 是必须的：nand write 的末尾不成页会被底层拒或写出脏数据。
 */
static int m3_prog(ulong off, ulong erase_len, ulong size, const char *what)
{
	char cmd[192];
	ulong wlen = (size + 0x7FF) & ~0x7FFUL;
	int ret;

	memset((void *)(M3_LOAD_ADDR + size), 0xFF, wlen - size);

	printf("MAX3: 写 %s @0x%08lx，有效 0x%lx，写到 0x%lx，擦 0x%lx\n",
	       what, off, size, wlen, erase_len);

	snprintf(cmd, sizeof(cmd), "nand erase 0x%lx 0x%lx", off, erase_len);
	ret = run_command(cmd, 0);
	if (ret) {
		printf("MAX3: 擦除失败 %d\n", ret);
		return ret;
	}

	snprintf(cmd, sizeof(cmd), "nand write 0x%lx 0x%lx 0x%lx",
		 (ulong)M3_LOAD_ADDR, off, wlen);
	ret = run_command(cmd, 0);
	if (ret) {
		printf("MAX3: 写入失败 %d\n", ret);
		return ret;
	}

	snprintf(cmd, sizeof(cmd), "nand read 0x%lx 0x%lx 0x%lx",
		 (ulong)M3_VFY_ADDR, off, wlen);
	ret = run_command(cmd, 0);
	if (ret) {
		printf("MAX3: 回读失败 %d\n", ret);
		return ret;
	}

	if (memcmp((void *)M3_LOAD_ADDR, (void *)M3_VFY_ADDR, wlen)) {
		printf("MAX3: 回读校验不一致，%s 不可信\n", what);
		return -1;
	}

	printf("MAX3: %s 写入并校验通过\n", what);
	return 0;
}

/* 从 M3_LOAD_ADDR 已落好的镜像烧到某个槽 */
static int m3_flash_slot(char slot)
{
	ulong off = (slot == 'b') ? M3_SLOT_B_OFF : M3_SLOT_A_OFF;
	ulong size = env_get_ulong("filesize", 16, 0);
	ulong erase_len;
	char what[16];

	if (!size) {
		printf("MAX3: filesize 未知，先 tftpboot 镜像\n");
		return -1;
	}
	if (size > M3_SLOT_MAX - M3_NAND_ERASE) {
		printf("MAX3: 镜像 %lu 字节超过单槽容量\n", size);
		return -1;
	}
	if (image_get_magic((const image_header_t *)M3_LOAD_ADDR) != IH_MAGIC) {
		printf("MAX3: 镜像没有 uImage/FIT 头，拒绝写入\n");
		return -1;
	}

	erase_len = (size + M3_NAND_ERASE - 1) & ~(M3_NAND_ERASE - 1);
	snprintf(what, sizeof(what), "槽 %c", slot);

	return m3_prog(off, erase_len, size, what);
}

/* 自升级 U-Boot：只写 0x20000 起 2 块，含头部合法性与 load 地址校验 */
static int m3_flash_uboot(void)
{
	const image_header_t *hdr = (const image_header_t *)M3_LOAD_ADDR;
	ulong size = env_get_ulong("filesize", 16, 0);
	ulong len;

	if (!size) {
		printf("MAX3: filesize 未知，先 tftpboot u-boot-lzma.img\n");
		return -1;
	}
	if (size > M3_UBOOT_LEN) {
		printf("MAX3: U-Boot 载荷 %lu 字节 > 上限 %lu，拒绝\n",
		       size, (ulong)M3_UBOOT_LEN);
		return -1;
	}
	if (image_get_magic(hdr) != IH_MAGIC) {
		printf("MAX3: 缺 uImage 头\n");
		return -1;
	}
	if (image_get_comp(hdr) != IH_COMP_LZMA) {
		printf("MAX3: 不是 LZMA 载荷（SPL 只认 LZMA）\n");
		return -1;
	}
	if (image_get_load(hdr) != CONFIG_SYS_TEXT_BASE) {
		printf("MAX3: load 地址 0x%08x != 0x%08x\n",
		       image_get_load(hdr), (unsigned)CONFIG_SYS_TEXT_BASE);
		return -1;
	}

	/* data 长度向上按 NAND 页对齐，供日志核对 */
	len = (image_get_data_size(hdr) + 0x7FF) & ~0x7FFUL;

	printf("MAX3: 升级 U-Boot，data 长度 0x%lx，落到 0x%lx 起 0x%lx\n",
	       len, (ulong)M3_UBOOT_OFF, (ulong)M3_UBOOT_LEN);

	if (m3_prog((ulong)M3_UBOOT_OFF, (ulong)M3_UBOOT_LEN, size, "U-Boot"))
		return -1;

	printf("MAX3: U-Boot 已更新（%lu 字节），重启后由 SPL 从 0x%lx 加载\n",
	       size, (ulong)M3_UBOOT_OFF);
	return 0;
}

static int m3_boot_slot(char slot)
{
	ulong off = (slot == 'b') ? M3_SLOT_B_OFF : M3_SLOT_A_OFF;
	int ret;

	printf("MAX3: 从槽 %c 启动（NAND 0x%08lx）\n", slot, off);

	ret = m3_run("nand read 0x%lx 0x%lx 0x%lx", (ulong)M3_LOAD_ADDR,
		     off, (ulong)M3_SLOT_MAX);
	if (ret) {
		printf("MAX3: 槽 %c 读取失败 %d\n", slot, ret);
		return ret;
	}

	ret = m3_run("bootm 0x%lx", (ulong)M3_LOAD_ADDR);
	printf("MAX3: 槽 %c bootm 返回 %d\n", slot, ret);
	return ret ? ret : -1;
}

/* ---------------- 网口救援：循环取 max3.cmd ---------------- */

static void m3_httpd(void)
{
	printf("\nMAX3: 进入网页救援（http://%s/ ，上传镜像写 firmware 分区）\n",
	       env_get("ipaddr") ? env_get("ipaddr") : "192.168.1.1");
	run_command("httpd", 0);
}

static void m3_tftploop(void)
{
	char *buf = (char *)M3_CMD_ADDR;
	char op[32], slot[8], img[64];
	ulong fs;

	printf("\nMAX3: 进入网口救援。PC 侧 ip=%s，反复 TFTP 取 max3.cmd\n",
	       env_get("serverip") ? env_get("serverip") : "192.168.1.2");

	for (;;) {
		if (m3_run("tftpboot 0x%lx max3.cmd", (ulong)M3_CMD_ADDR)) {
			mdelay(1000);
			continue;
		}

		fs = env_get_ulong("filesize", 16, 0);
		if (!fs || fs > M3_CMD_LIMIT) {
			printf("MAX3: max3.cmd 尺寸异常 %lu\n", fs);
			mdelay(1000);
			continue;
		}
		buf[fs] = 0;

		if (strncmp(buf, M3_CMD_MAGIC, strlen(M3_CMD_MAGIC))) {
			printf("MAX3: 缺 %s 头，忽略\n", M3_CMD_MAGIC);
			mdelay(1000);
			continue;
		}

		if (m3_field(buf, "op", op, sizeof(op)) ||
		    m3_field(buf, "slot", slot, sizeof(slot)) ||
		    m3_field(buf, "img", img, sizeof(img))) {
			printf("MAX3: 命令不完整（需 op= slot= img=），忽略\n");
			mdelay(1000);
			continue;
		}

		printf("MAX3: 收到命令 op=%s slot=%s img=%s\n", op, slot, img);

		if (!strcmp(op, "slot")) {
			if (strcmp(slot, "a") && strcmp(slot, "b")
			    && strcmp(slot, "auto")) {
				printf("MAX3: slot 只能是 a/b/auto\n");
			} else {
				env_set("boot_slot", slot);
				if (m3_run("saveenv"))
					printf("MAX3: saveenv 失败\n");
				else
					printf("MAX3: boot_slot=%s 已保存\n", slot);
			}
			continue;
		}

		if (!strcmp(op, "reset")) {
			printf("MAX3: 收到复位命令\n");
			mdelay(500);
			do_reset(NULL, 0, 0, NULL);
			continue;
		}

		if (!strcmp(op, "httpd")) {
			m3_httpd();
			continue;
		}

		if (!strcmp(op, "shell")) {
			printf("MAX3: 退出救援，回到命令行\n");
			return;
		}

		if (!strcmp(op, "fit") || !strcmp(op, "uboot")) {
			if (!img[0]) {
				printf("MAX3: 缺 img=\n");
				continue;
			}
			if (m3_run("tftpboot 0x%lx %s", (ulong)M3_LOAD_ADDR, img)) {
				printf("MAX3: 取 %s 失败\n", img);
				continue;
			}
			if (!strcmp(op, "uboot")) {
				if (!m3_flash_uboot()) {
					printf("MAX3: U-Boot 已更新，重启生效\n");
					mdelay(500);
					do_reset(NULL, 0, 0, NULL);
				}
			} else if (strcmp(slot, "a") && strcmp(slot, "b")) {
				printf("MAX3: 刷槽需要 slot=a|b\n");
			} else if (!m3_flash_slot(slot[0])) {
				env_set("boot_slot", slot);
				if (m3_run("saveenv"))
					printf("MAX3: saveenv 失败\n");
				else
					printf("MAX3: 槽 %s 已就绪并设为默认\n", slot);
			}
			continue;
		}

		printf("MAX3: 未知 op=%s\n", op);
		mdelay(1000);
	}
}

/* ---------------- 启动入口 ---------------- */

static int m3_rescue_entry(int steps)
{
	if (steps >= M3_KEY_TFTP_STEPS) {
		m3_tftploop();
		return 1;
	}
	if (steps >= M3_KEY_WEB_STEPS) {
		m3_httpd();
		return 1;
	}
	return 0;
}

static int do_max3autoboot(cmd_tbl_t *cmdtp, int flag, int argc,
			   char *const argv[])
{
	const char *slot;
	int i, ret = -1;

	/* 默认值落盘一次：boot_slot=a，webwait 只是提示位 */
	if (!env_get("boot_slot"))
		env_set("boot_slot", "a");

	/* 按键窗口：1.0s 判短按（网页），3.0s 判长按（网口） */
	for (i = 0; i < M3_KEY_TFTP_STEPS; i++) {
		if (!m3_key_down())
			break;
		mdelay(M3_KEY_STEP_MS);
	}
	if (i) {
		if (i >= M3_KEY_WEB_STEPS)
			printf("MAX3: 检测到 reset 按住 %d00ms\n", i);
		if (m3_rescue_entry(i))
			return 0;
	}

	slot = env_get("boot_slot");
	if (!slot || !strcmp(slot, "auto")) {
		ret = m3_boot_slot('a');
		printf("MAX3: a 槽未起来，回退 b 槽\n");
		ret = m3_boot_slot('b');
	} else if (!strcmp(slot, "b")) {
		ret = m3_boot_slot('b');
		if (ret) {
			printf("MAX3: b 槽未起来，回退 a 槽\n");
			ret = m3_boot_slot('a');
		}
	} else {
		ret = m3_boot_slot('a');
		if (ret) {
			printf("MAX3: a 槽未起来，回退 b 槽\n");
			ret = m3_boot_slot('b');
		}
	}

	printf("MAX3: 两槽都没能启动（最后一次 %d），自动进网页救援\n", ret);
	m3_httpd();
	return 0;
}

static int do_max3slot(cmd_tbl_t *cmdtp, int flag, int argc,
		       char *const argv[])
{
	const char *v;

	if (argc < 2)
		return CMD_RET_USAGE;

	v = argv[1];
	if (strcmp(v, "a") && strcmp(v, "b") && strcmp(v, "auto"))
		return CMD_RET_USAGE;

	env_set("boot_slot", v);
	if (m3_run("saveenv"))
		printf("MAX3: saveenv 失败（环境变量可能没烧进 0x60000）\n");

	printf("MAX3: boot_slot=%s\n", env_get("boot_slot"));
	return 0;
}

static int do_max3boot(cmd_tbl_t *cmdtp, int flag, int argc,
		       char *const argv[])
{
	const char *v = (argc > 1) ? argv[1] : env_get("boot_slot");

	if (!v)
		v = "a";
	if (!strcmp(v, "auto"))
		v = "a";
	if (strcmp(v, "a") && strcmp(v, "b"))
		return CMD_RET_USAGE;

	return m3_boot_slot(v[0]) ? CMD_RET_FAILURE : 0;
}

static int do_max3rescue(cmd_tbl_t *cmdtp, int flag, int argc,
			 char *const argv[])
{
	m3_tftploop();
	return 0;
}

static int do_max3uboot(cmd_tbl_t *cmdtp, int flag, int argc,
			char *const argv[])
{
	return m3_flash_uboot() ? CMD_RET_FAILURE : 0;
}

static int do_max3dload(cmd_tbl_t *cmdtp, int flag, int argc,
			char *const argv[])
{
	const char *ba = env_get("bootargs");

	if (!ba || !strstr(ba, "upgrade_mode=dload")) {
		char nb[256];

		snprintf(nb, sizeof(nb), "%s upgrade_mode=dload",
			 ba ? ba : "");
		env_set("bootargs", nb);
		printf("MAX3: bootargs=%s\n", env_get("bootargs"));
	}

	return m3_boot_slot('b') ? CMD_RET_FAILURE : 0;
}

U_BOOT_CMD(max3autoboot, 1, 0, do_max3autoboot,
	   "MAX3 dual-slot autoboot (key window / A-B fallback / web rescue)",
	   "");

U_BOOT_CMD(max3slot, 2, 0, do_max3slot,
	   "set boot slot and save env",
	   "a|b|auto");

U_BOOT_CMD(max3boot, 2, 0, do_max3boot,
	   "boot from slot without saving env",
   "[a|b|auto]");

U_BOOT_CMD(max3rescue, 1, 0, do_max3rescue,
	   "loop TFTP fetch of max3.cmd (slot switch / flash boot / flash uboot)",
	   "");

U_BOOT_CMD(max3uboot, 1, 0, do_max3uboot,
	   "upgrade U-Boot payload from 0x84000000 (writes 0x20000 +0x40000 only)",
	   "");

U_BOOT_CMD(max3dload, 1, 0, do_max3dload,
	   "boot slot b with upgrade_mode=dload (experimental)",
	   "");
