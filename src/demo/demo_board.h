/*
 * demo_board.h — dev-board-only pin overrides for the LED wave demo.
 *
 * board.h stays the single source of pin truth for the real product; this file
 * only says which of those pins the demo lights an LED on, and lets you point
 * it at a different pin if your dev board has an on-board LED you would rather
 * use. Nothing here is compiled into the player firmware.
 */

#ifndef DEMO_BOARD_H
#define DEMO_BOARD_H

#include "board.h"

/* Which pin drives the LED.
 *
 * Default is PD2 — the real audio pin (TIM1 CH1). That is deliberate: the demo
 * then exercises exactly the timer, pin and duty path the product will use, so
 * a working demo is real evidence about the product, not just about an LED.
 * TIM1 CH1 lives on PD2 and nowhere else, so any other pin means giving up
 * hardware PWM. If you want to use an on-board LED instead, check your board's
 * schematic for its pin and set these — but keep PD2 if you can.
 */
#define PIN_LED_PORT        PIN_AUDIO_PORT
#define PIN_LED_NUM         PIN_AUDIO_NUM
#define PIN_LED_RCC         RCC_APB2Periph_GPIOD

/* Most dev-board LEDs are wired anode-to-rail and sink through the pin, so the
 * pin is active LOW. An LED you add yourself from the pin through a resistor to
 * GND is active HIGH. Set this wrong and the chant plays inside-out: bright in
 * the silences, dark on the loud parts. */
#define LED_ACTIVE_LOW      0

/* ---- how the waveform becomes brightness --------------------------------- */

/* Envelope follower, PCM8 mode only. Attack is instant; release is a one-pole
 * decay of 1/2^LED_RELEASE_SHIFT per sample. At 8 kHz, shift 8 is a ~32 ms
 * time constant — slow enough that the eye sees syllables instead of flicker,
 * fast enough that the LED still tracks the chant. */
#define LED_RELEASE_SHIFT   8

/* Brightness gain, in 1/16ths. 16 = unity: a full-scale sample gives full
 * brightness. Raise it if a quiet recording never lights the LED properly -
 * anything above 16 simply clips the loud parts flat, which is easy to see. */
#define LED_GAIN_Q4         16

/* Perceptual correction. The eye responds roughly logarithmically to light, so
 * feeding a linear amplitude straight to a linear PWM duty makes everything
 * above a quarter scale look equally bright. Squaring the duty spreads the top
 * end back out and gives the LED visible dynamics. Set to 0 to see the raw,
 * flat-looking version — worth doing once, it is instructive. */
#define LED_GAMMA_SQUARE    1

/* ---- demo behaviour ------------------------------------------------------ */

/* 1 = play once at boot, then again on every PD3 press (the product's gesture).
 * 0 = loop forever, hands-off, for when you just want to watch it on a bench. */
#define DEMO_PRESS_TO_PLAY  1

#endif /* DEMO_BOARD_H */
