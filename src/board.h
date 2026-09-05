/*
 * board.h — single source of pin truth for the chant clicker.
 *
 * If a pin assignment changes here, change docs/hardware.md in the same commit.
 * Never hard-code a port/pin anywhere else.
 *
 * Board: CH32V003F4P6 (TSSOP-20) + PAM8302A + FM25Q08B, one 3 V rail from a CR123A.
 */

#ifndef BOARD_H
#define BOARD_H

/* ---- pins (see docs/hardware.md) ------------------------------------------ */

/* Audio PWM out — TIM1 CH1. This pin is fixed by the timer peripheral. */
#define PIN_AUDIO_PORT      GPIOD
#define PIN_AUDIO_NUM       2       /* PD2, U1 pin 19 -> R1 */

/* Switch in — active LOW, internal pull-up, EXTI wake source. */
#define PIN_SW_PORT         GPIOD
#define PIN_SW_NUM          3       /* PD3, U1 pin 20 <- SW1 -> GND */

/* Amp enable — HIGH = play. U2 SD is active LOW; R4 100k holds it off. */
#define PIN_AMP_EN_PORT     GPIOC
#define PIN_AMP_EN_NUM      3       /* PC3, U1 pin 13 -> U2 pin 4 */

/* SPI flash. SCK/MOSI/MISO are fixed by the SPI peripheral; CS is any GPIO. */
#define PIN_FLASH_CS_PORT   GPIOC
#define PIN_FLASH_CS_NUM    4       /* PC4 -> U3 pin 1 CS#  */
#define PIN_FLASH_SCK_NUM   5       /* PC5 -> U3 pin 6 CLK  */
#define PIN_FLASH_MOSI_NUM  6       /* PC6 -> U3 pin 5 DI   */
#define PIN_FLASH_MISO_NUM  7       /* PC7 <- U3 pin 2 DO   */

/* PD1 is SWIO (programming). Never reconfigure it as GPIO — you can lock the
 * WCH-LinkE out and need the power-cycle unlock. PD5/PD6 are UART TX/RX, free
 * for debug printf and for the audio loader in src/loader/. */

/* ---- audio timing (see docs/firmware-plan.md) ----------------------------- */

#define AUDIO_SAMPLE_RATE_HZ    8000u   /* one sample every 125 us */
#define AUDIO_PWM_PERIOD        256u    /* 8-bit; 48 MHz / 256 = 187.5 kHz carrier */
#define AUDIO_IDLE_DUTY         128u    /* mid-scale; ramp to/from this to avoid pops */
#define AUDIO_RAMP_MS           5u

#define AUDIO_BUF_BYTES         256u    /* one flash page */
#define AUDIO_BUF_COUNT         2       /* double buffer: ISR plays A while main fills B */

/* ---- input ---------------------------------------------------------------- */

#define SW_DEBOUNCE_MS          20u

#endif /* BOARD_H */
