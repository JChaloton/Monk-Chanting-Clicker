/* funconfig.h — ch32fun build configuration for chant-clicker.
 *
 * ch32fun.h includes "funconfig.h" unconditionally and the framework ships no
 * default, so every project must provide one. It sits in src/ because both
 * build systems already put src/ on the include path (-Isrc), which is what
 * lets ch32fun.c find it while compiling inside the framework package.
 *
 * All three firmwares (player, loader, LED demo) share this file.
 */
#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

#define CH32V003 1

/* 48 MHz off the internal HSI through the 2x PLL. The audio path depends on
 * this number: TIM1 at 48 MHz / 256 counts = 187.5 kHz PWM carrier, and the
 * 8 kHz sample tick is derived from the same clock. Changing it moves both. */
#define FUNCONF_USE_PLL             1
#define FUNCONF_USE_HSI             1
#define FUNCONF_SYSTEM_CORE_CLOCK   48000000

/* Where printf goes. Default is SWIO (PD1), read with `minichlink -T`.
 *
 * These are #ifndef-guarded so a single environment can override them from the
 * command line without changing this shared file - platformio.ini's [env:player]
 * passes -DFUNCONF_USE_UARTPRINTF=1 -DFUNCONF_USE_DEBUGPRINTF=0 to print on PD5
 * at 115200 instead, which any USB-serial adapter can read. A -D on the command
 * line is seen before this header, so the guards let it win.
 *
 * Only the player may do this. loader.c configures USART1 itself at register
 * level for the audio wire protocol, and ch32fun's UART printf would configure
 * the same peripheral behind its back - two owners of one USART is a silent
 * corruption bug, not a link error. So never set UARTPRINTF globally here. */
#ifndef FUNCONF_USE_DEBUGPRINTF
#define FUNCONF_USE_DEBUGPRINTF     1
#endif
#ifndef FUNCONF_USE_UARTPRINTF
#define FUNCONF_USE_UARTPRINTF      0
#endif

/* Matches monitor_speed in platformio.ini and the loader's own baud. */
#define FUNCONF_UART_PRINTF_BAUD    115200

/* Divide HCLK by 8 for SysTick (6 MHz), the ch32fun default for the '003. */
#define FUNCONF_SYSTICK_USE_HCLK    0

#endif
