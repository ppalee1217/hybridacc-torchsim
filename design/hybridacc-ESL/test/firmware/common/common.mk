# Common Makefile for HybridAcc CoreMcu firmware tests
#
# Usage: include from per-test Makefile after setting:
#   TARGET  — output ELF name (e.g. test_alu.elf)
#   SRCS    — list of source files (e.g. crt0.S main.c)
#   EXTRA_CFLAGS — optional extra C flags

CROSS   ?= riscv32-unknown-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

COMMON  := ../common

CFLAGS  := -march=rv32im_zicsr -mabi=ilp32 -Os -nostdlib -ffreestanding -Wall \
           -I$(COMMON) $(EXTRA_CFLAGS)
LDFLAGS := -T link.ld -nostdlib -Wl,--gc-sections

OBJS    := $(patsubst %.S,%.o,$(patsubst %.c,%.o,$(SRCS)))

all: $(TARGET) $(TARGET:.elf=.dis) $(TARGET:.elf=.mem)
	$(SIZE) $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(TARGET:.elf=.dis): $(TARGET)
	$(OBJDUMP) -d $< > $@

$(TARGET:.elf=.mem): $(TARGET)
	$(OBJCOPY) -O verilog $< $@

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: $(COMMON)/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: $(COMMON)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) *.dis *.mem

.PHONY: all clean
