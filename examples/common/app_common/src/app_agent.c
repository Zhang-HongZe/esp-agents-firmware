/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_log.h>
#include <esp_check.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_event.h>

#include <esp_agent.h>
#include <esp_agent_core.h>

#include <agent_setup.h>
#include <setup/rainmaker.h>
#include <board_defs.h>
#include <esp_console.h>
#include <string.h>

#include "app_audio.h"
#include "app_agent.h"
#include "app_device.h"

static const char *TAG = "app_agent";

typedef struct {
    bool initialized;
    app_agent_state_t state;
    esp_agent_handle_t agent_handle;
    esp_event_handler_instance_t agent_event_handler;
    esp_event_handler_instance_t agent_setup_event_handler;
    app_agent_config_t config;
} app_agent_data_t;

app_agent_data_t g_app_agent_data;

#define APP_AGENT_ERR_MSG_NOT_CONFIGURED "Use RainMaker Home"
#define APP_AGENT_MSG_WAITING_TOKEN      "Waiting for Home"
#define APP_AGENT_ERR_MSG_WIFI           "WiFi disconnected"
#define APP_AGENT_ERR_MSG_WIFI_NEED_CONNECT "Network not connected, please configure network"
#define APP_AGENT_MSG_WIFI_CONNECTED     "WiFi connected"
#define APP_AGENT_MSG_SERVER_CONNECTING  "Connecting..."
#define APP_AGENT_MSG_SERVER_CONNECTED   "Service connected"
#define APP_AGENT_ERR_MSG_QUOTA          "Quota exceeded"
#define APP_AGENT_ERR_MSG_CONVERSATION   "Service disconnected"
#define APP_AGENT_ERR_MSG_SERVICE        "Service disconnected"

/* Home app sends AgentAuth.UserToken after MQTT; stock RainMaker app never does.
 * This log wait covers that gap (Home typically ~10-15s after GOT_IP).
 */
#define APP_AGENT_TOKEN_WAIT_US          (40 * 1000 * 1000ULL)

/* Match speaker AI_Buddy: wait 10s, then play need-connect up to 3 times / 20s. */
#define APP_AGENT_WIFI_NEED_CONNECT_DELAY_US     (10 * 1000 * 1000ULL)
#define APP_AGENT_WIFI_NEED_CONNECT_INTERVAL_US  (20 * 1000 * 1000ULL)
#define APP_AGENT_WIFI_NEED_CONNECT_REPEAT       3

#define APP_AGENT_SERVER_CONNECTING_INTERVAL_US  (20 * 1000 * 1000ULL)
#define APP_AGENT_SERVER_CONNECTING_REPEAT       3

static esp_timer_handle_t s_token_wait_timer;
static esp_timer_handle_t s_wifi_need_connect_timer;
static int s_wifi_need_connect_remaining;
static esp_timer_handle_t s_server_connecting_timer;
static int s_server_connecting_remaining;
static bool s_wifi_had_ip;
static bool s_agent_ever_started;
static bool s_new_conversation_on_start;

static void app_agent_report_error(const char *text, app_device_error_kind_t error_kind)
{
    if (!text) {
        return;
    }

    /* Error strings are literals; do not strdup/free (heap was corrupted
     * when free() ran while GMF media started during agent stop).
     */
    device_event_data_t event_data = {
        .text = text,
        .error_kind = error_kind,
    };
    app_device_event_enqueue(DEVICE_EVENT_ERROR, &event_data);
}

static void app_agent_token_wait_timeout(void *arg)
{
    (void)arg;
    if (agent_setup_get_refresh_token()) {
        return;
    }
    ESP_LOGW(TAG, "No UserToken after Wi-Fi/MQTT; likely ESP RainMaker app instead of RainMaker Home");
    app_agent_report_error(APP_AGENT_ERR_MSG_NOT_CONFIGURED, APP_DEVICE_ERROR_NOT_CONFIGURED);
}

static void app_agent_stop_token_wait(void);
static void app_agent_stop_wifi_need_connect(void);
static void app_agent_schedule_wifi_need_connect(void);
static void app_agent_stop_server_connecting(void);
static void app_agent_start_server_connecting(void);

static void app_agent_wifi_need_connect_timeout(void *arg)
{
    (void)arg;
    if (s_wifi_had_ip) {
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi still down, play need-connect prompt (%d left)", s_wifi_need_connect_remaining);
    app_agent_report_error(APP_AGENT_ERR_MSG_WIFI_NEED_CONNECT, APP_DEVICE_ERROR_WIFI_NEED_CONNECT);
    s_wifi_need_connect_remaining--;
    if (s_wifi_need_connect_remaining > 0 && s_wifi_need_connect_timer) {
        esp_timer_start_once(s_wifi_need_connect_timer, APP_AGENT_WIFI_NEED_CONNECT_INTERVAL_US);
    }
}

static void app_agent_stop_wifi_need_connect(void)
{
    s_wifi_need_connect_remaining = 0;
    if (s_wifi_need_connect_timer && esp_timer_is_active(s_wifi_need_connect_timer)) {
        esp_timer_stop(s_wifi_need_connect_timer);
    }
}

static void app_agent_schedule_wifi_need_connect(void)
{
    if (s_wifi_had_ip || !s_wifi_need_connect_timer) {
        return;
    }
    app_agent_stop_wifi_need_connect();
    s_wifi_need_connect_remaining = APP_AGENT_WIFI_NEED_CONNECT_REPEAT;
    ESP_LOGI(TAG, "No Wi-Fi yet, play need-connect prompt in %d ms",
             (int)(APP_AGENT_WIFI_NEED_CONNECT_DELAY_US / 1000));
    esp_timer_start_once(s_wifi_need_connect_timer, APP_AGENT_WIFI_NEED_CONNECT_DELAY_US);
}

static void app_agent_stop_server_connecting(void)
{
    s_server_connecting_remaining = 0;
    if (s_server_connecting_timer && esp_timer_is_active(s_server_connecting_timer)) {
        esp_timer_stop(s_server_connecting_timer);
    }
}

static void app_agent_server_connecting_timeout(void *arg)
{
    (void)arg;
    if (g_app_agent_data.state == APP_AGENT_STATE_STARTED ||
        g_app_agent_data.state == APP_AGENT_STATE_DISCONNECTED) {
        return;
    }
    app_agent_report_error(APP_AGENT_MSG_SERVER_CONNECTING, APP_DEVICE_ERROR_SERVER_CONNECTING);
    s_server_connecting_remaining--;
    if (s_server_connecting_remaining > 0 && s_server_connecting_timer) {
        esp_timer_start_once(s_server_connecting_timer, APP_AGENT_SERVER_CONNECTING_INTERVAL_US);
    }
}

static void app_agent_start_server_connecting(void)
{
    if (!s_server_connecting_timer) {
        return;
    }
    app_agent_stop_server_connecting();
    s_server_connecting_remaining = APP_AGENT_SERVER_CONNECTING_REPEAT - 1;
    app_agent_report_error(APP_AGENT_MSG_SERVER_CONNECTING, APP_DEVICE_ERROR_SERVER_CONNECTING);
    if (s_server_connecting_remaining > 0) {
        esp_timer_start_once(s_server_connecting_timer, APP_AGENT_SERVER_CONNECTING_INTERVAL_US);
    }
}

static void app_agent_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }
    /* Ignore probe/assoc failures before the first GOT_IP, and reconnect retries. */
    if (!s_wifi_had_ip) {
        return;
    }
    s_wifi_had_ip = false;
    app_agent_stop_token_wait();
    app_agent_stop_server_connecting();
    ESP_LOGW(TAG, "Wi-Fi disconnected");
    app_agent_report_error(APP_AGENT_ERR_MSG_WIFI, APP_DEVICE_ERROR_WIFI);
    app_agent_schedule_wifi_need_connect();
}

static void app_agent_stop_token_wait(void)
{
    if (s_token_wait_timer && esp_timer_is_active(s_token_wait_timer)) {
        esp_timer_stop(s_token_wait_timer);
    }
}

static void app_agent_start_token_wait(void)
{
    if (!s_token_wait_timer) {
        return;
    }
    app_agent_stop_token_wait();
    esp_timer_start_once(s_token_wait_timer, APP_AGENT_TOKEN_WAIT_US);
}

static inline void app_agent_update_state(app_agent_state_t state)
{
    g_app_agent_data.state = state;
    app_device_event_enqueue(DEVICE_EVENT_AGENT_STATE_CHANGED, NULL);
}

void app_agent_default_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_agent_message_data_t *data = (esp_agent_message_data_t *) event_data;

    switch (event_id) {
        case ESP_AGENT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Agent Connected. Waiting to start conversation.");
            app_agent_update_state(APP_AGENT_STATE_CONNECTED);
            break;
        case ESP_AGENT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Agent Not Connected");
            app_agent_stop_server_connecting();
            s_new_conversation_on_start = true;
            app_agent_update_state(APP_AGENT_STATE_DISCONNECTED);
            // Stop microphone to prevent sending data while disconnected
            app_device_event_enqueue(DEVICE_EVENT_SLEEP, NULL);
            break;
        case ESP_AGENT_EVENT_SPEECH_START:
            ESP_LOGD(TAG, "ESP Agent Received Speech Start");
            app_device_event_enqueue(DEVICE_EVENT_SPEECH_START, NULL);
            break;
        case ESP_AGENT_EVENT_SPEECH_END:
            ESP_LOGD(TAG, "ESP Agent Received Speech End");
            app_device_event_enqueue(DEVICE_EVENT_SPEECH_END, NULL);
            break;
        case ESP_AGENT_EVENT_DATA_TYPE_TEXT:
            {
                if (data->text.generation_stage == ESP_AGENT_MESSAGE_GENERATION_STAGE_FINAL) {
                    break;
                }

                app_device_event_t event = DEVICE_EVENT_SET_USER_TEXT;
                char *text = NULL;
                if (data->text.text) {
                    text = strdup(data->text.text);
                }

                if (data->text.role == ESP_AGENT_MESSAGE_ROLE_USER) {
                    event = DEVICE_EVENT_SET_USER_TEXT;
                } else if (data->text.role == ESP_AGENT_MESSAGE_ROLE_ASSISTANT) {
                    event = DEVICE_EVENT_SET_ASSISTANT_TEXT;
                }

                device_event_data_t event_data = { .text = text };
                app_device_event_enqueue(event, &event_data);
            }
            break;
        case ESP_AGENT_EVENT_DATA_TYPE_SPEECH:
            ESP_LOGD(TAG, "ESP Agent Speech data: %d", data->speech.len);
            app_audio_play_speech((uint8_t *)data->speech.data, data->speech.len);
            break;
        case ESP_AGENT_EVENT_DATA_TYPE_THINKING:
            /* Display the thought in gray color */
            printf("\033[90mThought: %s\033[0m\n", data->thinking.thought);
            break;
        case ESP_AGENT_EVENT_ERROR:
            app_agent_stop_server_connecting();
            if (data->error.error == ESP_AGENT_AUDIO_CONVERSATION_ERROR) {
                ESP_LOGE(TAG, "ESP Agent Audio Conversation Error");
                app_agent_report_error(APP_AGENT_ERR_MSG_CONVERSATION, APP_DEVICE_ERROR_CONVERSATION);
                /* Device state will be changed to sleep on ESP_AGENT_EVENT_DISCONNECT */
                esp_agent_stop(g_app_agent_data.agent_handle);
            } else if (data->error.error == ESP_AGENT_QUOTA_EXCEEDED_ERROR) {
                ESP_LOGE(TAG, "ESP Agent Quota Exceeded");
                app_agent_report_error(APP_AGENT_ERR_MSG_QUOTA, APP_DEVICE_ERROR_QUOTA);
                esp_agent_stop(g_app_agent_data.agent_handle);
            } else {
                ESP_LOGE(TAG, "ESP Agent Error: %d", data->error.error);
                app_agent_report_error(APP_AGENT_ERR_MSG_SERVICE, APP_DEVICE_ERROR_SERVICE);
            }
            break;
        case ESP_AGENT_EVENT_START:
            s_agent_ever_started = true;
            app_agent_stop_server_connecting();
            app_agent_update_state(APP_AGENT_STATE_STARTED);
            app_agent_report_error(APP_AGENT_MSG_SERVER_CONNECTED, APP_DEVICE_ERROR_SERVER_CONNECTED);
            ESP_LOGI(TAG, "ESP Agent Started");
            break;
        default:
            break;
    }
}

esp_err_t app_agent_send_speech(uint8_t *audio_data, size_t audio_data_len)
{
    if (g_app_agent_data.state != APP_AGENT_STATE_STARTED) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_agent_send_speech(g_app_agent_data.agent_handle, audio_data, audio_data_len, pdMS_TO_TICKS(1000));
}

void app_agent_start_task(void *arg)
{
    char *agent_id = agent_setup_get_agent_id();
    char *refresh_token = agent_setup_get_refresh_token();
    if (!agent_id || !refresh_token) {
        ESP_LOGE(TAG, "Agent ID or refresh token not found");
        app_agent_report_error(APP_AGENT_ERR_MSG_NOT_CONFIGURED, APP_DEVICE_ERROR_NOT_CONFIGURED);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_ERROR(esp_agent_set_agent_id(g_app_agent_data.agent_handle, agent_id), end, TAG, "Failed to set agent ID");
    ESP_GOTO_ON_ERROR(esp_agent_set_refresh_token(g_app_agent_data.agent_handle, refresh_token), end, TAG, "Failed to set refresh token");
    ESP_GOTO_ON_ERROR(app_audio_start(), end, TAG, "Failed to start audio pipeline");
    ESP_GOTO_ON_ERROR(app_agent_connect(), end, TAG, "Failed to start agent");

    app_device_event_enqueue(DEVICE_EVENT_SYSTEM_INITIALIZED, NULL);

end:
    if (ret != ESP_OK) {
        app_agent_report_error(APP_AGENT_ERR_MSG_SERVICE, APP_DEVICE_ERROR_SERVICE);
    }
    vTaskDelete(NULL);
}

static void agent_setup_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
        case AGENT_SETUP_EVENT_AGENT_ID_UPDATE:
            {
                ESP_LOGI(TAG, "Agent ID updated");
                if (g_app_agent_data.agent_handle) {
                    char *agent_id = agent_setup_get_agent_id();
                    ESP_RETURN_VOID_ON_ERROR(esp_agent_set_agent_id(g_app_agent_data.agent_handle, agent_id), TAG, "Failed to set agent ID");
                }
            }
            break;

        case AGENT_SETUP_EVENT_START:
            {
                ESP_LOGI(TAG, "Agent setup completed");
                app_agent_stop_token_wait();
                xTaskCreate(app_agent_start_task, "app_agent_start_task", 4096, NULL, 5, NULL);
            }
            break;

        case AGENT_SETUP_EVENT_NETWORK_CONNECTED:
            /* GOT_IP is not "missing token". Home app writes AgentAuth.UserToken
             * only after MQTT; the stock RainMaker app never does.
             */
            s_wifi_had_ip = true;
            app_agent_stop_wifi_need_connect();
            ESP_LOGI(TAG, "Wi-Fi got IP");
            app_agent_report_error(APP_AGENT_MSG_WIFI_CONNECTED, APP_DEVICE_ERROR_WIFI_CONNECTED);
            if (!agent_setup_get_refresh_token()) {
                ESP_LOGI(TAG, "Network connected, waiting for RainMaker Home UserToken");
                app_agent_report_error(APP_AGENT_MSG_WAITING_TOKEN, APP_DEVICE_ERROR_WAITING_TOKEN);
                app_agent_start_token_wait();
            } else if (s_agent_ever_started &&
                       g_app_agent_data.state != APP_AGENT_STATE_STARTED &&
                       g_app_agent_data.state != APP_AGENT_STATE_CONNECTING) {
                ESP_LOGI(TAG, "Wi-Fi restored, restarting agent");
                app_agent_connect();
            }
            break;

        case AGENT_SETUP_EVENT_REFRESH_TOKEN_UPDATE:
            {
                app_agent_stop_token_wait();
                char *refresh_token = agent_setup_get_refresh_token();
                if (g_app_agent_data.agent_handle && refresh_token) {
                    ESP_LOGI(TAG, "Refresh token updated, applying without reboot");
                    ESP_RETURN_VOID_ON_ERROR(esp_agent_set_refresh_token(g_app_agent_data.agent_handle, refresh_token),
                                             TAG, "Failed to set refresh token");
                }
            }
            break;

        default:
            break;
    }
}

esp_err_t app_agent_speech_conversation_start(void)
{
    if (g_app_agent_data.state != APP_AGENT_STATE_STARTED) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_agent_speech_conversation_start(g_app_agent_data.agent_handle);
}

esp_err_t app_agent_speech_conversation_end(void)
{
    if (g_app_agent_data.state != APP_AGENT_STATE_STARTED) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_agent_speech_conversation_end(g_app_agent_data.agent_handle);
}

esp_err_t app_agent_connect(void)
{
    if (!g_app_agent_data.agent_handle) {
        ESP_LOGE(TAG, "Can't start agent, handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    app_agent_update_state(APP_AGENT_STATE_CONNECTING);
    app_agent_start_server_connecting();
    if (s_new_conversation_on_start) {
        ESP_LOGI(TAG, "Previous session disconnected, starting a new conversation");
        esp_agent_clear_conversation_id(g_app_agent_data.agent_handle);
        s_new_conversation_on_start = false;
    }
    esp_err_t ret = esp_agent_start(g_app_agent_data.agent_handle, NULL);
    if (ret != ESP_OK) {
        app_agent_stop_server_connecting();
        app_agent_update_state(APP_AGENT_STATE_DISCONNECTED);
        s_new_conversation_on_start = true;
        app_agent_report_error(APP_AGENT_ERR_MSG_SERVICE, APP_DEVICE_ERROR_SERVICE);
    }
    return ret;
}

esp_err_t app_agent_init(app_agent_config_t *config)
{
    if (g_app_agent_data.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!config->event_handler) {
        ESP_LOGE(TAG, "Event handler is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialize the agent setup: agent ID, refresh token, etc. */
    ESP_RETURN_ON_ERROR(agent_setup_init(), TAG, "Failed to initialize agent setup");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(AGENT_SETUP_EVENT, ESP_EVENT_ANY_ID, agent_setup_event_handler, NULL), TAG, "Failed to register agent event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   app_agent_wifi_event_handler, NULL),
                        TAG, "Failed to register Wi-Fi disconnect handler");

    if (!s_token_wait_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = app_agent_token_wait_timeout,
            .name = "token_wait",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_token_wait_timer), TAG, "Failed to create token wait timer");
    }

    if (!s_wifi_need_connect_timer) {
        const esp_timer_create_args_t wifi_need_args = {
            .callback = app_agent_wifi_need_connect_timeout,
            .name = "wifi_need",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&wifi_need_args, &s_wifi_need_connect_timer), TAG,
                            "Failed to create Wi-Fi need-connect timer");
    }

    if (!s_server_connecting_timer) {
        const esp_timer_create_args_t server_connecting_args = {
            .callback = app_agent_server_connecting_timeout,
            .name = "srv_conn",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&server_connecting_args, &s_server_connecting_timer), TAG,
                            "Failed to create server-connecting timer");
    }

    /* Initialize esp_agent without agent_id and refresh_token */
    esp_agent_audio_config_t upload_audio_config = {
        .format = ESP_AGENT_CONVERSATION_AUDIO_FORMAT_OPUS,
        .sample_rate = CONFIG_AUDIO_UPLOAD_SAMPLE_RATE,
        .frame_duration = CONFIG_AUDIO_UPLOAD_FRAME_DURATION_MS,
    };
    esp_agent_audio_config_t download_audio_config = {
        .format = ESP_AGENT_CONVERSATION_AUDIO_FORMAT_OPUS,
        .sample_rate = CONFIG_AUDIO_DOWNLOAD_SAMPLE_RATE,
        .frame_duration = CONFIG_AUDIO_DOWNLOAD_FRAME_DURATION_MS,
    };

    esp_agent_config_t agent_config = {
        .conversation_type = ESP_AGENT_CONVERSATION_SPEECH,
        .upload_audio_config = &upload_audio_config,
        .download_audio_config = &download_audio_config,
    };

    g_app_agent_data.agent_handle = esp_agent_init(&agent_config);
    if (g_app_agent_data.agent_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize agent");
        return ESP_FAIL;
    }

    /* Register event handler */
    esp_event_handler_t handler = config->event_handler;
    ESP_RETURN_ON_ERROR(esp_agent_register_event_handler(g_app_agent_data.agent_handle, ESP_EVENT_ANY_ID, handler, NULL, &g_app_agent_data.agent_event_handler), TAG, "Failed to register agent event handler");

    g_app_agent_data.state = APP_AGENT_STATE_DISCONNECTED;
    g_app_agent_data.initialized = true;
    g_app_agent_data.config = *config;

    return ESP_OK;
}

esp_err_t app_agent_start(void)
{
    esp_err_t ret = ESP_OK;
    /* Initialize ESP RainMaker.
    * This device manual shows up in the ESP RainMaker Home app after the device has been set up. This should be set
    * in the board_defs.h file.
    */
    ESP_RETURN_ON_ERROR(setup_rainmaker_init(BOARD_DEVICE_MANUAL_URL), TAG, "Failed to initialize RainMaker setup");

    /* Start network provisioning. This will start the network and connect to the cloud.
    * If the device has not been set up yet, it will start the setup process.
    * RainMaker will start automatically when network connectivity is established.
    */
    ESP_RETURN_ON_ERROR(agent_setup_start(), TAG, "Failed to start network provisioning");
    app_agent_schedule_wifi_need_connect();
    return ret;
}

bool app_agent_is_active(void)
{
    return (g_app_agent_data.state == APP_AGENT_STATE_STARTED);
}

app_agent_state_t app_agent_get_state(void)
{
    return g_app_agent_data.state;
}

esp_err_t app_agent_register_tool(const char *name, esp_agent_tool_handler_t tool_handler, void *user_data)
{
    if (!g_app_agent_data.agent_handle) {
        ESP_LOGE(TAG, "Agent handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_agent_register_local_tool(g_app_agent_data.agent_handle, name, tool_handler, user_data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register local tool: %s", name);
    }
    return err;
}

esp_err_t app_agent_tool_unregister(const char *name)
{
    if (!g_app_agent_data.agent_handle) {
        ESP_LOGE(TAG, "Agent handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    return esp_agent_unregister_local_tool(g_app_agent_data.agent_handle, name);
}
