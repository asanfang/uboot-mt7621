#!/bin/bash
#
# MAX3 自建 U-Boot 构建脚本（Linux / WSL / GitHub Actions 通用）
#
# 用法：
#   ./max3_build.sh                            # 自动找 ../openwrt*/toolchain-mipsel*/bin
#   ./max3_build.sh /abs/path/mipsel-openwrt-linux-
#
# 产物：
#   u-boot-lzma.img     ← 唯一需要烧进 mtd0 0x20000 的载荷（含 64 字节 uImage 头）
#   u-boot-mt7621.bin   ← 整片镜像（含 SPL），仅做留档，不要整片刷
#
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

CB="$1"
if [ -z "$CB" ]; then
	for d in ../openwrt*/toolchain-mipsel*/bin; do
		[ -d "$d" ] && CB="$(cd "$d" && pwd)/mipsel-openwrt-linux-" && break
	done
fi
if [ -z "$CB" ] || [ ! -x "${CB}gcc" ]; then
	echo "找不到交叉编译器：${CB}gcc"
	echo "请显式传入工具链前缀，例如 ./max3_build.sh /opt/tc/bin/mipsel-openwrt-linux-"
	exit 1
fi
STG="${CB%/bin/*}"
[ "$STG" = "$CB" ] && STG="${CB%/*/*}"

echo "CROSS_COMPILE=$CB"
echo "STAGING_DIR  =$STG"

# 上限：mtd0 0x20000-0x5FFFF 共 0x40000 = 262144 字节
LIMIT=$((0x40000))

# 只构建载荷目标：绕开 binaly 链路里的 binman（2018.09 的 binman 吃 python2，
# 而 u-boot-mt7621.bin 走 binman；我们只需 mtd0 0x20000 起的载荷）
TARGET="${TARGET:-u-boot-lzma.img}"

make distclean >/dev/null 2>&1 || true
make mt7621_max3_defconfig

# 关键一步：非交互补全 .config。
# 缺了这一步，在没有 tty 的环境（CI / 管道）里，syncconfig 会把"没有值"的符号
# 当成需要交互询问，fall through 到 xfgets() 去读 stdin；读不到就打印
# "Error in reading or end of file."，而 conf_sym() 是 while(1)，
# 于是无限刷屏、auto.conf 永远生成不出来，编译一步也不动。
make olddefconfig </dev/null

# 防呆（必须区分两种"空"）：
#   CONFIG_X=""   → 合法的空字符串值，syncconfig 不会询问（上游 defconfig 常态，
#                   如 IDENT_STRING / LOCALVERSION / DEFAULT_FDT_FILE）
#   CONFIG_X=     → 行尾什么都没有，即真正缺值，syncconfig 会把它当成 NEW 符号
#                   去交互问；无 tty 时 conf_sym() 的 while(1) 无限刷
#                   "Error in reading or end of file."，编译一步也不推进。
# 所以只拦后者，前者必须放行，否则会误杀合法配置。
EMPTY_RE='^CONFIG_[A-Za-z0-9_]+=[[:space:]]*$'
if grep -Eq "$EMPTY_RE" .config; then
	echo "配置里存在无值符号（syncconfig 会交互询问，导致死循环）："
	grep -nE "$EMPTY_RE" .config | head -n 20
	echo "请在上游 defconfig 里给这些符号补上合法值"
	exit 1
fi

# 留证：ENV 四项现值打进日志，方便回看这轮用的到底是哪套 env 几何
echo "ENV 符号现值："
grep -E '^CONFIG_ENV_(SECT_SIZE|OFFSET|OFFSET_REDUND|SIZE)=' .config || echo "  （未找到 ENV 符号）"

# LTO 插件兜底
# 这版 OpenWrt gcc 12.3.0 的 specs 默认带 -fuse-linker-plugin，需要
# libexec/gcc/<target>/<version>/liblto_plugin.so。精简工具链若缺这个插件，
# gcc 驱动在编译第一个文件时就报 "liblto_plugin.so not found" 并直接退出，
# 表现为 autoconf.mk.dep 生成失败、随后 auto.conf 找不到而 stop。
# 处置：显式关掉插件依赖（-fno-use-linker-plugin），再包一层 CC 覆盖
# Makefile:349 的 CC，保证 -E/-c/-S/链接四条路径都带上该选项。
PLUG=$("${CB}gcc" -print-file-name=liblto_plugin.so 2>/dev/null || echo "")
echo "liblto_plugin.so -> ${PLUG:-<未找到>}"
CC_WRAP="$ROOT/.tcwrap-gcc"
printf '#!/bin/sh\nexec "%s" -fno-use-linker-plugin -fno-lto "$@"\n' "${CB}gcc" > "$CC_WRAP"
chmod +x "$CC_WRAP"

echo "构建目标：$TARGET"
JOBS=$(nproc 2>/dev/null || echo 2)
if [ "$JOBS" -gt 4 ]; then
	JOBS=4
fi
echo "并行度：-j$JOBS"

set +e
timeout "${BUILD_TIMEOUT:-1500}" \
	make CROSS_COMPILE="$CB" STAGING_DIR="$STG" CC="$CC_WRAP" \
		KCFLAGS="-fno-use-linker-plugin -fno-lto" \
		-j"$JOBS" "$TARGET" </dev/null
RC=$?
set -e
if [ "$RC" -ne 0 ]; then
	echo "构建未成功，退出码 $RC"
	if [ "$RC" -eq 124 ]; then
		echo "已超过 ${BUILD_TIMEOUT:-1500} 秒仍未完成，按超时中止"
	fi
	exit "$RC"
fi

if [ ! -f "$TARGET" ]; then
	echo "缺少产物 $TARGET"
	exit 1
fi

SIZE=$(stat -c%s "$TARGET")
printf '%s  %d 字节 (0x%X)\n' "$TARGET" "$SIZE" "$SIZE"
printf '预算上限      %8d 字节 (0x%X)，余量 %d\n' "$LIMIT" "$LIMIT" $((LIMIT - SIZE))

if [ "$SIZE" -gt "$LIMIT" ]; then
	echo "超标！载荷超过 262144 字节：砍功能，或把 env 后移到 0xC0000（上限升到 655360）"
	exit 1
fi

mkdir -p dist
cp "$TARGET" dist/
cp .config dist/max3_defconfig_used
echo "产物已复制到 dist/"
