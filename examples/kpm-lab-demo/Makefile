# kpm-lab-demo — 用 Termux 自带 clang 直接构建 .kpm (无需 NDK)
#
# 用法:
#   make KP_DIR=/path/to/SukiSU_KernelPatch_patch
#
# 本机 (Termux) 已验证: clang 21 (aarch64-linux-android target)

KP_DIR ?= /data/data/com.termux/files/home/KP-SUKISU
CC ?= clang

INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include
INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))

CFLAGS := $(INCLUDE_FLAGS) -Wall -O2 -fno-PIC -fno-pie -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-unwind-tables -U_FORTIFY_SOURCE \
          -fno-common -fvisibility=hidden

TARGET := kpm_lab.kpm
OBJ := kpm_lab.o

all: $(TARGET)

$(TARGET): kpm_lab.c kpm_lab.lds
	$(CC) $(CFLAGS) -Tkpm_lab.lds -c -o $(OBJ) kpm_lab.c
	$(CC) -r -Tkpm_lab.lds -o $@ $(OBJ)
	rm -f $(OBJ)

clean:
	rm -f *.kpm *.o

.PHONY: all clean
