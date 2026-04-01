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

#define MQTT_DOOR_TOPIC_CONFIG \
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

#define MQTT_COFFEEBREAK_TOPIC_CONFIG \
    "{\n" \
    "  \"name\": \"Coffee Break\",\n" \
    "  \"unique_id\": \"bears_door_coffeebrake_1\",\n" \
    "  \"state_topic\": \"" CONFIG_MQTT_COFFEEBREAK_SENSOR_TOPIC "\",\n" \
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
#define BUTTON_PIN GPIO_NUM_0
#define LED_PIN    GPIO_NUM_2

#define INPUT_POLL_INTERVAL_MS           20
#define BUTTON_DEBOUNCE_MS               50
#define BUTTON_HOLD_CANCEL_MS            5000
#define COFFEE_ARM_WINDOW_MS             (5 * 60 * 1000)
#define COFFEE_ACTIVE_TIMEOUT_MS         (90 * 60 * 1000)
#define LED_FAST_FLASH_HALF_PERIOD_MS    150
#define LED_SLOW_FLASH_HALF_PERIOD_MS    1000

static const char *SENSOR_TAG = "sensor";

typedef enum {
    COFFEE_MODE_NORMAL = 0,
    COFFEE_MODE_ARMED,
    COFFEE_MODE_ACTIVE,
} coffee_mode_t;

typedef struct {
    int raw_level;
    int stable_level;
    TickType_t last_raw_change_tick;
} debounced_input_t;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static volatile int s_door_sensor_level = 1;
static volatile coffee_mode_t s_coffee_mode = COFFEE_MODE_NORMAL;

static bool s_button_pressed = false;
static bool s_button_hold_handled = false;
static TickType_t s_button_pressed_tick = 0;
static TickType_t s_coffee_mode_started_tick = 0;


// --- WIFI GLOBALS ---

/* Wi-Fi credentials and retry count are configured via menuconfig. */
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


// --- APP HELPERS ---

static const char *coffee_mode_to_string(coffee_mode_t mode)
{
    switch (mode) {
        case COFFEE_MODE_NORMAL:
            return "normal";
        case COFFEE_MODE_ARMED:
            return "armed";
        case COFFEE_MODE_ACTIVE:
            return "active";
        default:
            return "unknown";
    }
}

static bool door_is_unlocked(int sensor_level)
{
    return sensor_level == 0;
}

static bool ticks_elapsed(TickType_t now, TickType_t since, TickType_t duration)
{
    return (TickType_t)(now - since) >= duration;
}

static const char *door_state_to_mqtt_payload(int sensor_level)
{
    return door_is_unlocked(sensor_level) ? "ON" : "OFF";
}

static bool publish_mqtt_message(const char *topic, const char *payload, const char *reason)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL) {
        ESP_LOGW(MQTT_TAG, "Skipped publish to %s (%s), MQTT is disconnected", topic, reason);
        return false;
    }

    int msg_id = esp_mqtt_client_publish(
        s_mqtt_client,
        topic,
        payload,
        0,
        MQTT_QOS,
        MQTT_RETAIN
    );

    if (msg_id < 0) {
        ESP_LOGW(MQTT_TAG, "Publish failed for %s (%s)", topic, reason);
        return false;
    }

    ESP_LOGI(MQTT_TAG, "Published %s=%s (%s), msg_id=%d", topic, payload, reason, msg_id);
    return true;
}

static void publish_door_state(const char *reason)
{
    int sensor_level = s_door_sensor_level;
    const char *payload = door_state_to_mqtt_payload(sensor_level);

    publish_mqtt_message(CONFIG_MQTT_SENSOR_TOPIC, payload, reason);
}

static void publish_coffeebrake_state(bool active, const char *reason)
{
    publish_mqtt_message(
        CONFIG_MQTT_COFFEEBREAK_SENSOR_TOPIC,
        active ? "ON" : "OFF",
        reason
    );
}

static void publish_mqtt_discovery(void)
{
    publish_mqtt_message(CONFIG_MQTT_SENSOR_CONFIG_TOPIC, MQTT_DOOR_TOPIC_CONFIG, "discovery door");
    publish_mqtt_message(CONFIG_MQTT_COFFEEBREAK_SENSOR_CONFIG_TOPIC, MQTT_COFFEEBREAK_TOPIC_CONFIG, "discovery coffeebrake");
}

static void publish_runtime_state(const char *reason)
{
    if (s_coffee_mode == COFFEE_MODE_ACTIVE) {
        publish_coffeebrake_state(true, reason);
        return;
    }

    publish_coffeebrake_state(false, reason);
    publish_door_state(reason);
}

static void set_coffee_mode(coffee_mode_t new_mode, TickType_t now, const char *reason)
{
    if (s_coffee_mode == new_mode) {
        return;
    }

    s_coffee_mode = new_mode;
    s_coffee_mode_started_tick = now;
    ESP_LOGI(SENSOR_TAG, "coffee mode -> %s (%s)", coffee_mode_to_string(new_mode), reason);
}

static void update_led(TickType_t now)
{
    bool led_on = false;

    switch (s_coffee_mode) {
        case COFFEE_MODE_NORMAL:
            led_on = door_is_unlocked(s_door_sensor_level);
            break;

        case COFFEE_MODE_ARMED:
            led_on = ((now / pdMS_TO_TICKS(LED_FAST_FLASH_HALF_PERIOD_MS)) & 1U) == 0;
            break;

        case COFFEE_MODE_ACTIVE:
            led_on = ((now / pdMS_TO_TICKS(LED_SLOW_FLASH_HALF_PERIOD_MS)) & 1U) == 0;
            break;
    }

    gpio_set_level(LED_PIN, led_on ? 1 : 0);
}

static void debounced_input_init(debounced_input_t *input, gpio_num_t pin)
{
    int level = gpio_get_level(pin);
    input->raw_level = level;
    input->stable_level = level;
    input->last_raw_change_tick = xTaskGetTickCount();
}

static bool debounced_input_update(
    debounced_input_t *input,
    gpio_num_t pin,
    TickType_t now,
    TickType_t debounce_ticks,
    int *new_stable_level
)
{
    int raw_level = gpio_get_level(pin);

    if (raw_level != input->raw_level) {
        input->raw_level = raw_level;
        input->last_raw_change_tick = now;
    }

    if (input->stable_level != input->raw_level &&
        ticks_elapsed(now, input->last_raw_change_tick, debounce_ticks)) {
        input->stable_level = input->raw_level;
        *new_stable_level = input->stable_level;
        return true;
    }

    return false;
}

static void handle_button_press(TickType_t now)
{
    s_button_pressed = true;
    s_button_hold_handled = false;
    s_button_pressed_tick = now;

    if (s_coffee_mode == COFFEE_MODE_NORMAL && door_is_unlocked(s_door_sensor_level)) {
        set_coffee_mode(COFFEE_MODE_ARMED, now, "button press");
    }
}

static void handle_button_release(void)
{
    s_button_pressed = false;
    s_button_hold_handled = false;
}

static void handle_door_change(int sensor_level, TickType_t now)
{
    s_door_sensor_level = sensor_level;

    ESP_LOGI(
        SENSOR_TAG,
        "door -> %s, coffee mode=%s",
        door_is_unlocked(sensor_level) ? "UNLOCKED" : "LOCKED",
        coffee_mode_to_string(s_coffee_mode)
    );

    if (s_coffee_mode == COFFEE_MODE_ARMED && !door_is_unlocked(sensor_level)) {
        set_coffee_mode(COFFEE_MODE_ACTIVE, now, "door locked during coffee arm window");
        publish_coffeebrake_state(true, "coffee break started");
        return;
    }

    if (s_coffee_mode == COFFEE_MODE_ACTIVE && door_is_unlocked(sensor_level)) {
        publish_coffeebrake_state(false, "coffee break ended by unlock");
        set_coffee_mode(COFFEE_MODE_NORMAL, now, "door unlocked during coffee break");
        return;
    }

    publish_coffeebrake_state(false, "normal door change");
    publish_door_state("normal door change");
}

static void process_coffee_mode_timers(TickType_t now)
{
    if (s_coffee_mode == COFFEE_MODE_ARMED &&
        s_button_pressed &&
        !s_button_hold_handled &&
        ticks_elapsed(now, s_button_pressed_tick, pdMS_TO_TICKS(BUTTON_HOLD_CANCEL_MS))) {
        s_button_hold_handled = true;
        set_coffee_mode(COFFEE_MODE_NORMAL, now, "button hold cancel");
    }

    if (s_coffee_mode == COFFEE_MODE_ARMED &&
        ticks_elapsed(now, s_coffee_mode_started_tick, pdMS_TO_TICKS(COFFEE_ARM_WINDOW_MS))) {
        set_coffee_mode(COFFEE_MODE_NORMAL, now, "coffee arm timeout");
    }

    if (s_coffee_mode == COFFEE_MODE_ACTIVE &&
        ticks_elapsed(now, s_coffee_mode_started_tick, pdMS_TO_TICKS(COFFEE_ACTIVE_TIMEOUT_MS))) {
        publish_coffeebrake_state(false, "coffee break timeout");
        publish_door_state("coffee break timeout");
        set_coffee_mode(COFFEE_MODE_NORMAL, now, "coffee break timeout");
    }
}


// --- WIFI FUNCTIONS ---

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    (void)arg;

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

    if (strlen((char *)wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

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


// --- MQTT FUNCTIONS ---

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
            publish_mqtt_message(MQTT_SENSOR_AVAILABILITY_TOPIC, "online", "mqtt connected");
            publish_runtime_state("mqtt connected");
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


void app_main()
{
    const TickType_t input_poll_ticks = pdMS_TO_TICKS(INPUT_POLL_INTERVAL_MS);
    const TickType_t sensor_debounce_ticks = pdMS_TO_TICKS(atoi(CONFIG_DEBOUNCE_LIMIT_MS));
    const TickType_t button_debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);

    // --- GPIO CONFIG ---

    // D2 (GPIO4) is the door sensor input, active low with pull-up
    gpio_config_t sensor_pin_conf = {
        .pin_bit_mask = (1ULL << SENSOR_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&sensor_pin_conf);

    // D3 is GPIO0 on ESP8266, so keep it pulled high except when the button is pressed
    gpio_config_t button_pin_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&button_pin_conf);

    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);

    s_door_sensor_level = gpio_get_level(SENSOR_PIN);
    update_led(xTaskGetTickCount());


    // --- WIFI CONFIG ---

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();


    // --- MQTT CONFIG ---

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


    // --- INPUT LOOP ---

    debounced_input_t door_input;
    debounced_input_t button_input;

    debounced_input_init(&door_input, SENSOR_PIN);
    debounced_input_init(&button_input, BUTTON_PIN);

    ESP_LOGI(SENSOR_TAG, "Door lock sensor running...");

    while (1) {
        TickType_t now = xTaskGetTickCount();
        int new_level = 0;

        if (debounced_input_update(&door_input, SENSOR_PIN, now, sensor_debounce_ticks, &new_level)) {
            handle_door_change(new_level, now);
        }

        if (debounced_input_update(&button_input, BUTTON_PIN, now, button_debounce_ticks, &new_level)) {
            if (new_level == 0) {
                handle_button_press(now);
            } else {
                handle_button_release();
            }
        }

        process_coffee_mode_timers(now);
        update_led(now);
        vTaskDelay(input_poll_ticks);
    }
}
