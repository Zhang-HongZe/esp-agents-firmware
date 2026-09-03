/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>

/**
 * Initialize the battery fuel gauge.
 * On failure the low-battery prompt is disabled and init still returns ESP_OK.
 */
esp_err_t app_battery_init(void);
