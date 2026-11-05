// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OLIMEX LTD BULGARIA
//
// ch32fun configuration (read by the framework at build time).

#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

#define CH32V003 1

// 24 MHz straight from the internal HSI oscillator, no PLL: lower current,
// and the chip wakes from standby on the same clock, so nothing has to be
// set up again after sleep.
#define FUNCONF_USE_PLL           0
#define FUNCONF_SYSTEM_CORE_CLOCK 24000000

// SysTick runs at HCLK/8 = 3 MHz (hal.c counts on this).
#define FUNCONF_SYSTICK_USE_HCLK  0

// No printf over SWIO: the pin is used for LED7.
#define FUNCONF_USE_DEBUGPRINTF   0
#define FUNCONF_DEBUG_HARDFAULT   0

#endif
