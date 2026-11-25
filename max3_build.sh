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

echo "构建目标：$TARGET"
make CROSS_COMPILE="$CB" STAGING_DIR="$STG" -j"$(nproc 2>/dev/null || echo 4)" "$TARGET"

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
