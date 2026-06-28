#pragma once
#include <stdbool.h>

typedef enum {
    LED_MODE_SEARCHING   = 0,   /* fast blink — no WiFi signal    */
    LED_MODE_CONNECTED   = 1,   /* slow blink — good signal       */
    LED_MODE_LOW_BATTERY = 2,   /* medium blink — low battery     */
} led_mode_t;

void status_led_init(void);
void status_led_set_mode(led_mode_t mode);
void buzzer_init(void);         /* 5 beeps every 30s when no signal */
void buzzer_set_no_signal(bool no_signal);
