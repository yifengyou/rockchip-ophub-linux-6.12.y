#!/bin/bash

set -ex
WORKDIR=$(pwd)
TIMESTAMP=$(date +%Y%m%d)
mkdir -p ${WORKDIR}/release

# ==========================================
# >>> 可配置变量区 <<<
# >>> Configurable Variables Section <<<
# ==========================================
DEFCONFIG="aiot_3588ied_defconfig"
DTB_NAME="rockchip/rk3588-aiot-3588ied.dtb"
ARCH="arm64"
CROSS_COMPILE="aarch64-linux-gnu-"
LOCALVERSION="-kdev"
# ==========================================

# 构建内核
# Build kernel
cd ${WORKDIR}/

# 使用指定的 defconfig 生成内核配置
# Generate kernel configuration using the specified defconfig
make ARCH=${ARCH} \
  CROSS_COMPILE=${CROSS_COMPILE} \
  KBUILD_BUILD_USER="builder" \
  KBUILD_BUILD_HOST="kdevbuilder" \
  LOCALVERSION=${LOCALVERSION} \
  ${DEFCONFIG}

# 校验内核版本号是否包含自定义后缀
# Verify that the kernel version contains the custom suffix
KVER=$(make LOCALVERSION=${LOCALVERSION} kernelrelease)
KVER="${KVER/kdev*/kdev}"
if [[ "$KVER" != *kdev ]]; then
  echo "ERROR: KVER does not end with 'kdev', got: ${KVER}"
  exit 1
fi
echo "KVER: ${KVER}"

# 编译内核镜像、设备树和模块
# Compile kernel image, device tree and modules
echo ">>> 编译内核镜像、设备树和模块..."

# 编译设备树二进制文件 (DTB)
# Compile Device Tree Blob (DTB)
make ARCH=${ARCH} \
  CROSS_COMPILE=${CROSS_COMPILE} \
  KBUILD_BUILD_USER="builder" \
  KBUILD_BUILD_HOST="kdevbuilder" \
  LOCALVERSION=${LOCALVERSION} \
  ${DTB_NAME} \
  -j$(nproc)

# 编译内核主镜像 (Image)，抑制未使用函数警告
# Compile main kernel Image, suppress unused-function warnings
make ARCH=${ARCH} \
  CROSS_COMPILE=${CROSS_COMPILE} \
  KBUILD_BUILD_USER="builder" \
  KBUILD_BUILD_HOST="kdevbuilder" \
  LOCALVERSION=${LOCALVERSION} \
  KCFLAGS="-Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types" \
  -j$(nproc)

# 编译内核模块
# Compile kernel modules
make ARCH=${ARCH} \
  CROSS_COMPILE=${CROSS_COMPILE} \
  KBUILD_BUILD_USER="builder" \
  KBUILD_BUILD_HOST="kdevbuilder" \
  LOCALVERSION=${LOCALVERSION} \
  KCFLAGS="-Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types" \
  modules -j$(nproc)

# 安装模块到临时目录 kos/
# Install modules to temporary directory kos/
make ARCH=${ARCH} \
  CROSS_COMPILE=${CROSS_COMPILE} \
  KBUILD_BUILD_USER="builder" \
  KBUILD_BUILD_HOST="kdevbuilder" \
  LOCALVERSION=${LOCALVERSION} \
  INSTALL_MOD_PATH="$(pwd)/kos" \
  modules_install

# 安装内核头文件
# Install kernel headers
make ARCH=${ARCH} \
  CROSS_COMPILE=${CROSS_COMPILE} \
  KBUILD_BUILD_USER="builder" \
  KBUILD_BUILD_HOST="kdevbuilder" \
  LOCALVERSION=${LOCALVERSION} \
  INSTALL_HDR_PATH="$(pwd)/kernel-headers/${KVER}" \
  headers_install

KVER=$(cat include/config/kernel.release)

# 收集构建产物到 release/ 目录
# Collect build artifacts into release/ directory
echo ">>> 收集构建产物到 release/ ..."

# --- 内核镜像 ---
# --- Kernel Image ---
ls -alh arch/arm64/boot/Image
sha256sum arch/arm64/boot/Image
cp -a arch/arm64/boot/Image "${WORKDIR}/release/Image-${KVER}"

# --- 设备树（使用变量路径，并校验文件存在性）---
# --- Device Tree (use variable path and verify file existence) ---
DTB_PATH="./arch/arm64/boot/dts/${DTB_NAME}"
if [ ! -f "${DTB_PATH}" ]; then
  echo "ERROR: DTB not found at ${DTB_PATH}"
  exit 1
fi
ls -alh "${DTB_PATH}"
sha256sum "${DTB_PATH}"
cp -a "${DTB_PATH}" "${WORKDIR}/release/"

# --- 内核配置文件 ---
# --- Kernel Configuration File ---
cp .config "${WORKDIR}/release/config-${KVER}"
ls -alh "${WORKDIR}/release/config-${KVER}"
sha256sum "${WORKDIR}/release/config-${KVER}"

# --- System.map 符号表 ---
# --- System.map Symbol Table ---
cp System.map "${WORKDIR}/release/System.map-${KVER}"
ls -alh "${WORKDIR}/release/System.map-${KVER}"
sha256sum "${WORKDIR}/release/System.map-${KVER}"

# --- 内核模块打包（strip 版本 + debug 版本）---
# --- Kernel Modules Packaging (stripped release + debug version) ---
if [ -d kos/lib/modules ]; then
  dir_size=$(du -sb kos/lib/modules | awk '{print $1}')
  if [ "$dir_size" -gt 512 ]; then
    # 保留一份未 strip 的调试副本
    # Keep an unstripped debug copy
    cp -a kos kos-debug
    # 对发布版模块去除调试符号以减小体积
    # Strip debug symbols from release modules to reduce size
    find kos -name "*.ko" -print0 | xargs -0 -r ${CROSS_COMPILE}strip --strip-debug
    find kos -name "*.ko"
    ls -alh kos/lib/modules/
    mkdir -p "${WORKDIR}/release"
    tar -zcvf "${WORKDIR}/release/kos-${KVER}.tar.gz" kos
  fi
fi

# --- 内核调试信息归档 ---
# --- Kernel Debug Info Archive ---
if [ -f vmlinux ]; then
  mkdir -p "${WORKDIR}/release"
  DEBUGINFO_FILES=()
  for f in vmlinux vmlinux.unstripped System.map Module.symvers .config kos-debug; do
    [ -f "$f" ] && DEBUGINFO_FILES+=("$f")
  done

  if [ ${#DEBUGINFO_FILES[@]} -gt 0 ]; then
    tar -zcvf "${WORKDIR}/release/kernel-debuginfo-${KVER}.tar.gz" "${DEBUGINFO_FILES[@]}"
    echo "Kernel debuginfo archived: ${DEBUGINFO_FILES[*]}"
  else
    # 未找到可归档的调试信息文件
    # No debug info files found to archive
    echo "No debuginfo files found to archive"
  fi
fi

# --- 内核头文件打包 ---
# --- Kernel Headers Packaging ---
if [ -d kernel-headers ]; then
  mkdir -p "${WORKDIR}/release"
  tar -zcvf "${WORKDIR}/release/kernel-headers-${KVER}.tar.gz" kernel-headers
fi


# 生成 kernel-devel 开发包
# Generate kernel-devel development package
echo ">>> 生成 kernel-devel 包..."
cd "${WORKDIR}/${KERNEL_SRC_DIR:-.}"

DEVEL_DIR="${WORKDIR}/kernel-devel/${KVER}"

# 重新交叉编译 fixdep 和 modpost，确保它们是目标架构二进制
# Cross-recompile fixdep and modpost to ensure they are target-arch binaries
rm -f scripts/basic/fixdep
${CROSS_COMPILE}gcc -Wall \
  -Wmissing-prototypes -Wstrict-prototypes -O2 \
  -I scripts/include \
  -o scripts/basic/fixdep \
  scripts/basic/fixdep.c

# 重新交叉编译 modpost，动态收集源文件以兼容不同内核版本
# Cross-recompile modpost, dynamically collect source files for cross-version compatibility
rm -f scripts/mod/modpost
MODPOST_SRCS="scripts/mod/modpost.c"
for src in sumversion.c file2alias.c symsearch.c; do
  # 仅当源文件存在时才加入编译列表
  # Only add to compilation list if the source file exists
  [ -f "scripts/mod/${src}" ] && MODPOST_SRCS="${MODPOST_SRCS} scripts/mod/${src}"
done
${CROSS_COMPILE}gcc -Wall \
  -Wmissing-prototypes -Wstrict-prototypes -O2 \
  -I scripts/include \
  -o scripts/mod/modpost \
  ${MODPOST_SRCS}

# 清理旧的 devel 目录并重建结构
# Clean old devel directory and rebuild structure
rm -rf "${DEVEL_DIR}"
mkdir -p "${DEVEL_DIR}/arch/${ARCH}"
mkdir -p "${DEVEL_DIR}/include"
mkdir -p "${DEVEL_DIR}/scripts"
mkdir -p "${DEVEL_DIR}/tools"

# 复制顶层构建文件
# Copy top-level build files
cp -a Makefile "${DEVEL_DIR}/"
cp -a Kbuild "${DEVEL_DIR}/"

for f in Module.symvers System.map .config; do
  [ -f "$f" ] && cp -a "$f" "${DEVEL_DIR}/"
done

# 复制 scripts 目录（含编译好的 fixdep/modpost）
# Copy scripts directory (including compiled fixdep/modpost)
cp -a scripts "${DEVEL_DIR}/"

# 复制架构相关脚本
# Copy architecture-specific scripts
if [ -d "arch/${ARCH}/scripts" ]; then
  mkdir -p "${DEVEL_DIR}/arch/${ARCH}"
  cp -a "arch/${ARCH}/scripts" "${DEVEL_DIR}/arch/${ARCH}/"
fi

# 复制模块链接脚本
# Copy module linker script
if [ -f "scripts/module.lds" ]; then
  mkdir -p "${DEVEL_DIR}/scripts"
  cp -a scripts/module.lds "${DEVEL_DIR}/scripts/"
fi

# 复制头文件
# Copy header files
cp -a include "${DEVEL_DIR}/"
if [ -d "arch/${ARCH}/include" ]; then
  cp -a "arch/${ARCH}/include" "${DEVEL_DIR}/arch/${ARCH}/"
fi

# 复制架构 Makefile
# Copy architecture Makefile
if [ -f "arch/${ARCH}/Makefile" ]; then
  cp -a "arch/${ARCH}/Makefile" "${DEVEL_DIR}/arch/${ARCH}/"
fi

# 复制链接器脚本
# Copy linker scripts
for lds in arch/${ARCH}/*.lds; do
  [ -f "$lds" ] && cp -a "$lds" "${DEVEL_DIR}/arch/${ARCH}/"
done

# 复制设备树源文件（供外部模块引用 DTS 头文件）
# Copy device tree source files (for external modules to reference DTS headers)
if [ -d "arch/${ARCH}/boot/dts" ]; then
  mkdir -p "${DEVEL_DIR}/arch/${ARCH}/boot/dts"
  find "arch/${ARCH}/boot/dts" \
    \( -name '*.dts' -o -name '*.dtsi' -o -name '*.h' \
    -o -name 'Makefile' -o -name 'Kconfig' -o -name '*.overlay' \) \
    -type f \
    -exec cp --parents {} "${DEVEL_DIR}/" \;
fi

# ARM64 兼容层：复制 arm/include/asm（部分驱动需要）
# ARM64 compatibility layer: copy arm/include/asm (required by some drivers)
if [ "${ARCH}" = "arm64" ] && [ -d "arch/arm/include/asm" ]; then
  mkdir -p "${DEVEL_DIR}/arch/arm/include"
  cp -a arch/arm/include/asm "${DEVEL_DIR}/arch/arm/include/"
fi

# 递归复制所有 Makefile/Kconfig/Kbuild 文件（排除已复制目录和 .git）
# Recursively copy all Makefile/Kconfig/Kbuild files (excluding already copied dirs and .git)
find . -type f \( -name "Makefile" -o -name "Makefile.*" \
  -o -name "Kconfig" -o -name "Kconfig.*" \
  -o -name "Kbuild" -o -name "Kbuild.*" \) \
  ! -path './kernel-devel/*' \
  ! -path './.git/*' \
  -exec cp --parents {} "${DEVEL_DIR}/" \;

# 如果启用了 objtool，复制其二进制
# Copy objtool binary if enabled
if grep -q 'CONFIG_OBJTOOL=y' .config 2>/dev/null; then
  if [ -f tools/objtool/objtool ]; then
    mkdir -p "${DEVEL_DIR}/tools/objtool"
    cp -a tools/objtool/objtool "${DEVEL_DIR}/tools/objtool/"
  fi
fi

# 清理 devel 包中不需要的中间文件和临时文件
# Clean unnecessary intermediate and temporary files from devel package
find "${DEVEL_DIR}" -name ".*.cmd" -delete 2>/dev/null || true
find "${DEVEL_DIR}/scripts" -name "*.o" -delete 2>/dev/null || true
find "${DEVEL_DIR}" -name "*.dtb" -delete 2>/dev/null || true
find "${DEVEL_DIR}" -name "*.dtbo" -delete 2>/dev/null || true
find "${DEVEL_DIR}" -name ".tmp_*" -delete 2>/dev/null || true
find "${DEVEL_DIR}" -name "*.tmp" -delete 2>/dev/null || true
find "${DEVEL_DIR}" -type d -empty -delete 2>/dev/null || true

# 同步关键生成文件的时间戳，避免外部模块编译时触发不必要的重编
# Sync timestamps of key generated files to avoid unnecessary rebuilds of external modules
if [ -f "${DEVEL_DIR}/include/generated/utsrelease.h" ]; then
  touch -r Makefile "${DEVEL_DIR}/include/generated/utsrelease.h"
fi
if [ -f "${DEVEL_DIR}/include/generated/autoconf.h" ]; then
  touch -r .config "${DEVEL_DIR}/include/generated/autoconf.h"
fi
if [ -f "${DEVEL_DIR}/include/config/auto.conf" ]; then
  touch -r .config "${DEVEL_DIR}/include/config/auto.conf"
fi

# 兜底：确保 auto.conf 存在
# Fallback: ensure auto.conf exists
if [ ! -f "${DEVEL_DIR}/include/config/auto.conf" ]; then
  mkdir -p "${DEVEL_DIR}/include/config"
  cp .config "${DEVEL_DIR}/include/config/auto.conf"
fi

# 打包 kernel-devel
# Package kernel-devel
cd "${WORKDIR}"
tar -czf "${WORKDIR}/release/kernel-devel-${KVER}.tar.gz" kernel-devel

# 清理临时编译的工具二进制
# Clean temporarily compiled tool binaries
rm -f scripts/basic/fixdep
rm -f scripts/mod/modpost

# 展示最终产物列表
# Display final artifact list
echo ""
echo "=========================================="
echo "       构建产物清单 (${WORKDIR}/release)"
echo "       Build Artifacts (${WORKDIR}/release)"
echo "=========================================="
ls -alh "${WORKDIR}/release/"
echo ""
echo "Build completed successfully!"
exit 0

