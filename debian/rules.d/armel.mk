build_arch	= arm
header_arch	= arm
asm_link	= arm
defconfig	= defconfig
flavours	= orion5x iop32x ixp4xx versatile mv78xx0
build_image	= zImage
kernel_file	= arch/$(build_arch)/boot/zImage
install_file	= vmlinuz
no_dumpfile = true

loader		= grub
