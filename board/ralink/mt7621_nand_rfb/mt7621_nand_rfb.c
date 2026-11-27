// SPDX-License-Identifier:	GPL-2.0+
/*
 * Copyright (C) 2018 MediaTek Incorporation. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#include <common.h>
#include <environment.h>
#include <image.h>
#include <led.h>
#include <linux/types.h>
#include <debug_uart.h>
#include <asm/spl.h>
#include <asm/io.h>
#include <asm-generic/gpio.h>
#include <nand.h>
#include <linux/mtd/mtd.h>
#include <mach/mt7621_regs.h>

#include <nmbm/nmbm.h>
#include <nmbm/nmbm-mtd.h>
#include <linux/string.h>
#include <malloc.h>

#ifdef CONFIG_ENABLE_NAND_NMBM
static int nmbm_usable;
#endif

static inline int gpio_output_init(unsigned gpio, int value, const char *label)
{
	int ret;

	ret = gpio_request(gpio, label);
	if (ret && ret != -EBUSY) {
		printf("gpio: requesting pin %u failed\n", gpio);
		return -1;
	}

	gpio_direction_output(gpio, value);

	return 0;
}

static inline int gpio_input_init(unsigned gpio, const char *label)
{
	int ret;

	ret = gpio_request(gpio, label);
	if (ret && ret != -EBUSY) {
		printf("gpio: requesting pin %u failed\n", gpio);
		return -1;
	}

	gpio_direction_input(gpio);

	return 0;
}

/* Set unused pins to gpio input mode */
static int pins_unused_init(const int *pinlist, unsigned listsize)
{
	int cnt;
	char label[10] = "";

	for (cnt = 0; cnt < listsize; cnt ++) {
		/*
		 * 原厂这里判的是下标 cnt 而不是 pinlist[cnt]，等于用错边界：
		 * 列表长度为 1 时永远通过，真给一个越界 pin 也拦不住。
		 * MT7621 可用 GPIO 是 0..#61（pin group 表最大到 esw int = 61）。
		 */
		if (pinlist[cnt] < 0 || pinlist[cnt] > 61) {
			printf("unsupported GPIO: gpio#%d", pinlist[cnt]);
			return -1;
		}

		strcpy(label, "gpio#");
		strcat(label, simple_itoa(pinlist[cnt]));
		gpio_input_init(pinlist[cnt], label);
	}

	return 0;
}

#ifdef MT7621_MBB_SMEM_PHYS
/*
 * MAX3：MBB（5G 模组 IC5980）共享内存初始化
 *
 * 原厂 U-Boot（mtd0 载荷 0x8021B208..0x8021B250）在进命令行前做两件事：
 *   1) 读 strap GPIO13/15/16，组合成硬件 ID：(v13<<2)|(v15<<1)|v16；
 *   2) 把该 ID 以 32 位写到 0xa0a00060（物理 0x00A00060 的非缓存视图）。
 *
 * Linux 侧 mbb_kernel_version.ko 的 bsp_get_hardware_id() 就是
 *   get_msmem_addr() -> lw $v0, 0x60($v0) -> jr $ra
 * 没有任何兜底。这块没写，g_product_id 就是上电随机值（现场实测 255），
 * mbb_dload.ko 拿它去 product_info_st 里查表匹配失败 -> init 内部报错 ->
 * 内核回滚该模块 -> mbb_nand / factory_at_interface 缺导出符号无法加载 ->
 * /dev/at_exec 不存在 -> 上层 libapcmdproc.so 全线 "file hander = -1"。
 *
 * 本函数只补最小集：+0x60 硬件 ID。其余字段（+0x48 状态字、各 flag）实测
 * 在残留垃圾值下三个模块也能依次 insmod 成功，不动以免误伤 dload /
 * 多机升级等原厂分支。仅在判定"整块从未被写过"（+0x60 高 24 位非 0）时，
 * 才按原厂冷启动语义把前 0x90 字节清零。
 *
 * 幂等：写完后 +0x60 高 24 位为 0，下次启动不会再清零。
 */
static void max3_mbb_smem_init(void)
{
	volatile u32 *smem = (volatile u32 *)MT7621_MBB_SMEM_CKSEG1;
	u32 cur, id;
	int b13, b15, b16, i;

	/*
	 * strap 三个脚要先 request 成输入再读，否则 gpio_get_value() 拿不到值。
	 * pin13/15/16 属 "jtag" 组，模板 DTS 已把 jtag 设为 gpio 功能。
	 */
	gpio_input_init(13, "mbb-id13");
	gpio_input_init(15, "mbb-id15");
	gpio_input_init(16, "mbb-id16");

	b13 = gpio_get_value(13) ? 1 : 0;
	b15 = gpio_get_value(15) ? 1 : 0;
	b16 = gpio_get_value(16) ? 1 : 0;
	id = (u32)((b13 << 2) | (b15 << 1) | b16);

	/* 产品表 product_info_st 只有 0x00(MT7621A) / 0x02 / 0x03 三项 */
	if (id != 0 && id != 2 && id != 3) {
		printf("MAX3: strap hw_id=%u 不在产品表(0/2/3)，按 0x00 处理\n", id);
		id = 0;
	}

	cur = smem[MT7621_MBB_HWID_OFF / 4];

	if (cur & 0xFFFFFF00UL) {
		printf("MAX3: 共享内存未初始化（+0x60=0x%08x），清零前 0x90 字节\n",
		       cur);
		for (i = 0; i < 0x90 / 4; i++)
			smem[i] = 0;
	}

	smem[MT7621_MBB_HWID_OFF / 4] = id;

	if (smem[MT7621_MBB_HWID_OFF / 4] != id) {
		printf("MAX3: 共享内存 +0x60 回读不一致！\n");
		return;
	}

	printf("MAX3: MBB 共享内存 +0x%02x = 0x%02x（strap 13/15/16=%d/%d/%d）\n",
	       MT7621_MBB_HWID_OFF, id, b13, b15, b16);
}
#endif

/*
 * 注意：mtk_board_get_flash_dev()/mtk_board_flash_read() 定义在下面的
 * #ifndef CONFIG_SPL_BUILD 里，SPL 阶段没有这两个符号，所以本块也必须
 * 排除 SPL 构建；另外 misc_init_r() 只有 U-Boot 主镜像会调用。
 */
#if defined(CONFIG_MISC_INIT_R) && !defined(CONFIG_SPL_BUILD)
/*
 * ---------- MAX3: 从 NV(mnv) 区读 SN / MAC，并固化到 U-Boot env ----------
 *
 * 记录格式（2026-11-26 用 mtd05/06/07 裸备份逐字节实测 + 全量重扫校准）：
 *
 *   [magic-0x1C .. magic-0x01]  28 B 记录头
 *       +0x00 u32 sig      = 5C B6 0A 02   <- 真记录的签名；payload 内部还会
 *                                            嵌一个假 "FDNM"，靠这个区分
 *       +0x04 u32 ver      = 0100 0100
 *       +0x0C u32 idx      槽号，同一条记录写多份就靠它区分
 *       +0x10 u32 id       = 47 (0x2F)  <- 设备身份记录的类型号
 *       +0x14 u32 size     = 228 (0xE4) <- payload 长度（旧代码误取成 47）
 *       +0x18 u32 seq
 *   [magic+0x00 .. magic+0x1F]  32 B 记录摘要（"FDNM" 4 字节 + 28 B 校验）
 *   [magic+0x20 .. magic+0xE3]  228 B payload：
 *       +0x00 4 x 12 位大写 ASCII MAC（连写、无分隔符）
 *             实测 D0A0D663247C / D0A0D663E7CC / D0A0D6619DDD / D0A0D662612D
 *             依次对应：无接口(预留) / eth0 / wlan0 / wlan1
 *       +0x30 32 B = sha256(payload[0x00..0x30])，三份 nv 全部吻合
 *       +0x50 32 B 全 0 保留
 *       +0x70 32 B 校验
 *       +0x90 4 B 标记 + 16 B ASCII SN（实测 fc 3c 8a 0e | CPE5G54121000467）
 *       +0xA4 4 B 0xFF 结束符
 *       +0xA8 32 B 校验
 *
 * 同一条记录在三个 nv 分区里会重复多份（默认镜像 2 份、备份 4 份、产测 3 份），
 * 且各分区取值完全一致 —— 这就是天然的多份互校，无需再校验摘要。
 *
 * 分区偏移来自 CONFIG_MTDPARTS_DEFAULT：
 *   uboot 1M / boot 10M / bootbk 10M / oeminfo 1536K / mnvdload 1M /
 *   mnvbackup 1M(0x1780000) / mnvdefault 1M(0x1880000) / mnvimg 10M(0x1980000)
 */
#define MAX3_OEMINFO_OFF	0x1500000UL
#define MAX3_MNV_BACKUP_OFF	0x1780000UL
#define MAX3_MNV_DEFAULT_OFF	0x1880000UL
#define MAX3_MNV_IMG_OFF	0x1980000UL
#define MAX3_MNV_SCAN_LEN	0x80000UL	/* 每处扫 512 KiB，覆盖上述命中点 */
#define MAX3_MNV_REC_HDR	0x1CU		/* magic 前的 28 B 结构头 */
#define MAX3_MNV_REC_CHECK	0x20U		/* magic 后的 32 B 摘要 */
#define MAX3_MNV_REC_ID		47U		/* 设备身份记录的类型号 */
#define MAX3_MNV_REC_SIZE	0xE4U		/* payload 长度，228 B */
#define MAX3_MAC_NUM		4		/* 一条记录带 4 个 MAC */
#define MAX3_MAC_GRP		4		/* 一个区里最多记 4 组不同候选 */
#define MAX3_SN_OFF_MIN		0x50U		/* payload 里 SN 滑窗起点 */

/*
 * U-Boot 自己走的是 eth0（MT7621 的 GMAC0），所以默认取记录里第 2 个 MAC
 * （实测它正是 Linux 侧 eth0 的值）。要换用别的槽改这里即可：
 *   0 = 预留/无接口   1 = eth0   2 = wlan0   3 = wlan1
 */
#ifndef MAX3_U_BOOT_MAC_IDX
#define MAX3_U_BOOT_MAC_IDX	1
#endif
#if MAX3_U_BOOT_MAC_IDX >= MAX3_MAC_NUM
#error "MAX3_U_BOOT_MAC_IDX 超出 MAX3_MAC_NUM"
#endif

void *mtk_board_get_flash_dev(void);
int mtk_board_flash_read(void *flashdev, uint64_t offset, size_t len,
			 void *buf);

static int max3_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

/* "aa:bb:cc:dd:ee:ff" -> 6 字节；格式不对返回 -1（不依赖 <net.h>） */
static int max3_parse_mac(const char *s, u8 *mac)
{
	int i;

	if (!s)
		return -1;

	for (i = 0; i < 6; i++) {
		int h, l;

		h = max3_hexval(s[0]);
		l = (h < 0) ? -1 : max3_hexval(s[1]);
		if (h < 0 || l < 0)
			return -1;

		mac[i] = (u8)((h << 4) | l);
		s += 2;

		if (i < 5) {
			if (*s != ':' && *s != '-')
				return -1;
			s++;
		}
	}

	return 0;
}

static int max3_is_alnum(u8 c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
	       (c >= 'a' && c <= 'z');
}

/* payload 里 12 字节是不是 12 位十六进制（实测一律大写，这里两种都收） */
static int max3_mac_field_ok(const u8 *b, size_t off)
{
	int k;

	for (k = 0; k < 12; k++)
		if (max3_hexval((char)b[off + k]) < 0)
			return 0;

	return 1;
}

/* 12 位 ASCII -> 6 字节 */
static void max3_mac_decode(const u8 *p, u8 *out)
{
	int k;

	for (k = 0; k < 6; k++)
		out[k] = (u8)((max3_hexval((char)p[2 * k]) << 4) |
			      max3_hexval((char)p[2 * k + 1]));
}

/*
 * payload 偏移 off 处的 16 字节是不是 SN：
 * 纯字母数字、至少 4 个数字、且前后都不接字母数字（防止从长串里截一段）。
 */
static int max3_sn_at(const u8 *p, size_t len, size_t off, char *out)
{
	int k, dig = 0;

	if (off + 16 > len)
		return 0;
	if (off > 0 && max3_is_alnum(p[off - 1]))
		return 0;
	if (off + 16 < len && max3_is_alnum(p[off + 16]))
		return 0;

	for (k = 0; k < 16; k++) {
		u8 c = p[off + k];

		if (!max3_is_alnum(c))
			return 0;
		if (c >= '0' && c <= '9')
			dig++;
	}
	if (dig < 4)
		return 0;

	memcpy(out, p + off, 16);
	out[16] = '\0';

	return 1;
}

/*
 * 扫一个 512 KiB 窗口，找 id=47 / size=228 的设备身份记录。
 * 输出该区里出现次数最多的那一组 4 个 MAC（macs[0..3][6]），票数进 *hits，
 * SN 进 sn。一条都没命中时 *hits = 0。
 */
static void max3_mnv_scan(const u8 *buf, size_t len, u8 macs[][6], int *hits,
			  char *sn, size_t snsz)
{
	static u8 g_mac[MAX3_MAC_GRP][MAX3_MAC_NUM][6];
	static int g_cnt[MAX3_MAC_GRP];
	size_t i;
	int ng = 0, g, k, best = 0;

	*hits = 0;
	for (k = 0; k < MAX3_MAC_NUM; k++)
		memset(macs[k], 0, 6);
	if (sn && snsz)
		sn[0] = '\0';

	memset(g_mac, 0, sizeof(g_mac));
	memset(g_cnt, 0, sizeof(g_cnt));

	for (i = MAX3_MNV_REC_HDR; i + MAX3_MNV_REC_CHECK <= len; i++) {
		u32 rid, sz;
		const u8 *p;
		u8 f[MAX3_MAC_NUM][6];
		int ok = 1;

		if (buf[i] != 'F' || buf[i + 1] != 'D' ||
		    buf[i + 2] != 'N' || buf[i + 3] != 'M')
			continue;

		/* 真记录前面必有 sig；payload 里嵌的那个 "FDNM" 没有，跳过 */
		if (memcmp(buf + i - MAX3_MNV_REC_HDR,
			   "\x5c\xb6\x0a\x02", 4))
			continue;

		rid = (u32)buf[i - 0x0C] | ((u32)buf[i - 0x0B] << 8) |
		      ((u32)buf[i - 0x0A] << 16) | ((u32)buf[i - 0x09] << 24);
		sz  = (u32)buf[i - 0x08] | ((u32)buf[i - 0x07] << 8) |
		      ((u32)buf[i - 0x06] << 16) | ((u32)buf[i - 0x05] << 24);

		if (rid != MAX3_MNV_REC_ID || sz < MAX3_MNV_REC_SIZE)
			continue;

		if (i + MAX3_MNV_REC_CHECK + MAX3_MNV_REC_SIZE > len)
			continue;

		p = buf + i + MAX3_MNV_REC_CHECK;

		/* 4 个 12 位 MAC 连写，且必须同 OUI，否则不当身份记录 */
		for (k = 0; k < MAX3_MAC_NUM; k++) {
			if (!max3_mac_field_ok(p, 12 * k)) {
				ok = 0;
				break;
			}
			max3_mac_decode(p + 12 * k, f[k]);
		}

		if (ok && !memcmp(f[0], f[1], 3) && !memcmp(f[1], f[2], 3) &&
		    !memcmp(f[2], f[3], 3)) {
			for (g = 0; g < ng; g++)
				if (!memcmp(g_mac[g], f, sizeof(f)))
					break;
			if (g < ng) {
				g_cnt[g]++;
			} else if (ng < MAX3_MAC_GRP) {
				memcpy(g_mac[ng], f, sizeof(f));
				g_cnt[ng] = 1;
				ng++;
			}
		}

		/* SN：payload 中段滑窗，取第一个形态合格的串 */
		if (sn && snsz >= 17 && !sn[0]) {
			size_t j;

			for (j = MAX3_SN_OFF_MIN;
			     j + 16 <= MAX3_MNV_REC_SIZE; j++) {
				if (max3_sn_at(p, MAX3_MNV_REC_SIZE, j, sn))
					break;
			}
		}
	}

	for (g = 0; g < ng; g++) {
		if (g_cnt[g] > best) {
			best = g_cnt[g];
			for (k = 0; k < MAX3_MAC_NUM; k++)
				memcpy(macs[k], g_mac[g][k], 6);
		}
	}
	*hits = best;
}

static int max3_env_ids(void)
{
	static const u32 areas[] = {
		MAX3_MNV_DEFAULT_OFF, MAX3_MNV_BACKUP_OFF,
		MAX3_MNV_IMG_OFF, MAX3_OEMINFO_OFF
	};
	u8 *buf;
	u8 mac[6];
	int i, mac_hits = 0, need_save = 0, ret;
	u32 where = 0;
	char sn[20] = { 0 };
	char macstr[20] = { 0 };
	void *dev;

	/* 三项都固化过就不再读闪存，也不重复写 env */
	if (env_get("ethaddr") && env_get("eth1addr") && env_get("serial#"))
		return 0;

	dev = mtk_board_get_flash_dev();
	if (!dev) {
		printf("MAX3: 取不到 NAND 设备，跳过 SN/MAC 读取\n");
		return 0;
	}

	buf = malloc(MAX3_MNV_SCAN_LEN);
	if (!buf) {
		printf("MAX3: 分配 %u KiB 缓冲失败，跳过 SN/MAC 读取\n",
		       (unsigned)(MAX3_MNV_SCAN_LEN >> 10));
		return 0;
	}

	memset(mac, 0, sizeof(mac));

	for (i = 0; i < (int)(sizeof(areas) / sizeof(areas[0])); i++) {
		u8 m2[MAX3_MAC_NUM][6];
		int h2 = 0, k;
		char s2[20] = { 0 };

		memset(m2, 0, sizeof(m2));

		if (mtk_board_flash_read(dev, areas[i], MAX3_MNV_SCAN_LEN, buf)) {
			printf("MAX3: 读 NV 区 0x%08x 失败\n", (unsigned)areas[i]);
			continue;
		}

		max3_mnv_scan(buf, MAX3_MNV_SCAN_LEN, m2, &h2, s2, sizeof(s2));

		if (h2 > 0) {
			char tmp[80];
			int n = 0;

			for (k = 0; k < MAX3_MAC_NUM; k++)
				n += sprintf(tmp + n,
					     "%s%02x:%02x:%02x:%02x:%02x:%02x",
					     k ? " " : "",
					     m2[k][0], m2[k][1], m2[k][2],
					     m2[k][3], m2[k][4], m2[k][5]);
			printf("MAX3: NV 0x%08x 命中 %d 份，4 个 MAC = %s\n",
			       (unsigned)areas[i], h2, tmp);
		}

		if (h2 > mac_hits) {
			mac_hits = h2;
			memcpy(mac, m2[MAX3_U_BOOT_MAC_IDX], 6);
			where = areas[i];
		}
		if (!sn[0] && s2[0])
			memcpy(sn, s2, sizeof(sn));

		/* MAC 至少两处一致 + SN 都拿到，就够可信，不再多读 */
		if (mac_hits >= 2 && sn[0])
			break;
	}

	free(buf);

	if (mac_hits) {
		sprintf(macstr, "%02x:%02x:%02x:%02x:%02x:%02x",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		printf("MAX3: 从 NV(0x%08x) 读出 MAC %s（命中 %d 份）\n",
		       (unsigned)where, macstr, mac_hits);

		if (!env_get("ethaddr")) {
			env_set("ethaddr", macstr);
			need_save = 1;
		} else if (strcmp(env_get("ethaddr"), macstr)) {
			printf("MAX3: env 已有 ethaddr=%s，与 NV 不同，保留 env（要覆盖请先 env delete ethaddr）\n",
			       env_get("ethaddr"));
		}
	} else {
		printf("MAX3: NV 区里没找到可信 MAC，保持现状\n");
	}

	if (sn[0]) {
		printf("MAX3: 从 NV 读出 SN %s\n", sn);
		if (!env_get("serial#")) {
			env_set("serial#", sn);
			need_save = 1;
		} else if (strcmp(env_get("serial#"), sn)) {
			printf("MAX3: env 已有 serial#=%s，与 NV 不同，保留 env\n",
			       env_get("serial#"));
		}
	} else {
		printf("MAX3: NV 区里没找到 SN，保持现状\n");
	}

	/*
	 * 第二网口 eth1addr = 第一 MAC + 1。
	 * 基准 MAC 优先取 env 里生效的 ethaddr（保证 eth0/eth1 始终同芯片连号），
	 * env 没有或格式不合法时才退回 NV 读出的值。
	 * 进位按整 6 字节处理：...:4b:ff + 1 -> ...:4c:00。
	 */
	{
		u8 m2[6];
		char s2[20];
		const char *ea = env_get("ethaddr");
		int have = 0;

		if (ea && max3_parse_mac(ea, m2) == 0) {
			have = 1;
		} else if (mac_hits) {
			memcpy(m2, mac, 6);
			have = 1;
		}

		if (have) {
			for (i = 5; i >= 0; i--)
				if (++m2[i])
					break;

			sprintf(s2, "%02x:%02x:%02x:%02x:%02x:%02x",
				m2[0], m2[1], m2[2], m2[3], m2[4], m2[5]);
			printf("MAX3: 第二网口 eth1addr = %s（第一 MAC +1）\n", s2);

			if (!env_get("eth1addr")) {
				env_set("eth1addr", s2);
				need_save = 1;
			} else if (strcmp(env_get("eth1addr"), s2)) {
				printf("MAX3: env 已有 eth1addr=%s，与推算值不同，保留 env\n",
				       env_get("eth1addr"));
			}
		}
	}

	if (need_save) {
		ret = env_save();
		if (ret)
			printf("MAX3: SN/MAC 写入 env 失败(%d)，下次启动会重试\n", ret);
		else
			printf("MAX3: SN/MAC 已固化到 U-Boot env\n");
	}

	return 0;
}

/*
 * misc_init_r 在 initr_env 之后、initr_net 之前执行（common/board_r.c 里
 * initr_env=778 行、misc_init_r=808 行、initr_net=840 行），
 * 所以这里设好 ethaddr，**本次启动的网络初始化就直接用真实 MAC**，
 * 不用等下一次。
 */
int misc_init_r(void)
{
#ifndef CONFIG_SPL_BUILD
	/*
	 * MAX3: 外置看门狗必须在这里就强制关闭，不能等 max3autoboot。
	 *
	 * 实测故障：停在 U-Boot 命令行时 max3autoboot 不会被执行，
	 * max3_gpio_init() 因此不被调用，GPIO10 保持使能（高），
	 * 约 56 秒被外置看门狗硬复位 —— 表现为"U-Boot 自动重启"。
	 * gpio status 只看到 13/15/16(mbb-id*) 与 14(button-reset)，
	 * 表里 17/11/12/9/6/5 一个都没 claim，正是这个问题的指纹。
	 *
	 * misc_init_r 在 board_init_r 早期执行，早于命令行、
	 * 早于任何 tftp/NAND 操作，因此放在这里可以从根本上消除
	 * "有人停在命令行 → 狗咬"的窗口。函数幂等，后续
	 * max3_gpio_init() / m3_prog_src() 再调用也不会重复出错。
	 */
	extern int max3_wdt_disable(void);

	max3_wdt_disable();
#endif
	max3_env_ids();
	return 0;
}
#endif /* CONFIG_MISC_INIT_R */

#ifndef CONFIG_SPL_BUILD	/* 仅 U-Boot proper：由 max3autoboot 命令入口调用 */

/* 板载外置看门狗的复位/使能脚 */
#define MAX3_WDT_GPIO		10

/*
 * 关闭板载外置看门狗。
 *
 * 该芯片上电后默认使能，约 56 秒无喂狗即硬复位。这会打断 max3flash /
 * max3uboot 的 NAND 写入（单槽 7 MiB，含回读校验），写到一半被打断
 * 会留下半截镜像 —— 这是绝对不能出现的。
 *
 * 因此它不能挂在下面的"表序循环"里：循环遇到 request/direction 失败
 * 就 break，排在它前面的 17/11/12 任何一项失败都会让它被跳过。
 * 独立成函数、幂等、失败可重试，并在每个擦写/救援入口先执行一次。
 *
 * 实测：pin10 高=使能、低=禁用；拉低后 sleep 60 不再复位。
 * Linux 侧应用启动后会自行重新使能并喂狗，不受影响。
 */
int max3_wdt_disable(void)
{
	int ret;

	ret = gpio_request(MAX3_WDT_GPIO, "wdt_set");
	if (ret && ret != -EBUSY)
		return ret;			/* 只容忍已占用，其它错误上报 */

	gpio_direction_output(MAX3_WDT_GPIO, 0);
	gpio_set_value(MAX3_WDT_GPIO, 0);

	return 0;
}

/*
 * MAX3 板级 GPIO 表。
 *
 * 字段与布局对齐原厂 max3_gpio_init 所用的表（原有表 @0x80244DEC、
 * 步长 0x30、11 项，字段 enable/name/gpio/dir/level）：enable 决定做不做，
 * dir==1 表示输出并取 level，dir==0 表示输入。执行方式同为"按表序逐项执行"。
 *
 * 本表只列出本板真正要执行的动作。原厂表中 enable=0 的
 * max3_gpio_wdt_set(10) / m_nr_led_green(24) / m_lte_led_green(22) /
 * m_warm_test_led(26) / ap_led(27) 原厂一律跳过，这里同样不碰。
 * 其中 GPIO10（外置看门狗）改由独立的 max3_wdt_disable() 强制关闭，
 * 不放进这张表 —— 理由见该函数注释。
 *
 * 顺序即执行顺序，必须与原厂表序一致 —— 尤其 usb20_sw(11) 必须先于
 * usb20_vbus_ctrl(12)：原厂是"先把 USB2 通道选通、再控 VBUS"。
 */
struct max3_gpio_cfg {
	int enable;
	const char *name;
	int gpio;
	int dir;
	int level;
};

static const struct max3_gpio_cfg max3_gpio_table[] = {
	/* 0 */ { 1, "max3_gpio_m_pwr_ctrl",        17, 0, 0 },
	/* 1 */ { 1, "max3_gpio_usb20_sw",          11, 1, 1 },
	/* 2 */ { 1, "max3_gpio_usb20_vbus_ctrl",   12, 1, 0 },
	/* 3 */ /* 原厂第 3 项 wdt_set(10)：改由 max3_wdt_disable() 独立强制关闭 */
	/* 4 */ { 1, "max3_gpio_m_fastboot_ctrl",    9, 1, 0 },
	/* 5 */ { 1, "max3_gpio_m_power_on_ctrl",    6, 1, 1 },
	/* 6 */ { 1, "max3_gpio_m_rest_ctrl",        5, 0, 0 },
	/* 7 */ { 1, "max3_gpio_led_lte",            3, 1, 0 },
	/* 8 */ { 1, "max3_gpio_led_nr",             4, 1, 0 },
};

/*
 * 表驱动初始化。与原厂循环等价：enable != 1 直接跳过；
 * 输出型走 gpio_direction_output(gpio, level)，输入型走 gpio_direction_input。
 * gpio_output_init()/gpio_input_init() 内部已做 request 与 -EBUSY 容忍。
 *
 * 非 static：调用点已移到 max3autoboot 命令入口（cmd_max3.c），
 * 与原厂"先打印 max3 autoboot start、再执行表"的时序一致。
 */
void max3_gpio_init(void)
{
	const struct max3_gpio_cfg *cfg = max3_gpio_table;
	unsigned int i, n = sizeof(max3_gpio_table) / sizeof(max3_gpio_table[0]);

	/* 看门狗先行关闭：不受下面循环 break 的影响 */
	max3_wdt_disable();

	for (i = 0; i < n; i++, cfg++) {
		if (cfg->enable != 1)
			continue;

		if (cfg->dir == 1) {
			if (gpio_output_init(cfg->gpio, cfg->level, cfg->name))
				break;
		} else {
			if (gpio_input_init(cfg->gpio, cfg->name))
				break;
		}
	}
}

#endif /* !CONFIG_SPL_BUILD */

#ifdef CONFIG_LAST_STAGE_INIT

int last_stage_init(void)
{
	/*
	 * MAX3: GPIO 表已移到 max3autoboot 命令入口（cmd_max3.c 的
	 * do_max3autoboot），时序与原厂一致 —— 原厂也是先打印
	 * "max3 autoboot start"，再执行那张 11 项表。
	 *
	 * 这里只保留 JTAG_MODE 置位：该地址是 SYSCTL 的 GPIO_MODE 寄存器，
	 * bit7 = JTAG_MODE。置 1 后 JTAG 引脚（13/15/16）才作为普通 GPIO
	 * 使用 —— 这是下面读 strap、以及 max3_mbb_smem_init() 里
	 * gpio_get_value(13/15/16) 取到真实电平的前提。
	 */
	void __iomem *sysctl = (void __iomem *)CKSEG1ADDR(MT7621_SYSCTL_BASE);

	setbits_le32(sysctl + MT7621_SYS_GPIO_MODE_REG, 1 << JTAG_MODE_S);

	/*
	 * GPIO 表不在此处执行：已移到 do_max3autoboot()（cmd_max3.c），
	 * 以对齐原厂"autoboot 之后才设定 11/12"的时序。
	 * 表内顺序：17(输入) -> 11(输出高 usb20_sw) -> 12(输出低 vbus)
	 *   -> 9 -> 6 -> 5 -> LED(3/4)；外置看门狗(10) 不在此表，
	 *   由 max3_wdt_disable() 在表之前单独关闭。
	 */

#if defined(CONFIG_BOARD_MT7621_NAND_TEMPLATE)
	#define MT7621_UNUSED_PIN_LIST {}
#ifdef MT7621_BUTTON_RESET
	gpio_input_init(MT7621_BUTTON_RESET, "button-reset");	// init button reset
#endif
	/*
	 * 三颗灯已并入 max3_gpio_table（11=led-sys 常亮、3=led-lte 灭、
	 * 4=led-nr 灭），此处不再重复申请，避免同一脚被 request 两次。
	 */
#else
	#define MT7621_UNUSED_PIN_LIST {}
#endif

	int pinlist[] = MT7621_UNUSED_PIN_LIST;

	pins_unused_init(pinlist, sizeof(pinlist)/sizeof(int));

	/*
	 * 必须在 Linux 起来之前补 MBB 共享区，模块 probe 时才读得到硬件 ID。
	 * 这是 5G AT 通道全挂的根因修复点，详见 max3_mbb_smem_init() 注释。
	 */
#ifdef MT7621_MBB_SMEM_PHYS
	max3_mbb_smem_init();
#endif

#ifndef CONFIG_BOARD_MT7621_NAND_TEMPLATE
	/* configure the default state (auto-probe) */
	led_default_state();
#endif // CONFIG_BOARD_MT7621_NAND_TEMPLATE

	/* Sort variables alphabetically if the variable "_sortenv" exists */
	if (env_get("_sortenv") != NULL) {
		printf("Sort environment variables alphabetically\n");
		env_set("_sortenv", "");
		env_save();
	}

	return 0;
}
#endif // CONFIG_LAST_STAGE_INIT

#ifdef CONFIG_DEBUG_UART_BOARD_INIT
void board_debug_uart_init(void)
{
#if defined(CONFIG_SPL_BUILD)
	void __iomem *base = (void __iomem *) CKSEG1ADDR(MT7621_SYSCTL_BASE);

	setbits_le32(base + MT7621_SYS_RSTCTL_REG, REG_SET_VAL(UART1_RST, 1));
	clrbits_le32(base + MT7621_SYS_RSTCTL_REG, REG_MASK(UART1_RST));
	clrbits_le32(base + MT7621_SYS_GPIO_MODE_REG, REG_MASK(UART1_MODE));
#endif
}
#endif

int board_nmbm_init(void)
{
#ifdef CONFIG_ENABLE_NAND_NMBM
	struct mtd_info *lower, *upper;
	int ret;

	printf("\n");
	printf("Initializing NMBM ...\n");

	lower = get_nand_dev_by_index(0);
	if (!lower) {
		printf("Failed to create NMBM device due to nand0 not found\n");
		return 0;
	}

	ret = nmbm_attach_mtd(lower, NMBM_F_CREATE, CONFIG_NMBM_MAX_RATIO,
		CONFIG_NMBM_MAX_BLOCKS, &upper);

	printf("\n");

	if (ret)
		return 0;

	add_mtd_device(upper);

	nmbm_usable = 1;
#endif

	return 0;
}

#ifdef CONFIG_SPL_BUILD
void board_boot_order(u32 *spl_boot_list)
{
	spl_boot_list[0] = BOOT_DEVICE_MTK_NAND;
	spl_boot_list[1] = BOOT_DEVICE_MTK_UART;
}

ulong get_mtk_image_search_start(void)
{
	if (__rom_cfg.magic != MTK_ROM_CFG_MAGIC)
		return CONFIG_SPL_ALIGN_TO;

	return __rom_cfg.size + sizeof(struct image_header);
}

ulong get_mtk_image_search_end(void)
{
	return CONFIG_MAX_U_BOOT_SIZE;
}

ulong get_mtk_image_search_sector_size(void)
{
	if (__rom_cfg.magic != MTK_ROM_CFG_MAGIC)
		return CONFIG_SPL_ALIGN_TO;

	return __rom_cfg.align;
}
#endif

struct mtd_info *get_mtk_board_nand_mtd(void)
{
#ifdef CONFIG_ENABLE_NAND_NMBM
	struct mtd_info *mtd;

	if (nmbm_usable) {
		mtd = get_mtd_device_nm("nmbm0");
		if (IS_ERR(mtd))
			return NULL;
		return mtd;
	}
#endif

	return get_nand_dev_by_index(0);
}

#ifndef CONFIG_SPL_BUILD
void *mtk_board_get_flash_dev(void)
{
	return get_mtk_board_nand_mtd();
}

size_t mtk_board_get_flash_erase_size(void *flashdev)
{
	struct mtd_info *mtd = (struct mtd_info *)flashdev;

	return mtd->erasesize;
}

int mtk_board_flash_erase(void *flashdev, uint64_t offset, uint64_t len)
{
	struct mtd_info *mtd = (struct mtd_info *)flashdev;
	struct erase_info instr;
	int ret;

	memset(&instr, 0, sizeof(instr));

	instr.addr = offset;
	instr.len = len;

	ret = mtd_erase(mtd, &instr);
	if (ret)
		return ret;

	if (instr.state != MTD_ERASE_DONE)
		return -1;

	return 0;
}

int mtk_board_flash_read(void *flashdev, uint64_t offset, size_t len,
			 void *buf)
{
	struct mtd_info *mtd = (struct mtd_info *)flashdev;
	size_t retlen;
	int ret;

	ret = mtd_read(mtd, offset, len, &retlen, buf);
	if (ret && ret != -EUCLEAN)
		return ret;

	if (retlen != len)
		return -EIO;

	return 0;
}

int mtk_board_flash_write(void *flashdev, uint64_t offset, size_t len,
			  const void *buf)
{
	struct mtd_info *mtd = (struct mtd_info *)flashdev;
	size_t retlen;

	return mtd_write(mtd, offset, len, &retlen, buf);
}
#endif
