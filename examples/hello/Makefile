# SPDX-License-Identifier: MIT
# Builds HELLO.ELF with the SDK only. Run `make` here, or `make examples` at the repository root.
SDK ?= ../../sdk
TOOLCHAIN ?= /Applications/ArmGNUToolchain/15.3.rel2/arm-none-eabi/bin/arm-none-eabi-
CC := $(TOOLCHAIN)gcc
# ARM1176, ARM state, soft float, no libc; text at 0x08000000 from the SDK link map.
CFLAGS := -mcpu=arm1176jzf-s -marm -mfloat-abi=soft -std=c11 -Os -ffreestanding -fno-builtin -nostdlib \
  -Wall -Wextra -Werror -I$(SDK)/include -T$(SDK)/lib/sandbox.ld -Wl,-z,noexecstack -Wl,--build-id=none \
  -Wl,-z,max-page-size=4096
HELLO.ELF: main.c Makefile $(SDK)/lib/crt0.S $(SDK)/lib/mem.c $(SDK)/lib/sandbox.ld $(wildcard $(SDK)/include/sandbox/*.h)
	$(CC) $(CFLAGS) $(SDK)/lib/crt0.S $(SDK)/lib/mem.c main.c -lgcc -o $@
clean:
	rm -f HELLO.ELF
