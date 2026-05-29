#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2020 Western Digital Corporation or its affiliates.
#
# Authors:
#   Anup Patel <anup.patel@wdc.com>
#

# Compiler flags
platform-cppflags-y =
platform-cflags-y =
platform-asflags-y =
platform-ldflags-y = -L../sm/target/riscv64gc-unknown-linux-gnu/debug -lvela	#vela patch

# Command for platform specific "make run"
platform-runcmd = qemu-system-riscv$(PLATFORM_RISCV_XLEN) -M virt -m 256M \
  -nographic -bios $(build_dir)/platform/generic/firmware/fw_payload.bin

# Objects to build
platform-objs-y += platform.o
platform-objs-y += platform_override_modules.carray.o

# Blobs to build
#FW_TEXT_START=0x80000000					#vela patch
FW_ENC_SIZE = 0x0						#vela patch
FW_DYNAMIC=y
FW_JUMP=y
ifeq ($(PLATFORM_RISCV_XLEN), 32)
  # This needs to be 4MB aligned for 32-bit system
  FW_JUMP_OFFSET=0x400000
else
  # This needs to be 2MB aligned for 64-bit system
  FW_JUMP_OFFSET=0x200000
endif
FW_JUMP_FDT_OFFSET=0x2200000
FW_PAYLOAD=y
ifeq ($(PLATFORM_RISCV_XLEN), 32)
  # This needs to be 4MB aligned for 32-bit system
  FW_PAYLOAD_OFFSET=0x400000
else
  # This needs to be 2MB aligned for 64-bit system
  FW_PAYLOAD_OFFSET=0x200000
endif
FW_PAYLOAD_FDT_OFFSET=$(FW_JUMP_FDT_OFFSET)

#vela patch start
ifdef PLATFORM
  platform-genflags-y += "-DTARGET_PLATFORM_HEADER=\"platform/$(PLATFORM)/platform.h\""
else
        PLATFORM = "generic"
  platform-genflags-y += "-DTARGET_PLATFORM_HEADER=\"platform/generic/platform.h\""
endif

ifeq ($(PLATFORM), wgrocket)
        carray-platform_override_modules-y += wgrocket
        platform-objs-y += wgrocket.o
#else
#        carray-platform_override_modules-y += sifive_fu540
#        platform-objs-y += sifive_fu540.o
endif
platform-objs-y += vyond.o
platform-objs-y += mprv.o

$(info FW_JUMP_ADDR $(FW_JUMP_ADDR))
$(info FW_JUMP_FDT_ADDR $(FW_JUMP_FDT_ADDR))
#vela patch end
