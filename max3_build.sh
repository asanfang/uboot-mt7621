#!/bin/bash
#
# MAX3 自建 U-Boot 构建脚本（Linux / WSL / GitHub Actions 通用）
#
# 用法：
#   ./max3_build.sh                            # 自动找 ../openwrt*/toolchain-mipsel*/bin
#   ./max3_build.sh /abs/path/mipsel-openwrt-linux-
#
# 产物（都在 dist/）：
#   u-boot-lzma.img          ← 唯一需要烧进 mtd0 0x20000 的载荷（含 64 字节 uImage 头）
#   u-boot.bin               ← 解压前的裸镜像，用于对照
#   u-boot.map / u-boot      ← 符号表与 ELF，核对 _start / reset 地址用
#   max3_defconfig_used      ← 本轮实际使用的 .config
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

# 只构建载荷目标：绕开 binman（2018.09 的 binman 吃 python2，
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

# ---- 载荷起始校验 / 自愈 ----
# 现象：这条构建链产出的 u-boot.bin 会从链接地址 4 开始输出，丢掉 _start 的第一条
#   b reset（LE 0x10000001）。链接器把 reset 定在 0x80200008，代码里所有 la 都按
#   这个地址签发；镜像若少这 4 字节，加载到 0x80200000 后全部绝对地址引用会偏 4。
# 判据（解压后首 4 字节）：
#   01 00 00 10 → 正常，b reset 在位（offset=1，跳 PC+4+4 = +8）
#   00 48 80 40 → 缺 b，首字是 delay slot 的 mtc0 zero, CP0_COUNT
# 处置：补回 4 字节，用 lzma 工具重压缩，用本轮构建出的 tools/mkimage 重打包，
#   与 Makefile 的 cmd_lzma / mkimage 流程同源，头由 mkimage 自己签发。
FIX_BIN="$ROOT/u-boot.bin.fix"
FIX_RC=0
if command -v python3 >/dev/null 2>&1; then
	set +e
	python3 - "$TARGET" "$FIX_BIN" <<'PYEOF'
import sys, lzma
src, dst = sys.argv[1], sys.argv[2]
raw = lzma.decompress(open(src, 'rb').read()[64:], format=lzma.FORMAT_ALONE)
head = raw[:4]
if head == b'\x01\x00\x00\x10':
    print('载荷起始正常：b reset 在位，未做改动')
    sys.exit(0)
if head == b'\x00\x48\x80\x40':
    open(dst, 'wb').write(b'\x01\x00\x00\x10' + raw)
    print('载荷缺少 b reset（解压后首字 mtc0），已生成补齐版')
    sys.exit(10)
print('载荷起始 4 字节非预期：' + head.hex())
sys.exit(2)
PYEOF
	FIX_RC=$?
	set -e
else
	echo "无 python3，跳过载荷起始自检"
fi

if [ "$FIX_RC" -eq 10 ]; then
	if ! command -v lzma >/dev/null 2>&1; then
		echo "缺少 lzma 工具（xz-utils），无法重压缩"
		exit 1
	fi
	if [ ! -x tools/mkimage ]; then
		echo "缺少 tools/mkimage，无法重打包"
		exit 1
	fi
	LZ_TMP="$ROOT/u-boot.bin.fix.lzma"
	lzma -z -9 -c "$FIX_BIN" > "$LZ_TMP"
	tools/mkimage -A mips -T firmware -C lzma -O u-boot \
		-a 0x80200000 -e 0x80200000 \
		-n "U-Boot 2018.09" \
		-d "$LZ_TMP" "$TARGET"
	echo "已重打包 $TARGET（补 4 字节后）"
elif [ "$FIX_RC" -ne 0 ]; then
	echo "载荷起始 4 字节非预期（python 退出码 $FIX_RC），不自动修补，请人工确认"
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
cp u-boot.bin dist/ 2>/dev/null || true
cp u-boot.map dist/ 2>/dev/null || true
echo "产物已复制到 dist/"
sha256sum "$TARGET"
