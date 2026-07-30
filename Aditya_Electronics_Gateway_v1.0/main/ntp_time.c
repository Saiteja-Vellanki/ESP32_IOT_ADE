#include "ntp_time.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "NTP";


void ntp_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    setenv("TZ", "IST-5:30", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = {0};

    int retry = 0;
    while (retry < 15)
    {
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year > (2024 - 1900))
        {
            ESP_LOGI(TAG, "NTP synchronized");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
}
