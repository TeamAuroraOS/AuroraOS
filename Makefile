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
               -g -O2 \
               -I$(INC_DIR)

ARM9_ASFLAGS := $(ARM9_ARCH) -mthumb-interwork
ARM9_LDFLAGS := -T $(ARM9_LD) -nostdlib -nostartfiles -Wl,--build-id=none -Wl,--gc-sections

ARM11_ARCH := -mcpu=mpcore -march=armv6k -marm
ARM11_ASFLAGS := $(ARM11_ARCH) -mthumb-interwork
ARM11_CFLAGS := $(ARM11_ARCH) \
                -mthumb-interwork \
                -ffreestanding \
                -fno-builtin \
                -nostdlib \
                -nostartfiles \
                -Wall -Wextra \
                -g -O2 \
                -I$(INC_DIR)
ARM11_LDFLAGS := -T $(ARM11_LD) -nostdlib -nostartfiles -Wl,--build-id=none

ARM9_LOAD_ADDR  := 0x08006800
ARM9_ENTRY      := 0x08006800
ARM11_LOAD_ADDR := 0x1FF80000
ARM11_ENTRY     := 0x1FF80000

.PHONY: all clean rebuild dirs os greentest

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

OS_DIR  := src/os
OS_LD   := $(OS_DIR)/os.ld
OS_OBJS := $(BUILD_DIR)/os_start.o $(BUILD_DIR)/os_main.o \
           $(BUILD_DIR)/os_setup.o $(BUILD_DIR)/os_audio9.o \
           $(BUILD_DIR)/os_crash.o $(BUILD_DIR)/os_crashasm.o \
           $(BUILD_DIR)/os_screen.o $(BUILD_DIR)/os_i2c.o \
           $(BUILD_DIR)/os_string.o $(BUILD_DIR)/os_container.o \
           $(BUILD_DIR)/os_sdmmc.o $(BUILD_DIR)/os_diskio.o \
           $(BUILD_DIR)/os_ff.o $(BUILD_DIR)/os_ffunicode.o \
           $(BUILD_DIR)/os_launch.o

AUDIO11_OBJS := $(BUILD_DIR)/audio11_start.o $(BUILD_DIR)/audio11.o
AUDIO11_BIN  := $(BUILD_DIR)/audio11.bin
AUDIO11_BLOB := $(BUILD_DIR)/audio11_blob.h

$(BUILD_DIR)/os_start.o: $(OS_DIR)/os_start.s | dirs
	@echo [AS9 ] Assembling $<
	$(AS) $(ARM9_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/os_main.o: $(OS_DIR)/os_main.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $<
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_setup.o: $(OS_DIR)/os_setup.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $<
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_crash.o: $(OS_DIR)/crash.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $<
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_crashasm.o: $(OS_DIR)/crash.s | dirs
	@echo [AS9 ] Assembling $<
	$(AS) $(ARM9_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/audio11_start.o: $(OS_DIR)/audio11_start.s | dirs
	@echo [AS11] Assembling $<
	$(AS) $(ARM11_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/audio11.o: $(OS_DIR)/audio11.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC11] Compiling $<
	$(CC) $(ARM11_CFLAGS) -c $< -o $@

$(BUILD_DIR)/audio11.elf: $(AUDIO11_OBJS) $(OS_DIR)/audio11.ld
	@echo [LD11] Linking ARM11 audio core
	$(LD) -T $(OS_DIR)/audio11.ld -nostdlib -nostartfiles -Wl,--build-id=none -Wl,--gc-sections $(AUDIO11_OBJS) -o $@ -lgcc

$(AUDIO11_BIN): $(BUILD_DIR)/audio11.elf
	@echo [BIN11] Creating $@
	$(OBJCOPY) -O binary $< $@

$(AUDIO11_BLOB): $(AUDIO11_BIN)
	@echo [BLOB] Embedding $< '->' $@
	python tools/bin2c.py $< audio11_bin > $@

$(BUILD_DIR)/os_audio9.o: $(OS_DIR)/audio9.c $(wildcard $(INC_DIR)/*.h) $(AUDIO11_BLOB) | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -I$(BUILD_DIR) -c $< -o $@

$(BUILD_DIR)/os_screen.o: $(SRC_DIR)/screen.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_i2c.o: $(SRC_DIR)/i2c.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_string.o: $(SRC_DIR)/string.c | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_container.o: $(SRC_DIR)/container.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_sdmmc.o: $(SRC_DIR)/sdmmc.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_diskio.o: $(SRC_DIR)/diskio.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_ff.o: $(SRC_DIR)/ff.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_ffunicode.o: $(SRC_DIR)/ffunicode.c $(wildcard $(INC_DIR)/*.h) | dirs
	@echo [CC9 ] Compiling $< '(for OS)'
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

$(BUILD_DIR)/os_launch.o: $(OS_DIR)/os_launch.s | dirs
	@echo [AS9 ] Assembling $<
	$(AS) $(ARM9_ASFLAGS) -c $< -o $@

os: $(OS_OBJS)
	@echo [LD9 ] Linking AuroraOS payload
	$(LD) -T $(OS_LD) -nostdlib -nostartfiles -Wl,--build-id=none -Wl,--gc-sections $(OS_OBJS) -o $(BUILD_DIR)/os.elf -lgcc
	$(OBJCOPY) -O binary $(BUILD_DIR)/os.elf $(BUILD_DIR)/os.bin
	@echo [PACK] Packing AURORAOS.BIN
	python tools/aos_pack.py pack $(BUILD_DIR)/os.bin -o AURORAOS.BIN --arm9-load 0x22000000
	@echo "  -> AURORAOS.BIN (copy this to the SD card root)"

greentest: dirs
	$(AS) $(ARM9_ASFLAGS) -c tools/testpayload/payload.s -o $(BUILD_DIR)/payload.o
	$(LD) -T tools/testpayload/payload.ld -nostdlib -nostartfiles -Wl,--build-id=none $(BUILD_DIR)/payload.o -o $(BUILD_DIR)/payload.elf
	$(OBJCOPY) -O binary $(BUILD_DIR)/payload.elf $(BUILD_DIR)/payload.bin
	python tools/aos_pack.py pack $(BUILD_DIR)/payload.bin -o greentest.bin --arm9-load 0x22000000
	@echo "  -> greentest.bin"