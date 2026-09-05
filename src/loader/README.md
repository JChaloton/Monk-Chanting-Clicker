# Loader firmware (stub)

A one-off firmware, flashed instead of the player, that receives the audio blob over
UART (PD5 TX / PD6 RX + any USB-serial dongle) and writes it into U3 page by page.
After the load, flash the real player firmware back.

This is route C in `docs/audio-pipeline.md`. Routes A (CH341A programmer with the flash
on its DIP adapter) and B (DediProg DP3000 at work) need no firmware at all and are
easier while the chip is still socketed — write this only when the flash is soldered
down on a real PCB.

Protocol (keep it dumb):

```
host → mcu : 'C' 'H' 'N' 'T'  <len u32 LE>       start; mcu erases enough sectors
mcu  → host: 'K'
host → mcu : 256 bytes                           one page
mcu  → host: 'K'                                 written and verified by read-back
...
mcu  → host: 'D' <crc32 u32 LE>                  done — compare with the tool's CRC
```

Reuses `../spiflash.c`. Erase (`20h` per 4 KB) before programming, `06h` write-enable
before each page, poll `05h` BUSY after each write.

TODO: `loader.c`, and a host-side sender (add it to `tools/`).
