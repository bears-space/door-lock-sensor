/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
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

#define MQTT_TOPIC_CONFIG \
    "{\n" \
    "  \"name\": \"Bears Door\",\n" \
    "  \"unique_id\": \"bears_door_lock_1\",\n" \
    "  \"device_class\": \"door\",\n" \
    "  \"state_topic\": \"" CONFIG_MQTT_SENSOR_TOPIC "\",\n" \
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

static TaskHandle_t main_task_handle = NULL;


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

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1


static const char *WIFI_TAG = "wifi";

static int s_retry_num = 0;


// --- WIFI FUNCTIONS ---

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(WIFI_TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(WIFI_TAG,"connect to the AP fail");
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

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(WIFI_TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    }

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    vEventGroupDelete(s_wifi_event_group);
}


// --- SENSOR ISR FUNCTION ---

static void IRAM_ATTR sensor_isr_handler(void *arg)
{
    vTaskNotifyGiveFromISR(main_task_handle, NULL);
    portYIELD_FROM_ISR();
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

    main_task_handle = xTaskGetCurrentTaskHandle();
    gpio_set_level(LED_PIN, !gpio_get_level(SENSOR_PIN)); // set inital LED state
    gpio_install_isr_service(0);
    gpio_isr_handler_add(SENSOR_PIN, sensor_isr_handler, NULL);


    // --- WIFI CONFIG ---

    // NVS is required by the WiFi driver
    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init_sta();

    // wifi modem will sleep in small intervals saving power, this works because we are only using station mode
    // wifi_set_sleep_type(MODEM_SLEEP_T);


    // -- MQTT CONFIG ---

    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_MQTT_BROKER_URL,
        .username = CONFIG_MQTT_USERNAME,
        .password = CONFIG_MQTT_PASSWORD
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    // don't need event handler (see mqtt example for event handler)
    esp_mqtt_client_start(client);

    int msg_id = esp_mqtt_client_publish(client, CONFIG_MQTT_SENSOR_CONFIG_TOPIC, MQTT_TOPIC_CONFIG, 0, 1, 0);


    // --- MAIN APP LOOP ---

    ESP_LOGI(SENSOR_TAG, "Activity sensor running...\n");

    TickType_t last_publish = xTaskGetTickCount();

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int sensor_state = gpio_get_level(SENSOR_PIN);

        gpio_set_level(LED_PIN, !sensor_state);
        ESP_LOGI(SENSOR_TAG, "sensor state -> %s\n", sensor_state == 0 ? "LOW (LED off)" : "HIGH (LED on)");

        // debounce to avoid spamming mqtt on sensor jitter
        uint32_t elapsed_ms = (xTaskGetTickCount() - last_publish) * portTICK_PERIOD_MS;
        if (elapsed_ms < atoi(CONFIG_DEBOUNCE_LIMIT_MS)) {
            ESP_LOGI(SENSOR_TAG, "Debounce limit reached, skipped mqtt publish");
            continue;
        }

        char *mqtt_sensor_state;
        mqtt_sensor_state = sensor_state == 0 ? "ON" : "OFF"; // the sensor being low means the door is closed -> the door lock is on

        msg_id = esp_mqtt_client_publish(client, CONFIG_MQTT_SENSOR_TOPIC, mqtt_sensor_state, 0, 1, 0);
        ESP_LOGI(MQTT_TAG, "Sent publish successful, msg_id=%d", msg_id);
        last_publish = xTaskGetTickCount();
    }

}
