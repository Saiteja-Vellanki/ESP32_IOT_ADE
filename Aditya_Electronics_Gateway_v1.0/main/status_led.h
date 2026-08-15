#pragma once
#include <stdbool.h>

typedef enum {
    LED_MODE_SEARCHING   = 0,
    LED_MODE_CONNECTED   = 1,
    LED_MODE_LOW_BATTERY = 2,
} led_mode_t;

void status_led_init(void);
void status_led_set_mode(led_mode_t mode);
void buzzer_init(void);
void buzzer_set_no_signal(bool no_signal);
