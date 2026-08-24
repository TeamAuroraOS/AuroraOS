#-----------------------------------------------------------------------
# Aurora OS - Build System
#
# Compiles the Aurora custom OS into a .firm payload for the Nintendo 3DS.
#
# Targets:
#   all       - Build Aurora.firm (default)
#   clean     - Remove all build artifacts
#   rebuild   - Clean and rebuild
#
# Requirements:
#   - devkitARM (arm-none-eabi-gcc)
#   - firmtool (pip install firmtool)
#-----------------------------------------------------------------------

# Output
TARGET     := Aurora
FIRM       := output/$(TARGET).firm

# Toolchain
PREFIX     := arm-none-eabi-
CC         := $(PREFIX)gcc
AS         := $(PREFIX)gcc
LD         := $(PREFIX)gcc
OBJCOPY    := $(PREFIX)objcopy

# Firmtool - invoke as a Python module so this doesn't depend on
# firmtool's console-script entry point being on PATH
FIRMTOOL   := python -m firmtool

# Directories
SRC_DIR    := src
INC_DIR    := include
BUILD_DIR  := build
OUTPUT_DIR := output

# Source files
C_SOURCES  := $(wildcard $(SRC_DIR)/*.c)
S_SOURCES  := $(wildcard $(SRC_DIR)/*.s)

# ARM9 Sources (main payload: all C files + arm9 startup)
ARM9_C_SRC := $(C_SOURCES)
ARM9_S_SRC := $(SRC_DIR)/start.s

# ARM11 Sources (spinloop only)
ARM11_S_SRC := $(SRC_DIR)/arm11_start.s

# Object files
ARM9_C_OBJ := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/arm9_%.o,$(ARM9_C_SRC))
ARM9_S_OBJ := $(patsubst $(SRC_DIR)/%.s,$(BUILD_DIR)/arm9_%.o,$(ARM9_S_SRC))
ARM11_S_OBJ := $(patsubst $(SRC_DIR)/%.s,$(BUILD_DIR)/arm11_%.o,$(ARM11_S_SRC))

ARM9_OBJS  := $(ARM9_S_OBJ) $(ARM9_C_OBJ)
ARM11_OBJS := $(ARM11_S_OBJ)

# ELF and BIN intermediates
ARM9_ELF   := $(BUILD_DIR)/arm9.elf
ARM9_BIN   := $(BUILD_DIR)/arm9.bin
ARM11_ELF  := $(BUILD_DIR)/arm11.elf
ARM11_BIN  := $(BUILD_DIR)/arm11.bin

# Linker scripts
ARM9_LD    := arm9.ld
ARM11_LD   := arm11.ld

#-----------------------------------------------------------------------
# Compiler / Assembler / Linker Flags
#-----------------------------------------------------------------------

# ARM9 is an ARM946E-S core
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

# ARM11 is an ARM11 MPCore (ARMv6K)
ARM11_ARCH := -mcpu=mpcore -march=armv6k -marm
ARM11_ASFLAGS := $(ARM11_ARCH) -mthumb-interwork
ARM11_LDFLAGS := -T $(ARM11_LD) -nostdlib -nostartfiles -Wl,--build-id=none

# Memory addresses
ARM9_LOAD_ADDR  := 0x08006800
ARM9_ENTRY      := 0x08006800
ARM11_LOAD_ADDR := 0x1FF80000
ARM11_ENTRY     := 0x1FF80000

#-----------------------------------------------------------------------
# Build Rules
#-----------------------------------------------------------------------

.PHONY: all clean rebuild dirs

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

# Build the FIRM file from the ARM9 and ARM11 binaries
$(FIRM): $(ARM9_BIN) $(ARM11_BIN)
	@echo [FIRM] Building $@
	$(FIRMTOOL) build $@ \
		-n $(ARM9_ENTRY) -e $(ARM11_ENTRY) \
		-D $(ARM9_BIN) $(ARM11_BIN) \
		-A $(ARM9_LOAD_ADDR) $(ARM11_LOAD_ADDR) \
		-C NDMA XDMA

# ARM9: Link ELF
$(ARM9_ELF): $(ARM9_OBJS) $(ARM9_LD)
	@echo [LD9 ] Linking $@
	$(LD) $(ARM9_LDFLAGS) $(ARM9_OBJS) -o $@ -lgcc

# ARM9: Convert ELF to raw binary
$(ARM9_BIN): $(ARM9_ELF)
	@echo [BIN9] Creating $@
	$(OBJCOPY) -O binary $< $@

# ARM11: Link ELF
$(ARM11_ELF): $(ARM11_OBJS) $(ARM11_LD)
	@echo [LD11] Linking $@
	$(LD) $(ARM11_LDFLAGS) $(ARM11_OBJS) -o $@ -lgcc

# ARM11: Convert ELF to raw binary
$(ARM11_BIN): $(ARM11_ELF)
	@echo [BIN11] Creating $@
	$(OBJCOPY) -O binary $< $@

#-----------------------------------------------------------------------
# Compilation Rules
#-----------------------------------------------------------------------

# ARM9 C files
$(BUILD_DIR)/arm9_%.o: $(SRC_DIR)/%.c $(wildcard $(INC_DIR)/*.h)
	@echo [CC9 ] Compiling $<
	$(CC) $(ARM9_CFLAGS) -c $< -o $@

# ARM9 Assembly files
$(BUILD_DIR)/arm9_%.o: $(SRC_DIR)/%.s
	@echo [AS9 ] Assembling $<
	$(AS) $(ARM9_ASFLAGS) -c $< -o $@

# ARM11 Assembly files
$(BUILD_DIR)/arm11_%.o: $(SRC_DIR)/%.s
	@echo [AS11] Assembling $<
	$(AS) $(ARM11_ASFLAGS) -c $< -o $@

#-----------------------------------------------------------------------
# Clean
#-----------------------------------------------------------------------

clean:
	@echo [CLEAN] Removing build artifacts...
	@rm -rf $(BUILD_DIR)
	@rm -rf $(OUTPUT_DIR)
	@echo [CLEAN] Done.

rebuild: clean all