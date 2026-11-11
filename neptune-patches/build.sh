#!/bin/sh
# Assemble the Neptune firmware patch code. Run before tools/build_neptune_fw.py whenever a .S file changes.
# Outputs (*.o, *.elf, *.bin) are build products; build_neptune_fw.py reads the .bin blobs and ccbtn.elf symbols.
set -e
cd "$(dirname "$0")"
CC="arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb"
$CC dbg.S -o dbg.o && arm-none-eabi-ld -T dbg.ld dbg.o -o dbg.elf
arm-none-eabi-objcopy -O binary -j .handler dbg.elf handler.bin
arm-none-eabi-objcopy -O binary -j .cave dbg.elf cave.bin
$CC ccbtn.S -o ccbtn.o && arm-none-eabi-ld -T ccbtn.ld ccbtn.o -o ccbtn.elf
for s in h43 snd pat pat2; do arm-none-eabi-objcopy -O binary -j .$s ccbtn.elf $s.bin; done
ls -l *.bin | awk '{print $5, $9}'
