#!/bin/bash

# Ensure the script exits on error
set -e

# Store the current working directory
cwd="$PWD"

# Toolchain paths
TOOLCHAIN_PATH=~/android/lunaris/prebuilts/clang/host/linux-x86/clang-r574158/bin

export PATH="$TOOLCHAIN_PATH:$GCC64_PATH:$GCC32_PATH:$PATH"

# Build variables
export ARCH=arm64
export SUBARCH=arm64
export KBUILD_BUILD_USER="spidey"
export KBUILD_BUILD_HOST="spidey"

MAKE_ARGS="O=out LLVM=1 LLVM_IAS=1 CC=clang CROSS_COMPILE=aarch64-linux-android- CROSS_COMPILE_ARM32=arm-linux-androideabi- CLANG_TRIPLE=aarch64-linux-gnu- KSU_GIT_VERSION_VALID=1 KSU_GIT_VERSION=3025 KSU_GIT_TAG=v3.4.2-legacy"

# The in-tree dtc (DTC 1.4.6) emits overlay fixups that the device ABL fails
# to apply, causing a boot hang. Use the AOSP prebuilt dtc (1.7.0), which is
# what the ROM build uses and produces byte-identical dtbos to the stock image.
DTC_BIN="$HOME/android/lunaris/prebuilts/kernel-build-tools/linux-x86/bin/dtc"
if [ ! -x "$DTC_BIN" ]; then
    echo "==> AOSP prebuilt dtc not found at $DTC_BIN"
    exit 1
fi
export DTC_EXT="$DTC_BIN"

echo "==> Cleaning..."
rm -rf out/
mkdir -p out

echo "==> Building kernel for lemonades..."

# Sync config
make $MAKE_ARGS vendor/kona-perf_defconfig

# Apply customizations to .config
cat arch/arm64/configs/vendor/oplus.config >> out/.config
cat <<EOF >> out/.config
CONFIG_LITTLE_CPU_MASK=15
CONFIG_BIG_CPU_MASK=112
CONFIG_PRIME_CPU_MASK=128
EOF

make $MAKE_ARGS olddefconfig
make $MAKE_ARGS -j$(nproc --all) Image.gz dtbs 2>&1 | tee build.log

if [ -f "out/arch/arm64/boot/Image.gz" ]; then
    echo "==> Kernel built successfully!"
else
    echo "==> Build failed!"
    exit 1
fi

# Packaging into flashable ZIP
ANYKERNEL_DIR="$HOME/AnyKernel3"
GIT_COMMIT_ID=$(git rev-parse --short=8 HEAD)
ZIP_NAME="Spidey_kernel_lemonades_anykernel3_${GIT_COMMIT_ID}_$(date +%Y%m%d_%H%M%S).zip"

echo "==> Packaging Flashable ZIP..."
rm -f "$ANYKERNEL_DIR/$ZIP_NAME"
cp out/arch/arm64/boot/Image.gz "$ANYKERNEL_DIR/"

DTB_DIR=out/arch/arm64/boot/dts/vendor/oplus
DTB_LIST="$(find "$DTB_DIR" -maxdepth 1 -name '*.dtb' | LC_ALL=C sort)"
if [ -z "$DTB_LIST" ]; then
    echo "==> No DTBs built in $DTB_DIR!"
    exit 1
fi
cat $DTB_LIST > "$ANYKERNEL_DIR/dtb"

DTBO_LIST="$(find "$DTB_DIR" -maxdepth 1 -name '*-overlay.dtbo' | LC_ALL=C sort)"
if [ -z "$DTBO_LIST" ]; then
    echo "==> No DTBO overlays built in $DTB_DIR!"
    exit 1
fi
python3 "$cwd/pack_dtbo.py" -o "$ANYKERNEL_DIR/dtbo.img" --pad-to 25165824 $DTBO_LIST

cd "$ANYKERNEL_DIR"
zip -r9 "$ZIP_NAME" * -x .git README.md *placeholder
OUTPUT_DIR="$HOME/kernel-zips"
mkdir -p "$OUTPUT_DIR"
cp "$ZIP_NAME" "$OUTPUT_DIR"
mv "$ZIP_NAME" "$cwd" # Move the ZIP from Anykernel3 directory else next build will include all zip files
echo "------------------------------ Completed ------------------------------"
echo "-   $ZIP_NAME   -"
echo "-----------------------------------------------------------------------"
