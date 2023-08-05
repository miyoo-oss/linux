#!/bin/bash

./ms_builtin_dtb_update.py arch/arm/boot/Image arch/arm/boot/dts/sigmastar/mstar-infinity2m-ssd202d-miyoo-mini.dtb
./ms_bin_option_update_int.py arch/arm/boot/Image  '#IMG_SZ#' $(stat -c %s arch/arm/boot/Image)
xz -z -k -f arch/arm/boot/Image;
mkimage -A arm -O linux -T kernel -C lzma -a 0x20008000 -e 0x20008000 -n "Linux kernel uImage" -d arch/arm/boot/Image.xz arch/arm/boot/uImage.xz
