TARGET     := Aurora
FIRM       := output/$(TARGET).firm

PREFIX     := arm-none-eabi-
CC         := $(PREFIX)gcc
AS         := $(PREFIX)gcc
LD         := $(PREFIX)gcc
OBJCOPY    := $(PREFIX)objcopy
FIRMTOOL   := python -m firmtool

SRC_DIR    := src
INC_DIR    := include
BUILD_DIR  := build
OUTPUT_DIR := output

C_SOURCES  := $(wildcard $(SRC_DIR)/*.c)
S_SOURCES  := $(wildcard $(SRC_DIR)/*.s)

ARM9_C_SRC := $(C_SOURCES)
ARM9_S_SRC := $(SRC_DIR)/start.s $(SRC_DIR)/arm9_jump.s
ARM11_S_SRC := $(SRC_DIR)/arm11_start.s $(SRC_DIR)/arm11_jump.s

ARM9_C_OBJ := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/arm9_%.o,$(ARM9_C_SRC))
ARM9_S_OBJ := $(patsubst $(SRC_DIR)/%.s,$(BUILD_DIR)/arm9_%.o,$(ARM9_S_SRC))
ARM11_S_OBJ := $(patsubst $(SRC_DIR)/%.s,$(BUILD_DIR)/arm11_%.o,$(ARM11_S_SRC))

ARM9_OBJS  := $(ARM9_S_OBJ) $(ARM9_C_OBJ)
ARM11_OBJS := $(ARM11_S_OBJ)

ARM9_ELF   := $(BUILD_DIR)/arm9.elf
ARM9_BIN   := $(BUILD_DIR)/arm9.bin
ARM11_ELF  := $(BUILD_DIR)/arm11.elf
ARM11_BIN  := $(BUILD_DIR)/arm11.bin

ARM9_LD    := arm9.ld
ARM11_LD   := arm11.ld

ARM9_ARCH  := -mcpu=arm946e-s -march=armv5te -marm
ARM9_CFLAGS := $(ARM9_ARCH) \
               -mthumb-interwork \
               -ffreestanding \
               -fno-builtin \
               -nostdlib \
               -nostartfiles \
               -Wall -Wextra \
               -g -O0 \
               -I$(INC_DIR)

ARM9_ASFLAGS := $(ARM9_ARCH) -mthumb-interwork
ARM9_LDFLAGS := -T $(ARM9_LD) -nostdlib -nostartfiles -Wl,--build-id=none -Wl,--gc-sections

ARM11_ARCH := -mcpu=mpcore -march=armv6k -marm
ARM11_ASFLAGS := $(ARM11_ARCH) -mthumb-interwork
ARM11_LDFLAGS := -T $(ARM11_LD) -nostdlib -nostartfiles -Wl,--build-id=none

ARM9_LOAD_ADDR  := 0x08006800
ARM9_ENTRY      := 0x08006800
ARM11_LOAD_ADDR := 0x1FF80000
ARM11_ENTRY     := 0x1FF80000

.PHONY: all clean rebuild dirs testpayload

all: dirs $(FIRM)
	@echo ""
	@echo "========================================"
	@echo "  Aurora OS build complete!"
	@echo "  Output: $(FIRM)"
	@echo "========================================"
	@echo ""

dirs:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(OUTPUT_DIR)
	@mkdir -p Files

$(FIRM): $(ARM9_BIN) $(ARM11_BIN)
	@echo [FIRM] Building $@
	$(FIRMTOOL) build $@ \
		-n $(ARM9_ENTRY) -e $(ARM11_ENTRY) \
		-D $(ARM9_BIN) $(ARM11_BIN) \
		-A $(ARM9_LOAD_ADDR) $(ARM11_LOAD_ADDR) \
		-C NDMA XDMA

$(ARM9_ELF): $(ARM9_OBJS) $(ARM9_LD)
	@echo [LD9 ] Linking $@
	$(LD) $(ARM9_LDFLAGS) $(ARM9_OBJS) -o $@ -lgcc

$(ARM9_BIN): $(ARM9_ELF)
	@echo [BIN9] Creating $@
	$(OBJCOPY) -O binary $< $@

$(ARM11_ELF): $(ARM11_OBJS) $(ARM11_LD)
	@echo [LD11] Linking $@
	$(LD) $(ARM11_LDFLAGS) $(ARM11_OBJS) -o $@ -lgcc

$(ARM11_BIN): $(ARM11_ELF)
	@echo [BIN11] Creating $@
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/arm9_%.o: $(SRC_DIR)/%.c $(wildcard $(INC_DIR)/*.h)
	@echo [CC9 ] Compiling $<
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/arm9_%.o: $(SRC_DIR)/%.s
	@echo [AS9 ] Assembling $<
	$(AS) $(ARM9_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/arm11_%.o: $(SRC_DIR)/%.s
	@echo [AS11] Assembling $<
	$(AS) $(ARM11_ASFLAGS) -c $< -o $@

clean:
	@echo [CLEAN] Removing build artifacts...
	@rm -rf $(BUILD_DIR)
	@rm -rf $(OUTPUT_DIR)
	@echo [CLEAN] Done.

rebuild: clean all

# Phase 7 throwaway test payload -> AURORAOS.BIN at the repo root (kept out of
# output/ so `make clean` doesn't wipe it). Copy the result to the SD card root.
TESTPAYLOAD_DIR := tools/testpayload

testpayload: dirs
	@echo [AS9 ] Assembling $(TESTPAYLOAD_DIR)/payload.s
	$(AS) $(ARM9_ARCH) -mthumb-interwork -c $(TESTPAYLOAD_DIR)/payload.s -o $(BUILD_DIR)/payload.o
	@echo [LD9 ] Linking payload
	$(LD) -T $(TESTPAYLOAD_DIR)/payload.ld -nostdlib -nostartfiles -Wl,--build-id=none $(BUILD_DIR)/payload.o -o $(BUILD_DIR)/payload.elf
	$(OBJCOPY) -O binary $(BUILD_DIR)/payload.elf $(BUILD_DIR)/payload.bin
	@echo [PACK] Packing AURORAOS.BIN
	python tools/aos_pack.py pack $(BUILD_DIR)/payload.bin -o AURORAOS.BIN --arm9-load 0x22000000
	@echo "  -> AURORAOS.BIN (copy this to the SD card root)"