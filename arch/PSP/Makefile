TARGET = AuroraOS
OBJS = display.o GPIO_write.o virt_addr.o uart.o

INCDIR = /arch/PSP/
CFLAGS = -O0 -Wall -g -ffreestanding 
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = 
LIBDIR =

EXTRA_TARGETS = EBOOT.PBP

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
