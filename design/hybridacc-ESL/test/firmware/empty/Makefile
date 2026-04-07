CROSS   ?= riscv32-unknown-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

CFLAGS  := -march=rv32im_zicsr -mabi=ilp32 -Os -nostdlib -ffreestanding -Wall
LDFLAGS := -T link.ld -nostdlib -Wl,--gc-sections

TARGET  := empty.elf

SRCS    := crt0.S main.c
OBJS    := $(patsubst %.S,%.o,$(patsubst %.c,%.o,$(SRCS)))

all: $(TARGET)
	$(OBJDUMP) -d $< > $(TARGET:.elf=.dis)
	$(SIZE) $<

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) *.dis

.PHONY: all clean
