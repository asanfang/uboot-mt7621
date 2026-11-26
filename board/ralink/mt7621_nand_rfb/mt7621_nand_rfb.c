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

#ifdef CONFIG_LAST_STAGE_INIT
int last_stage_init(void)
{
	/*
	 * MAX3: 板级 GPIO 初始化，与原厂 U-Boot 的 max3_gpio_init 对齐
	 * （载荷 0x8021B720，表 0x80244DEC：每项 enable|name|gpio|dir|level，
	 *   步长 0x30，共 11 项）。11 项里只有 6 项 enable=1，其余 5 项是灯
	 *   （en=0），原厂同样不设，交给应用层。
	 *
	 * 首步照搬原厂的 (*(u32 *)0xbe000060) |= 0x80：
	 * 该地址是 SYSCTL 的 GPIO_MODE 寄存器，bit7 = JTAG_MODE。
	 * 置 1 后 JTAG 引脚（13/15/16）才作为普通 GPIO 使用 ——
	 * 这是下面读 strap、以及 max3_mbb_smem_init() 里
	 * gpio_get_value(13/15/16) 取到真实电平的前提。
	 */
	void __iomem *sysctl = (void __iomem *)CKSEG1ADDR(MT7621_SYSCTL_BASE);

	setbits_le32(sysctl + MT7621_SYS_GPIO_MODE_REG, 1 << JTAG_MODE_S);

	/*
	 * 本轮关键修复点：GPIO12 = usb20_vbus_ctrl。
	 * 原厂每次 boot 都把它设成 output low；V5 此前完全不碰，
	 * 引脚保持输入高阻（悬空）-> 5G 模块 USB VBUS 开关不受控 ->
	 * 模块枚举被拖到 75s 之后 -> 撞上 mic 39.5s 判定窗 ->
	 * [micdetmg]abnormal reboot -> 二次重启。
	 */
	gpio_output_init(12, 0, "usb20_vbus_ctrl");

	/* 原厂表里其余 4 项，方向/电平原样照搬 */
	gpio_output_init(9, 0, "m_fastboot_ctrl");
	gpio_output_init(6, 1, "m_power_on_ctrl");
	gpio_input_init(5, "m_rest_ctrl");
	gpio_input_init(17, "m_pwr_ctrl");

	/*
	 * GPIO11(usb20_sw) 不在此重复设置：它同时是 MT7621_LED_STATUS1(sys_led)，
	 * 下面的灯初始化会把它设成 output + ACTIVE_HIGH(1)，与原厂 usb20_sw
	 * 的 output high 电平一致，无需再抢一次。
	 */

#ifdef MT7621_BUTTON_RESET
	/*
	 * MAX3: 关闭板载外置看门狗
	 *
	 * 原厂 U-Boot 在 GPIO0(wdt_di)/GPIO10(wdt_set) 上控制一颗外置看门狗，
	 * 复位后默认使能，约 46 秒无喂狗即硬复位。这会让 console 停留、
	 * httpd/tftploop 网页救援、按分区刷写等长操作中途被打断。
	 *
	 * 实测：gpio clear 10 -> "mt7621-gpio10: output: 0"，sleep 60 不再复位，
	 * 确认 pin10 高=使能、低=禁用。此处在进入命令行之前关闭，
	 * 之后 U-Boot 全程不再受 46 秒窗口约束。
	 * Linux 侧原厂应用启动后会自行重新使能并喂狗，不受影响。
	 */
	gpio_output_init(10, 0, "wdt_set");
#endif

#if defined(CONFIG_BOARD_MT7621_NAND_TEMPLATE)
	#define MT7621_UNUSED_PIN_LIST {}
#ifdef MT7621_BUTTON_RESET
	gpio_input_init(MT7621_BUTTON_RESET, "button-reset");	// init button reset
#endif
	/*
	 * 三颗灯上电即定态：系统灯常亮（电源/存活指示），lte/nr 灭。
	 * 注意极性：ACTIVE_HIGH(1) 才是亮，原厂模板这里写的 0 是灭，
	 * 直接照抄会得到"上电全黑"，已按实测极性修正。
	 */
#ifdef MT7621_LED_STATUS1
	gpio_output_init(MT7621_LED_STATUS1, MT7621_LED_ACTIVE_HIGH, "led-sys");
#endif
#ifdef MT7621_LED_STATUS2
	gpio_output_init(MT7621_LED_STATUS2, !MT7621_LED_ACTIVE_HIGH, "led-lte");
#endif
#ifdef MT7621_LED_STATUS3
	gpio_output_init(MT7621_LED_STATUS3, !MT7621_LED_ACTIVE_HIGH, "led-nr");
#endif
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
