/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "mqtt_client.h"

// --- MQTT GLOBALS ---

// most config macros are set via the sdkmenu (`make menuconfig`)
static const char *MQTT_TAG = "mqtt";

#define MQTT_SENSOR_AVAILABILITY_TOPIC CONFIG_MQTT_SENSOR_TOPIC "/availability"
#define MQTT_QOS 1
#define MQTT_RETAIN 1
#define MQTT_KEEPALIVE_SECONDS 120

#define MQTT_TOPIC_CONFIG \
    "{\n" \
    "  \"name\": \"Bears Door\",\n" \
    "  \"unique_id\": \"bears_door_lock_1\",\n" \
    "  \"device_class\": \"lock\",\n" \
    "  \"state_topic\": \"" CONFIG_MQTT_SENSOR_TOPIC "\",\n" \
    "  \"availability_topic\": \"" MQTT_SENSOR_AVAILABILITY_TOPIC "\",\n" \
    "  \"payload_available\": \"online\",\n" \
    "  \"payload_not_available\": \"offline\",\n" \
    "  \"payload_on\": \"ON\",\n" \
    "  \"payload_off\": \"OFF\",\n" \
    "  \"device\": {\n" \
    "    \"identifiers\": [\"door_lock_sensor_dev\"],\n" \
    "    \"name\": \"Lock Sensor\",\n" \
    "    \"manufacturer\": \"NJT Industrie\",\n" \
    "    \"model\": \"Door of Doom v1.0\"\n" \
    "  }\n" \
    "}"


// --- SENSOR GLOBALS ---

#define SENSOR_PIN GPIO_NUM_4
#define LED_PIN    GPIO_NUM_2

static const char *SENSOR_TAG = "sensor";

static TaskHandle_t s_main_task_handle = NULL;
static TaskHandle_t s_sync_task_handle = NULL;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static volatile bool s_force_state_sync = false;


// --- WIFI GLOBALS ---

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_WIFI_MAXIMUM_RETRY  CONFIG_ESP_WIFI_MAXIMUM_RETRY

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about
 * being connected to the AP with an IP. */
#define WIFI_CONNECTED_BIT BIT0


static const char *WIFI_TAG = "wifi";

static int s_retry_num = 0;

static bool sensor_is_locked(int sensor_state)
{
    return sensor_state == 0;
}

static const char *sensor_state_to_mqtt_payload(int sensor_state)
{
    return sensor_is_locked(sensor_state) ? "OFF" : "ON";
}

static void publish_sensor_state(const char *reason)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL) {
        ESP_LOGW(MQTT_TAG, "Skipped state publish (%s), MQTT is disconnected", reason);
        return;
    }

    int sensor_state = gpio_get_level(SENSOR_PIN);
    const char *mqtt_sensor_state = sensor_state_to_mqtt_payload(sensor_state);
    int msg_id = esp_mqtt_client_publish(
        s_mqtt_client,
        CONFIG_MQTT_SENSOR_TOPIC,
        mqtt_sensor_state,
        0,
        MQTT_QOS,
        MQTT_RETAIN
    );

    if (msg_id < 0) {
        ESP_LOGW(MQTT_TAG, "State publish failed (%s)", reason);
        return;
    }

    ESP_LOGI(
        MQTT_TAG,
        "Published lock state=%s (%s), msg_id=%d",
        sensor_is_locked(sensor_state) ? "LOCKED" : "UNLOCKED",
        reason,
        msg_id
    );
}

static void publish_mqtt_discovery(void)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL) {
        return;
    }

    int msg_id = esp_mqtt_client_publish(
        s_mqtt_client,
        CONFIG_MQTT_SENSOR_CONFIG_TOPIC,
        MQTT_TOPIC_CONFIG,
        0,
        MQTT_QOS,
        MQTT_RETAIN
    );

    if (msg_id < 0) {
        ESP_LOGW(MQTT_TAG, "Discovery publish failed");
        return;
    }

    ESP_LOGI(MQTT_TAG, "Published Home Assistant discovery config, msg_id=%d", msg_id);
}

static void publish_availability(const char *payload)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL) {
        return;
    }

    int msg_id = esp_mqtt_client_publish(
        s_mqtt_client,
        MQTT_SENSOR_AVAILABILITY_TOPIC,
        payload,
        0,
        MQTT_QOS,
        MQTT_RETAIN
    );

    if (msg_id < 0) {
        ESP_LOGW(MQTT_TAG, "Availability publish failed");
        return;
    }

    ESP_LOGI(MQTT_TAG, "Published availability=%s, msg_id=%d", payload, msg_id);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt_connected = true;
            ESP_LOGI(MQTT_TAG, "MQTT connected");
            publish_mqtt_discovery();
            publish_availability("online");
            publish_sensor_state("mqtt connected");
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(MQTT_TAG, "MQTT disconnected");
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGW(MQTT_TAG, "MQTT error event received");
            break;

        default:
            break;
    }
}


// --- WIFI FUNCTIONS ---

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_retry_num < EXAMPLE_ESP_WIFI_MAXIMUM_RETRY) {
            s_retry_num++;
            ESP_LOGW(WIFI_TAG, "disconnected from AP, retry %d/%d", s_retry_num, EXAMPLE_ESP_WIFI_MAXIMUM_RETRY);
        } else {
            s_retry_num++;
            ESP_LOGW(WIFI_TAG, "disconnected from AP, retry %d (continuing to reconnect)", s_retry_num);
        }

        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "got ip:%s",
                 ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS
        },
    };

    /* Setting a password implies station will connect to all security modes including WEP/WPA.
        * However these modes are deprecated and not advisable to be used. Incase your Access point
        * doesn't support WPA2, these mode can be enabled by commenting below line */

    if (strlen((char *)wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

    /* Wait for the first successful connection. After that the event handlers stay
     * registered so the station keeps reconnecting on future outages. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    }
}


// --- SENSOR ISR FUNCTION ---

static void IRAM_ATTR sensor_isr_handler(void *arg)
{
    vTaskNotifyGiveFromISR(s_main_task_handle, NULL);
    portYIELD_FROM_ISR();
}

void debounce_sync_task(void *pvParameters) {

    TickType_t sleep_time = pdMS_TO_TICKS(atoi(CONFIG_DEBOUNCE_LIMIT_MS));

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (1) {

            uint32_t notified = ulTaskNotifyTake(pdTRUE, sleep_time);

            if ( !notified ) {
                xTaskNotifyGive(s_main_task_handle);
                break;
            }
        }
    }
}

void app_main()
{
    // --- GPIO CONFIG ---

    // Configure D2 (GPIO4) as input with pull-up
    gpio_config_t sensor_pin_conf = {
        .pin_bit_mask = (1ULL << SENSOR_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&sensor_pin_conf);

    // Configure LED pin (GPIO2) as output
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);

    s_main_task_handle = xTaskGetCurrentTaskHandle();
    gpio_set_level(LED_PIN, !gpio_get_level(SENSOR_PIN)); // set inital LED state
    gpio_install_isr_service(0);
    gpio_isr_handler_add(SENSOR_PIN, sensor_isr_handler, NULL);


    // --- WIFI CONFIG ---

    // NVS is required by the WiFi driver
    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init_sta();

    // wifi modem will sleep in small intervals saving power, this works because we are only using station mode
    // wifi_set_sleep_type(MODEM_SLEEP_T);


    // --- MQTT CONFIG ---

    const uint32_t debounce_limit_ms = (uint32_t)atoi(CONFIG_DEBOUNCE_LIMIT_MS);

    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_MQTT_BROKER_URL,
        .keepalive = MQTT_KEEPALIVE_SECONDS,
        .username = CONFIG_MQTT_USERNAME,
        .password = CONFIG_MQTT_PASSWORD,
        .disable_auto_reconnect = false,
        .lwt_topic = MQTT_SENSOR_AVAILABILITY_TOPIC,
        .lwt_msg = "offline",
        .lwt_qos = MQTT_QOS,
        .lwt_retain = MQTT_RETAIN
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);


    // --- DEBOUNCE CONFIG ---

    xTaskCreate(
        debounce_sync_task,
        "debounce_sync",
        2048,
        NULL,
        0,  // low prio idle task, should be preempted by main (prio 1)
        &s_sync_task_handle
    );


    // --- MAIN APP LOOP ---

    ESP_LOGI(SENSOR_TAG, "Activity sensor running...\n");

    TickType_t last_publish = xTaskGetTickCount();
    int sensor_state = gpio_get_level(SENSOR_PIN);
    int sensor_state_old = sensor_state;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool force_publish = s_force_state_sync;
        sensor_state = gpio_get_level(SENSOR_PIN);
        if (sensor_state == sensor_state_old && !force_publish)
            continue;
        else
            sensor_state_old = sensor_state;

        gpio_set_level(LED_PIN, !sensor_state);
        ESP_LOGI(
            SENSOR_TAG,
            "sensor raw=%s, lock=%s",
            sensor_state == 0 ? "LOW" : "HIGH",
            sensor_is_locked(sensor_state) ? "LOCKED" : "UNLOCKED"
        );

        // debounce to avoid spamming mqtt on sensor jitter
        uint32_t elapsed_ms = (xTaskGetTickCount() - last_publish) * portTICK_PERIOD_MS;
        if (!force_publish && elapsed_ms < debounce_limit_ms) {
            ESP_LOGI(SENSOR_TAG, "Debounce limit reached, skipped mqtt publish");

            // start a quiet-period timer so the final stable state is published after debounce
            s_force_state_sync = true;
            xTaskNotifyGive(s_sync_task_handle);
            continue;
        }

        publish_sensor_state(force_publish ? "debounce resync" : "state change");
        last_publish = xTaskGetTickCount();
        s_force_state_sync = false;
    }

}
