/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
 * Copyright (c) 2016 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>

#include "py/runtime.h"
#include "common-hal/pulseio/PWMOut.h"
#include "shared-bindings/pulseio/PWMOut.h"
#include "shared-bindings/microcontroller/Processor.h"

#include "atmel_start_pins.h"
#include "hal/utils/include/utils_repeat_macro.h"

#include "samd21_pins.h"

#undef ENABLE

#  define _TCC_SIZE(unused, n) TCC ## n ## _SIZE,
#  define TCC_SIZES         { REPEAT_MACRO(_TCC_SIZE, 0, TCC_INST_NUM) }

static uint32_t tcc_periods[TCC_INST_NUM];
static uint32_t tc_periods[TC_INST_NUM];

uint32_t target_tcc_frequencies[TCC_INST_NUM];
uint8_t tcc_refcount[TCC_INST_NUM];
const uint16_t prescaler[8] = {1, 2, 4, 8, 16, 64, 256, 1024};

// This bitmask keeps track of which channels of a TCC are currently claimed.
#ifdef SAMD21
uint8_t tcc_channels[3] = {0xf0, 0xfc, 0xfc};
const uint8_t tcc_cc_num[3] = {4, 2, 2};
const uint8_t tc_gclk_ids[] = {};
const uint8_t tc_gclk_ids[5] = {TC3_GCLK_ID, TC4_GCLK_ID, TC5_GCLK_ID, TC6_GCLK_ID, TC7_GCLK_ID};
const uint8_t tcc_gclk_ids[3] = {TCC0_GCLK_ID, TCC1_GCLK_ID, TCC2_GCLK_ID};
#endif
#ifdef SAMD51
uint8_t tcc_channels[5] = {0xc0, 0xf0, 0xf8, 0xfc, 0xfc};
static const uint8_t tcc_cc_num[5] = {6, 4, 3, 2, 2};
const uint8_t tc_gclk_ids[TC_INST_NUM] = {TC0_GCLK_ID,
                                          TC1_GCLK_ID,
                                          TC2_GCLK_ID,
                                          TC3_GCLK_ID
#ifdef TC4_GCLK_ID
                                          , TC4_GCLK_ID
#endif
#ifdef TC5_GCLK_ID
                                          , TC5_GCLK_ID
#endif
#ifdef TC6_GCLK_ID
                                          , TC6_GCLK_ID
#endif
#ifdef TC7_GCLK_ID
                                          , TC7_GCLK_ID
#endif
                                      };
const uint8_t tcc_gclk_ids[5] = {TCC0_GCLK_ID, TCC1_GCLK_ID, TCC2_GCLK_ID, TCC3_GCLK_ID,
                                 TCC4_GCLK_ID};
#endif
static Tc* const tc_insts[TC_INST_NUM] = TC_INSTS;
static Tcc* const tcc_insts[TCC_INST_NUM] = TCC_INSTS;

void pwmout_reset(void) {
    // Reset all timers
    for (int i = 0; i < TCC_INST_NUM; i++) {
        target_tcc_frequencies[i] = 0;
        tcc_refcount[i] = 0;
    }
    Tcc *tccs[TCC_INST_NUM] = TCC_INSTS;
    for (int i = 0; i < TCC_INST_NUM; i++) {
        // Disable the module before resetting it.
        if (tccs[i]->CTRLA.bit.ENABLE == 1) {
            tccs[i]->CTRLA.bit.ENABLE = 0;
            while (tccs[i]->SYNCBUSY.bit.ENABLE == 1) {
            }
        }
        uint8_t mask = 0xff;
        for (uint8_t j = 0; j < tcc_cc_num[i]; j++) {
            mask <<= 1;
        }
        tcc_channels[i] = 0xf0;
        tccs[i]->CTRLA.bit.SWRST = 1;
    }
    Tc *tcs[TC_INST_NUM] = TC_INSTS;
    for (int i = 0; i < TC_INST_NUM; i++) {
        tcs[i]->COUNT16.CTRLA.bit.SWRST = 1;
        while (tcs[i]->COUNT16.CTRLA.bit.SWRST == 1) {
        }
    }
}

static uint8_t tcc_channel(const pin_timer_t* t) {
    // For the SAMD51 this hardcodes the use of OTMX == 0x0, the output matrix mapping, which uses
    // SAMD21-style modulo mapping.
    return t->wave_output % tcc_cc_num[t->index];
}

static void tc_set_enable(Tc* tc, bool enable) {
    tc->COUNT16.CTRLA.bit.ENABLE = enable;
    while (tc->COUNT16.SYNCBUSY.bit.ENABLE != 0) {
        /* Wait for sync */
    }
}

static void tcc_set_enable(Tcc* tcc, bool enable) {
    tcc->CTRLA.bit.ENABLE = enable;
    while (tcc->SYNCBUSY.bit.ENABLE != 0) {
        /* Wait for sync */
    }
}

bool channel_ok(const pin_timer_t* t) {
    uint8_t channel_bit = 1 << tcc_channel(t);
    return (!t->is_tc && ((tcc_channels[t->index] & channel_bit) == 0)) ||
            t->is_tc;
}

void turn_on_clocks(const pin_timer_t* timer) {
    uint8_t gclk_id;
    if (timer->is_tc) {
        gclk_id = tc_gclk_ids[timer->index];
    } else {
        gclk_id = tcc_gclk_ids[timer->index];
    }
    // Turn on the clocks for the peripherals.
    #ifdef SAMD51
    if (timer->is_tc) {
        switch (timer->index) {
            case 0:
                MCLK->APBAMASK.reg |= MCLK_APBAMASK_TC0;
                break;
            case 1:
                MCLK->APBAMASK.reg |= MCLK_APBAMASK_TC1;
                break;
            case 2:
                MCLK->APBBMASK.reg |= MCLK_APBBMASK_TC2;
                break;
            case 3:
                MCLK->APBBMASK.reg |= MCLK_APBBMASK_TC3;
                break;
            case 4:
                MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC4;
                break;
            case 5:
                MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC5;
                break;
            case 6:
                MCLK->APBDMASK.reg |= MCLK_APBDMASK_TC6;
                break;
            case 7:
                MCLK->APBDMASK.reg |= MCLK_APBDMASK_TC7;
                break;
            default:
                break;
        }
    } else {
        switch (timer->index) {
            case 0:
                MCLK->APBBMASK.reg |= MCLK_APBBMASK_TCC0;
                break;
            case 1:
                MCLK->APBBMASK.reg |= MCLK_APBBMASK_TCC1;
                break;
            case 2:
                MCLK->APBCMASK.reg |= MCLK_APBCMASK_TCC2;
                break;
            case 3:
                MCLK->APBCMASK.reg |= MCLK_APBCMASK_TCC3;
                break;
            case 4:
                MCLK->APBDMASK.reg |= MCLK_APBDMASK_TCC4;
                break;
            default:
                break;
        }
    }

    // FIXME(tannewt): TC4-TC7 can only have 100mhz inputs.

    hri_gclk_write_PCHCTRL_reg(GCLK, gclk_id,
                               GCLK_PCHCTRL_GEN_GCLK0_Val | (1 << GCLK_PCHCTRL_CHEN_Pos));
    #endif

    #ifdef SAMD21
    // Determine the clock slot on the APBC bus. TCC0 is the first and 8 slots in.
    uint8_t clock_slot = 8 + timer->index;
    // We index TCs starting at zero but in memory they begin at three so we have to add three.
    if (timer->is_tc) {
        clock_slot += 3;
    }
    PM->APBCMASK.reg |= 1 << clock_slot;
    _gclk_enable_channel(gclk_id, GCLK_CLKCTRL_GEN_GCLK0_Val);
    #endif
}

void common_hal_pulseio_pwmout_construct(pulseio_pwmout_obj_t* self,
                                          const mcu_pin_obj_t* pin,
                                          uint16_t duty,
                                          uint32_t frequency,
                                          bool variable_frequency) {
    self->pin = pin;
    self->variable_frequency = variable_frequency;

    if (pin->timer[0].index >= TC_INST_NUM &&
        pin->timer[1].index >= TCC_INST_NUM
#ifdef SAMD51
        && pin->timer[2].index >= TCC_INST_NUM
#endif
        ) {
        mp_raise_ValueError("Invalid pin");
    }

    if (frequency == 0 || frequency > 6000000) {
        mp_raise_ValueError("Invalid PWM frequency");
    }

    // Figure out which timer we are using.

    // First see if a tcc is already going with the frequency we want and our
    // channel is unused. tc's don't have neough channels to share.
    const pin_timer_t* timer = NULL;
    uint8_t mux_position = 0;
    if (!variable_frequency) {
        for (uint8_t i = 0; i < TCC_INST_NUM && timer == NULL; i++) {
            if (target_tcc_frequencies[i] != frequency) {
                continue;
            }
            for (uint8_t j = 0; j < NUM_TIMERS_PER_PIN && timer == NULL; j++) {
                const pin_timer_t* t = &pin->timer[j];
                if (t->index != i || t->is_tc || t->index >= TCC_INST_NUM) {
                    continue;
                }
                Tcc* tcc = tcc_insts[t->index];
                if (tcc->CTRLA.bit.ENABLE == 1 && channel_ok(t)) {
                    timer = t;
                    mux_position = j;
                }
            }
        }
    }

    // No existing timer has been found, so find a new one to use and set it up.
    if (timer == NULL) {
        // By default, with fixed frequency we want to share a TCC because its likely we'll have
        // other outputs at the same frequency. If the frequency is variable then we'll only have
        // one output so we start with the TCs to see if they work.
        int8_t direction = -1;
        uint8_t start = NUM_TIMERS_PER_PIN - 1;
        if (variable_frequency) {
            direction = 1;
            start = 0;
        }
        for (uint8_t i = start; i >= 0 && i < NUM_TIMERS_PER_PIN && timer == NULL; i += direction) {
            const pin_timer_t* t = &pin->timer[i];
            if ((!t->is_tc && t->index >= TCC_INST_NUM) ||
                (t->is_tc && t->index >= TC_INST_NUM)) {
                continue;
            }
            if (t->is_tc) {
                Tc* tc = tc_insts[t->index];
                if (tc->COUNT16.CTRLA.bit.ENABLE == 0 && t->wave_output == 1) {
                    timer = t;
                    mux_position = i;
                }
            } else {
                Tcc* tcc = tcc_insts[t->index];
                if (tcc->CTRLA.bit.ENABLE == 0 && channel_ok(t)) {
                    timer = t;
                    mux_position = i;
                }
            }
        }

        if (timer == NULL) {
            mp_raise_RuntimeError("All timers in use");
            return;
        }

        uint8_t resolution = 0;
        if (timer->is_tc) {
            resolution = 16;
        } else {
            // TCC resolution varies so look it up.
            const uint8_t _tcc_sizes[TCC_INST_NUM] = TCC_SIZES;
            resolution = _tcc_sizes[timer->index];
        }
        // First determine the divisor that gets us the highest resolution.
        uint32_t system_clock = common_hal_mcu_processor_get_frequency();
        uint32_t top;
        uint8_t divisor;
        for (divisor = 0; divisor < 8; divisor++) {
            top = (system_clock / prescaler[divisor] / frequency) - 1;
            if (top < (1u << resolution)) {
                break;
            }
        }

        turn_on_clocks(timer);

        if (timer->is_tc) {
            tc_periods[timer->index] = top;
            Tc* tc = tc_insts[timer->index];
            #ifdef SAMD21
            tc->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 |
                                    TC_CTRLA_PRESCALER(divisor) |
                                    TC_CTRLA_WAVEGEN_MPWM;
            tc->COUNT16.CC[0].reg = top;
            #endif
            #ifdef SAMD51

            tc->COUNT16.CTRLA.bit.SWRST = 1;
            while (tc->COUNT16.CTRLA.bit.SWRST == 1) {
            }
            tc_set_enable(tc, false);
            tc->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER(divisor);
            tc->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MPWM;
            tc->COUNT16.CCBUF[0].reg = top;
            tc->COUNT16.CCBUF[1].reg = 0;
            #endif

            tc_set_enable(tc, true);
        } else {
            tcc_periods[timer->index] = top;
            Tcc* tcc = tcc_insts[timer->index];
            tcc_set_enable(tcc, false);
            tcc->CTRLA.bit.PRESCALER = divisor;
            tcc->PER.bit.PER = top;
            tcc->WAVE.bit.WAVEGEN = TCC_WAVE_WAVEGEN_NPWM_Val;
            tcc_set_enable(tcc, true);
            target_tcc_frequencies[timer->index] = frequency;
            tcc_refcount[timer->index]++;
            if (variable_frequency) {
                // We're changing frequency so claim all of the channels.
                tcc_channels[timer->index] = 0xff;
            } else {
                tcc_channels[timer->index] |= (1 << tcc_channel(timer));
            }
        }
    }

    self->timer = timer;

    gpio_set_pin_function(pin->pin, GPIO_PIN_FUNCTION_E + mux_position);

    common_hal_pulseio_pwmout_set_duty_cycle(self, duty);
}

bool common_hal_pulseio_pwmout_deinited(pulseio_pwmout_obj_t* self) {
    return self->pin == mp_const_none;
}

void common_hal_pulseio_pwmout_deinit(pulseio_pwmout_obj_t* self) {
    if (common_hal_pulseio_pwmout_deinited(self)) {
        return;
    }
    const pin_timer_t* t = self->timer;
    if (t->is_tc) {
        Tc* tc = tc_insts[t->index];
        tc_set_enable(tc, false);
        tc->COUNT16.CTRLA.bit.SWRST = true;
        while (tc->COUNT16.SYNCBUSY.bit.SWRST != 0) {
            /* Wait for sync */
        }
    } else {
        tcc_refcount[t->index]--;
        tcc_channels[t->index] &= ~(1 << tcc_channel(t));
        if (tcc_refcount[t->index] == 0) {
            target_tcc_frequencies[t->index] = 0;
            Tcc* tcc = tcc_insts[t->index];
            tcc_set_enable(tcc, false);
            tcc->CTRLA.bit.SWRST = true;
            while (tcc->SYNCBUSY.bit.SWRST != 0) {
                /* Wait for sync */
            }
        }
    }
    reset_pin(self->pin->pin);
    self->pin = mp_const_none;
}

extern void common_hal_pulseio_pwmout_set_duty_cycle(pulseio_pwmout_obj_t* self, uint16_t duty) {
    const pin_timer_t* t = self->timer;
    if (t->is_tc) {
        uint16_t adjusted_duty = tc_periods[t->index] * duty / 0xffff;
        #ifdef SAMD21
        tc_insts[t->index]->COUNT16.CC[t->wave_output].reg = adjusted_duty;
        #endif
        #ifdef SAMD51
        Tc* tc = tc_insts[t->index];
        while (tc->COUNT16.SYNCBUSY.bit.CC1 != 0) {
            // Wait for a previous value to be written.
        }
        tc->COUNT16.CCBUF[1].reg = adjusted_duty;
        #endif
    } else {
        uint32_t adjusted_duty = ((uint64_t) tcc_periods[t->index]) * duty / 0xffff;
        uint8_t channel = tcc_channel(t);
        Tcc* tcc = tcc_insts[t->index];
        while ((tcc->SYNCBUSY.vec.CC & (1 << channel)) != 0) {
            // Wait for a previous value to be written.
        }
        tcc->CCBUF[channel].reg = adjusted_duty;
    }
}

uint16_t common_hal_pulseio_pwmout_get_duty_cycle(pulseio_pwmout_obj_t* self) {
    const pin_timer_t* t = self->timer;
    if (t->is_tc) {
        Tc* tc = tc_insts[t->index];
        while (tc->COUNT16.SYNCBUSY.reg != 0) {
            /* Wait for sync */
        }
        uint16_t cv = tc->COUNT16.CC[t->wave_output].reg;
        return cv * 0xffff / tc_periods[t->index];
    } else {
        Tcc* tcc = tcc_insts[t->index];
        uint8_t channel = tcc_channel(t);
        uint32_t cv = 0;
        #ifdef SAMD21
        if ((tcc->STATUS.vec.CCBV & (1 << channel)) != 0) {
            cv = tcc->CCB[channel].reg;
        } else {
            cv = tcc->CC[channel].reg;
        }
        #endif
        #ifdef SAMD51
        if ((tcc->STATUS.vec.CCBUFV & (1 << channel)) != 0) {
            cv = tcc->CCBUF[channel].reg;
        } else {
            cv = tcc->CC[channel].reg;
        }
        #endif

        uint32_t duty_cycle = ((uint64_t) cv) * 0xffff / tcc_periods[t->index];

        return duty_cycle;
    }
}


void common_hal_pulseio_pwmout_set_frequency(pulseio_pwmout_obj_t* self,
                                              uint32_t frequency) {
    if (frequency == 0 || frequency > 6000000) {
        mp_raise_ValueError("Invalid PWM frequency");
    }
    const pin_timer_t* t = self->timer;
    uint8_t resolution;
    if (t->is_tc) {
        resolution = 16;
    } else {
        resolution = 24;
    }
    uint32_t system_clock = common_hal_mcu_processor_get_frequency();
    uint32_t new_top;
    uint8_t new_divisor;
    for (new_divisor = 0; new_divisor < 8; new_divisor++) {
        new_top = (system_clock / prescaler[new_divisor] / frequency) - 1;
        if (new_top < (1u << resolution)) {
            break;
        }
    }
    uint16_t old_duty = common_hal_pulseio_pwmout_get_duty_cycle(self);
    if (t->is_tc) {
        Tc* tc = tc_insts[t->index];
        uint8_t old_divisor = tc->COUNT16.CTRLA.bit.PRESCALER;
        if (new_divisor != old_divisor) {
            tc->COUNT16.CTRLA.bit.ENABLE = false;
            while (tc->COUNT16.SYNCBUSY.bit.ENABLE != 0) {
                /* Wait for sync */
            }
            tc->COUNT16.CTRLA.bit.PRESCALER = new_divisor;
            tc->COUNT16.CTRLA.bit.ENABLE = true;
        }
        tc_periods[t->index] = new_top;
        while (tc->COUNT16.SYNCBUSY.reg != 0) {
            /* Wait for sync */
        }
        #ifdef SAMD21
        tc->COUNT16.CC[0].reg = new_top;
        #endif
        #ifdef SAMD51
        tc->COUNT16.CCBUF[0].reg = new_top;
        #endif
    } else {
        Tcc* tcc = tcc_insts[t->index];
        uint8_t old_divisor = tcc->CTRLA.bit.PRESCALER;
        if (new_divisor != old_divisor) {
            tcc->CTRLA.bit.ENABLE = false;
            while (tcc->SYNCBUSY.bit.ENABLE != 0) {
                /* Wait for sync */
            }
            tcc->CTRLA.bit.PRESCALER = new_divisor;
            tcc->CTRLA.bit.ENABLE = true;
        }
        tcc_periods[t->index] = new_top;
        while (tcc->SYNCBUSY.reg != 0) {
            /* Wait for sync */
        }
        tcc->PERBUF.bit.PERBUF = new_top;
    }

    common_hal_pulseio_pwmout_set_duty_cycle(self, old_duty);
}

uint32_t common_hal_pulseio_pwmout_get_frequency(pulseio_pwmout_obj_t* self) {
    uint32_t system_clock = common_hal_mcu_processor_get_frequency();
    const pin_timer_t* t = self->timer;
    uint8_t divisor;
    uint32_t top;
    if (t->is_tc) {
        divisor = tc_insts[t->index]->COUNT16.CTRLA.bit.PRESCALER;
        top = tc_periods[t->index];
    } else {
        divisor = tcc_insts[t->index]->CTRLA.bit.PRESCALER;
        top = tcc_periods[t->index];
    }
    return (system_clock / prescaler[divisor]) / (top + 1);
}

bool common_hal_pulseio_pwmout_get_variable_frequency(pulseio_pwmout_obj_t* self) {
    return self->variable_frequency;
}
