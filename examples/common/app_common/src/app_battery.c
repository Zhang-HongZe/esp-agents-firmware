/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_board_manager.h>

#include "app_audio.h"
#include "app_battery.h"
#include "app_device.h"

extern const uint8_t low_power_mp3_start[] asm("_binary_low_power_mp3_start");
extern const uint8_t low_power_mp3_end[] asm("_binary_low_power_mp3_end");

static const char *TAG = "app_battery";

#define APP_BATTERY_POLL_INTERVAL_MS   2000
#define APP_BATTERY_LOW_SOC_PCT        1
#define APP_BATTERY_TASK_STACK         (4 * 1024)
#define APP_BATTERY_MIN_VOLTAGE_MV     2500
#define APP_BATTERY_MAX_VOLTAGE_MV     5000
#define BQ27220_I2C_ADDR               0x55
#define BQ27220_CMD_VOLTAGE            0x08
#define BQ27220_CMD_SOC                0x2C
#define BQ27220_I2C_TIMEOUT_MS         100

static i2c_master_dev_handle_t s_gauge_dev;
static bool s_low_power_reported;

static esp_err_t bq27220_read_u16(i2c_master_dev_handle_t dev, uint8_t cmd, uint16_t *out)
{
    uint8_t buf[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(dev, &cmd, 1, buf, sizeof(buf), BQ27220_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *out = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return ESP_OK;
}

static void battery_report_low_power(void)
{
    if (s_low_power_reported) {
        return;
    }
    s_low_power_reported = true;

    device_event_data_t event_data = {
        .text = "Low Power",
        .error_kind = APP_DEVICE_ERROR_LOW_POWER,
    };
    app_device_event_enqueue(DEVICE_EVENT_ERROR, &event_data);
    /* Let the device task show "Low Power" before playback occupies the speaker. */
    vTaskDelay(pdMS_TO_TICKS(100));

    (void)app_audio_speaker_stop();
    (void)app_audio_play_media_async("embed://audio/0_low_power.mp3",
                                     low_power_mp3_start,
                                     low_power_mp3_end - low_power_mp3_start);
    /* Wait for the prompt to finish; do not block device_process_task. */
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGE(TAG, "Low power, shutting down");
    esp_deep_sleep_start();
}

static void battery_poll_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_gauge_dev && !s_low_power_reported) {
            uint16_t soc = 0xFFFF;
            if (bq27220_read_u16(s_gauge_dev, BQ27220_CMD_SOC, &soc) == ESP_OK) {
                ESP_LOGI(TAG, "Battery SOC: %" PRIu16 "%%", soc);
                if (soc <= APP_BATTERY_LOW_SOC_PCT) {
                    ESP_LOGE(TAG, "Battery low: %" PRIu16 "%%", soc);
                    battery_report_low_power();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(APP_BATTERY_POLL_INTERVAL_MS));
    }
}

static i2c_master_bus_handle_t battery_get_i2c_bus(void)
{
    if (!esp_board_manager_check_name("i2c_master")) {
        return NULL;
    }

    void *handle = NULL;
    if (esp_board_manager_get_periph_handle("i2c_master", &handle) != ESP_OK || handle == NULL) {
        return NULL;
    }
    return (i2c_master_bus_handle_t)handle;
}

static i2c_master_dev_handle_t battery_open_bq27220(void)
{
    i2c_master_bus_handle_t bus = battery_get_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "Battery management chip not found");
        return NULL;
    }

    if (i2c_master_probe(bus, BQ27220_I2C_ADDR, BQ27220_I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "Battery management chip not found");
        return NULL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ27220_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK || dev == NULL) {
        ESP_LOGE(TAG, "Battery management chip not found");
        return NULL;
    }

    uint16_t voltage_mv = 0;
    if (bq27220_read_u16(dev, BQ27220_CMD_VOLTAGE, &voltage_mv) != ESP_OK ||
        voltage_mv < APP_BATTERY_MIN_VOLTAGE_MV || voltage_mv > APP_BATTERY_MAX_VOLTAGE_MV) {
        ESP_LOGE(TAG, "Battery management chip not supported");
        i2c_master_bus_rm_device(dev);
        return NULL;
    }

    ESP_LOGI(TAG, "Battery management chip initialized, voltage %" PRIu16 " mV", voltage_mv);
    return dev;
}

esp_err_t app_battery_init(void)
{
    s_gauge_dev = battery_open_bq27220();
    if (s_gauge_dev == NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(battery_poll_task, "app_battery", APP_BATTERY_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create battery poll task");
        s_gauge_dev = NULL;
        return ESP_OK;
    }
    return ESP_OK;
}
