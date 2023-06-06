#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "mqtt_client.h"

#define WIFI_SSID "rogo"
#define WIFI_PASSWORD "123456789"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define TAG_WIFI "[WIFI STATION]"
#define TAG_MQTT "[MQTT HANDLER]"

#define MQTT_BROKER_URI "mqtt://broker.hivemq.com"
#define MQTT_TOPIC "/__GK__/hello"

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
        {
            if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
            {
                esp_wifi_connect();
            }
            else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
            {
                if (s_retry_num < 3)
                {
                    esp_wifi_connect();
                    s_retry_num++;
                    ESP_LOGI(TAG_WIFI, "retry to connect to the AP");
                }
                else
                {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                ESP_LOGI(TAG_WIFI, "connect to the AP fail");
            }
            else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
            {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                ESP_LOGI(TAG_WIFI, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                s_retry_num = 0;
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }


class WIFI_Handler {

    public:

        WIFI_Handler() {
            s_retry_num = 0;
        }

        void init_sta(void) {

            s_wifi_event_group = xEventGroupCreate();

            ESP_ERROR_CHECK(esp_netif_init());

            ESP_ERROR_CHECK(esp_event_loop_create_default());
            esp_netif_create_default_wifi_sta();

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg));

            esp_event_handler_instance_t instance_any_id;
            esp_event_handler_instance_t instance_got_ip;
            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                &event_handler,
                                                                NULL,
                                                                &instance_any_id));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                                IP_EVENT_STA_GOT_IP,
                                                                &event_handler,
                                                                NULL,
                                                                &instance_got_ip));

            wifi_config_t wifi_config = {
                .sta = {
                    .ssid = WIFI_SSID,
                    .password = WIFI_PASSWORD,
                    .threshold = {
                        .authmode = WIFI_AUTH_WPA2_PSK
                    }
                },
            };

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());

            ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");

            /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
            * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                pdFALSE,
                                                pdFALSE,
                                                portMAX_DELAY);

            /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
            * happened. */
            if (bits & WIFI_CONNECTED_BIT)
            {
                ESP_LOGI(TAG_WIFI, "connected to ap SSID:%s password:%s",
                        WIFI_SSID, WIFI_PASSWORD);
            }
            else if (bits & WIFI_FAIL_BIT)
            {
                ESP_LOGI(TAG_WIFI, "Failed to connect to SSID:%s, password:%s",
                        WIFI_SSID, WIFI_PASSWORD);
            }
            else
            {
                ESP_LOGE(TAG_WIFI, "UNEXPECTED EVENT");
            }

            /* The event will not be processed after unregister */
            ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
            ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
            vEventGroupDelete(s_wifi_event_group);
        }
}; 

esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, MQTT_TOPIC, 0);
            ESP_LOGI(TAG_MQTT, "sent subscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_DATA");
            // Handle received MQTT data
            // event->topic contains the topic, and event->data contains the payload
            if (strcmp(event->data, "ping") == 0) {
                ESP_LOGI(TAG_MQTT, "ping received from topic");
                msg_id = esp_mqtt_client_publish(client, MQTT_TOPIC, "pong", 0, 1, 0);
                ESP_LOGI(TAG_MQTT, "Sent pong : %d", msg_id);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

class MQTT_Handler {

    public:

       static void event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
            ESP_LOGI(TAG_MQTT, "Event detected, base : %s, event_id : %d", base, (int)event_id);
            mqtt_event_handler_cb(static_cast<esp_mqtt_event_handle_t>(event_data));
        }

        void start(void) {
            esp_mqtt_client_config_t mqtt_config = {
                .broker = {
                    .address = {
                        .uri = MQTT_BROKER_URI,
                    },
                },
            };
            esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_config);
            esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, event_handler, client);

        }

};



extern "C" void app_main() {

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG_WIFI, "Lauching wifi connection");
    WIFI_Handler wifi = WIFI_Handler();
    wifi.init_sta();

    MQTT_Handler mqtt = MQTT_Handler();
    mqtt.start();
}

