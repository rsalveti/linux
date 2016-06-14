#!/bin/sh
# Bootimage building helper for db410c
# Author: Pingbo Wen <pingbo.wen@linaro.org>

BUILD_KERNEL_MODULE=0
CPU_NUMS=`cat /proc/cpuinfo | grep processor | wc -l`

OUT_DIR=${OUT:=./out}
MKBOOTIMG=${MKBOOTIMG:=`which mkbootimg`}
DTBTOOL=${DTBTOOL:=`which dtbTool`}
RAMDISK_PATH=${RAMDISK:=${OUT}/ramdisk.img}
KTP=${CROSS_COMPILE:=aarch64-linux-android-}

if [ ! -e "$RAMDISK_PATH" ]; then
	echo "File ${RAMDISK_PATH} not found !!!"
	echo "Please download it in below url:"
	echo "\thttps://builds.96boards.org/releases/reference-platform/aosp/dragonboard410c/16.03/ramdisk.img.xz"
fi

if [ ! -x "$MKBOOTIMG" ] || [ ! -x "$DTBTOOL" ]; then
	echo "mkbootimg or dtbTool is not found !!!"
	echo "Please download it in below url:"
	echo "\tgit://codeaurora.org/quic/kernel/skales"
fi

if [ -e ${KTP}ld.bfd ]; then
	LD=${KTP}ld.bfd;
else
	LD=${KTP}ld;
fi

scripts/kconfig/merge_config.sh -m arch/arm64/configs/defconfig kernel/configs/distro.config kernel/configs/android.config

mv .config ${OUT_DIR}/.config
make -j1 O=${OUT_DIR} ARCH=arm64 KCONFIG_ALLCONFIG=${OUT_DIR}/.config alldefconfig

[ $? -ne 0 ] && exit 1

# build kernel
make O=${OUT_DIR} ARCH=arm64 CROSS_COMPILE="${KTP}" KCFLAGS="-fno-pic" LD="${LD}" -j${CPU_NUMS} Image

[ $? -ne 0 ] && exit 1

# build kernel modules
if [ $BUILD_KERNEL_MODULE -eq 1 ]; then
	mkdir -pv ${OUT_DIR}/modules_for_android
	make O=${OUT_DIR} ARCH=arm64 CROSS_COMPILE="${KTP}" LD="${LD}" -j${CPU_NUMS} EXTRA_CFLAGS="-fno-pic" modules
	make O=${OUT_DIR} ARCH=arm64 CROSS_COMPILE="${KTP}" KCFLAGS="-fno-pic" LD="${LD}" -j${CPU_NUMS} modules_install INSTALL_MOD_PATH=${OUT_DIR}/modules_for_android
	echo "You should copy ${OUT_DIR}/modules_for_android to /system/modules"
fi

# build dtbs
make O=${OUT_DIR} ARCH=arm64 CROSS_COMPILE="${KTP}" dtbs

mkdir -pv ${OUT_DIR}/dtbs
cp -f ${OUT_DIR}/arch/arm64/boot/dts/qcom/apq8016-sbc.dtb ${OUT_DIR}/dtbs
cp -f ${OUT_DIR}/arch/arm64/boot/dts/qcom/msm8916-mtp.dtb ${OUT_DIR}/dtbs

${DTBTOOL} -o ${OUT_DIR}/dt.img -s 2048 ${OUT_DIR}/dtbs

[ $? -ne 0 ] && exit 1

# create bootimage
${MKBOOTIMG} --kernel ${OUT_DIR}/arch/arm64/boot/Image --ramdisk ${RAMDISK_PATH} --output ${OUT_DIR}/boot-db410c.img --dt ${OUT_DIR}/dt.img --pagesize "2048" --base "0x80000000" --cmdline "console=ttyMSM0,115200n8 debug earlyprintk=serial,0x16640000,115200 verbose androidboot.selinux=permissive androidboot.hardware=db410c user_debug=31"

[ $? -eq 0 ] && echo "\n*** Build ${OUT_DIR}/boot-db410c.img successfully ***"
