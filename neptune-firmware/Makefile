PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
AS      = $(PREFIX)gcc
LD      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -Wall -Werror
CFLAGS += -ffreestanding -nostdlib -fno-builtin -fdata-sections -ffunction-sections
CFLAGS += -Iinclude

ASFLAGS = -mcpu=cortex-m4 -mthumb

LDFLAGS = -mcpu=cortex-m4 -mthumb -T linker.ld -nostdlib -Wl,--gc-sections -Wl,-Map=neptune.map

C_SRCS = $(wildcard src/*.c)
S_SRCS = $(wildcard src/*.s)
OBJS   = $(C_SRCS:.c=.o) $(S_SRCS:.s=.o)

all: neptune.bin

neptune.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

neptune.bin: neptune.elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.s
	$(AS) $(ASFLAGS) -c -o $@ $<

syx: neptune.bin
	python3 ../tools/mk4_syx_tool.py encode neptune.bin -o neptune.syx

clean:
	rm -f src/*.o neptune.elf neptune.bin neptune.map neptune.syx

.PHONY: all syx clean
