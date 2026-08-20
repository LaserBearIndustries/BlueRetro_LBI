/*
 * Copyright (c) 2019-2024, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "zephyr/atomic.h"
#include "system/gpio.h"
#include "driver/ledc.h"
#include "adapter/config.h"
#include "led.h"

#ifdef CONFIG_BLUERETRO_SYSTEM_SEA_BOARD
#define ERR_LED_PIN 32
#else
#define ERR_LED_PIN 17
#endif

/* LED flags */
enum {
    ERR_LED_SET = 0,
};

static atomic_t led_flags = 0;
static TaskHandle_t err_led_task_hdl;
static uint8_t err_led_pin = ERR_LED_PIN;

static void err_led_task(void *param) {
    while (1) {
        if (!atomic_test_bit(&led_flags, ERR_LED_SET)) {
            ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_duty_max,
                hw_config.led_pulse_fade_time_ms, LEDC_FADE_NO_WAIT);
            vTaskDelay(hw_config.led_pulse_fade_cycle_delay_ms / portTICK_PERIOD_MS);
            ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_duty_min,
                hw_config.led_pulse_fade_time_ms, LEDC_FADE_NO_WAIT);
            vTaskDelay(hw_config.led_pulse_fade_cycle_delay_ms / portTICK_PERIOD_MS);
        }
        else {
            vTaskSuspend(err_led_task_hdl);
        }
    }
}

void err_led_init(uint32_t package) {
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = hw_config.led_pulse_hz,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t ledc_channel = {
        .channel    = LEDC_CHANNEL_0,
        .duty       = hw_config.led_pulse_off_duty_cycle,
        .gpio_num   = ERR_LED_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER_0,
    };

    /* Upstream moved the error LED to another pin when it found itself on an
     * ESP32-PICO-V3-02, the module HW2 used, because that part bonds out fewer
     * pins than a D0WDQ6. The S31 board is a single known module with 60 GPIOs
     * and nothing to detect, so the LED pin is simply a board fact -- one that
     * belongs with the rest of the pin map still to be redone for this board.
     * package is left in the signature because it is part of the init
     * plumbing shared with the wired drivers.
     */
    (void)package;

    ledc_timer_config(&ledc_timer);
    ledc_channel_config(&ledc_channel);
    ledc_fade_func_install(0);
    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_off_duty_cycle, 0);

    xTaskCreatePinnedToCore(&err_led_task, "err_led_task", 768, NULL, 5, &err_led_task_hdl, 0);
    err_led_clear();
}

void err_led_cfg_update(void) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, hw_config.led_pulse_hz);
}

void err_led_set(void) {
    vTaskSuspend(err_led_task_hdl);
    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_on_duty_cycle, 0);
    atomic_set_bit(&led_flags, ERR_LED_SET);
}

void err_led_clear(void) {
    /* When error is set it stay on until power cycle */
    if (!atomic_test_bit(&led_flags, ERR_LED_SET)) {
        vTaskSuspend(err_led_task_hdl);
        ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_off_duty_cycle, 0);
    }
}

void err_led_pulse(void) {
    vTaskResume(err_led_task_hdl);
}

uint32_t err_led_get_pin(void) {
    return err_led_pin;
}
