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

MAKE_ARGS="O=out LLVM=1 LLVM_IAS=1 CC=clang CROSS_COMPILE=aarch64-linux-android- CROSS_COMPILE_ARM32=arm-linux-androideabi- CLANG_TRIPLE=aarch64-linux-gnu-"

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
find out/arch/arm64/boot/dts/vendor/oplus/ -name "*.dtb" -exec cp {} "$ANYKERNEL_DIR/dtb" \;
cp out/arch/arm64/boot/dts/vendor/oplus/kona-lemonades-overlay.dtbo "$ANYKERNEL_DIR/dtbo.img"

cd "$ANYKERNEL_DIR"
zip -r9 "$ZIP_NAME" * -x .git README.md *placeholder
OUTPUT_DIR="$HOME/kernel-zips"
mkdir -p "$OUTPUT_DIR"
cp "$ZIP_NAME" "$OUTPUT_DIR"
cp "$ZIP_NAME" "$cwd"

echo "==> Done! ZIP: $ZIP_NAME"
