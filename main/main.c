#include <esp_log.h>
#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <string.h>
#include "driver/uart.h"
#include "esp32hwcontext.h"
#include "hwcontext.h"
#include "appcontext.h"
#include "shell.h"
#include "u8g2_esp32_hal.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "mqtt_config.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "message.h"
#include "commands.h"
#include "mbedtls/md.h"

#define PIN_CLK 19
#define PIN_MOSI 23
#define PIN_RESET 33
#define PIN_DC 14
#define PIN_CS 32

#define UART_EVENT_QUEUE_LEN 32
#define UART_NUM (UART_NUM_0)
#define UART_BUF_SIZE 1024
#define RD_BUF_SIZE (UART_BUF_SIZE)
#define PATTERN_CHR_NUM 2
#define CMD_IDX 1

#define HDR_KEY_DOWN 'D'
#define HDR_KEY_UP 'U'
#define KEY_IDX 2
#define KEY_MSG_LEN 3

#define HDR_SN 'S'
#define SN_MSG_LEN 16
#define SN_HEADER_LEN 4
#define SN_LEN 12

#define KEY_EVENTS_QUEUE_LEN 32

#define SMS_NOTIF_CMD "&&N3\n"

#define DEVICE_BASE_IP "10.0.40.0"
#define DEVICE_GW "10.0.43.254"
#define DEVICE_NETMASK "255.255.252.0"

#ifndef CONFIG_ESP_WIFI_SSID
#define CONFIG_ESP_WIFI_SSID "IHC.camp"
#endif

// Wifi password is not set
// #ifndef CONFIG_ESP_WIFI_PASSWORD
// #define CONFIG_ESP_WIFI_PASSWORD "IHC2018"
// #endif

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI "mqtts://broker.cafuddia.ml:8883"
#endif

static const char *wifi_tag = "WIFI";
static const char *mqtt_tag = "MQTT";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

static struct AppContext *appctx;

static QueueHandle_t uart0_queue;
static QueueHandle_t key_events_queue;

const char *uart_tag = "uart_events";

static void mqtt_app_start();

static void init_display(struct HWContext *hw_context) {
    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
    u8g2_esp32_hal.clk   = PIN_CLK;
    u8g2_esp32_hal.mosi  = PIN_MOSI;
    u8g2_esp32_hal.cs    = PIN_CS;
    u8g2_esp32_hal.dc    = PIN_DC;
    u8g2_esp32_hal.reset = PIN_RESET;
    u8g2_esp32_hal_init(u8g2_esp32_hal);

    u8g2_t *u8g2 = malloc(sizeof(u8g2_t));;
    u8g2_Setup_pcd8544_84x48_f(
        u8g2,
        U8G2_R0,
        u8g2_esp32_spi_byte_cb,
        u8g2_esp32_gpio_and_delay_cb);  // init u8g2 structure

    u8g2_InitDisplay(u8g2); // send init sequence to the display, display is in sleep mode after this,

    u8g2_SetPowerSave(u8g2, 0); // wake up display
    u8g2_SetFont(u8g2, u8g2_font_t0_11_t_all);
    u8g2_ClearBuffer(u8g2);

    hw_context->u8g2 = u8g2;
}

static void init_serial()
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, UART_EVENT_QUEUE_LEN, &uart0_queue, 0));
    ESP_ERROR_CHECK(uart_enable_pattern_det_intr(UART_NUM, '+', PATTERN_CHR_NUM, 10000, 10, 10));
    ESP_ERROR_CHECK(uart_pattern_queue_reset(UART_NUM, UART_EVENT_QUEUE_LEN));
}

static char *phone_number(const char *payload)
{
    uint8_t sha_result[32];

    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

    const size_t payload_length = strlen(payload);

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char *) payload, payload_length);
    mbedtls_md_finish(&ctx, sha_result);
    mbedtls_md_free(&ctx);

    char *tel_num = malloc(10);
    sprintf(tel_num, "%i%i%i", (int) sha_result[0], (int) sha_result[1], (int) sha_result[2]);

    return tel_num;
}

static void handle_serial_msg(uint8_t *msg)
{
    appctx->serial_number = strndup((char *)msg + SN_HEADER_LEN, SN_LEN);
    appctx->phone_number = phone_number(appctx->serial_number);

    // Now we have serial and phone, so we can start MQTT
    mqtt_app_start();
}

static void handle_key_msg(uint8_t *msg)
{
    struct KeyEvent ev;

    TickType_t ticks = xTaskGetTickCount();
    ev.timestamp.tv_sec = (ticks * portTICK_PERIOD_MS) / 1000;
    ev.timestamp.tv_nsec = ((ticks * portTICK_PERIOD_MS) % 1000) * 1000000;

    switch (msg[CMD_IDX]) {
        case 'D':
            ev.pressed = 1;
            break;

        case 'U':
            ev.pressed = 0;
            break;

        default:
            // Invalid
            return;
    }

    switch (msg[KEY_IDX]) {
        // Valid keys
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
        case '0':
        case '*':
        case '#':
        case 'U':
        case 'D':
        case 'C':
        case 'M':
            ev.key = msg[KEY_IDX];
            break;

        default:
            // Invalid
            return;
    }

    ESP_LOGI(uart_tag, "Received key: %c pressed: %d", ev.key, ev.pressed);
    xQueueSend(key_events_queue, (void *)&ev, portMAX_DELAY);
}

static void handle_msg(uint8_t *buf, int buf_len)
{
    // Find header
    int header_idx = 0;
    while (buf[header_idx] != '$' && header_idx < buf_len) {
        header_idx++;
    }

    if (header_idx == buf_len) {
        // Header not found
        return;
    }

    int msg_len = buf_len - header_idx;
    if (msg_len < 1) {
        // No CMD_IDX
        return;
    }

    uint8_t *msg = malloc(msg_len);
    memcpy(msg, buf, msg_len);

    switch (msg[CMD_IDX]) {
        case HDR_KEY_DOWN:
        case HDR_KEY_UP:
            if (msg_len < KEY_MSG_LEN) {
                break;
            }
            handle_key_msg(msg);
            break;

        case HDR_SN:
            if (msg_len < SN_MSG_LEN) {
                break;
            }
            handle_serial_msg(msg);
            break;

        default:
            ESP_LOGI(uart_tag, "Received unhandled message type: %c", msg[CMD_IDX]);
    }

    free(msg);
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    struct AppContext *appctx = (struct AppContext *)event->user_context;

    char *messages_topic = malloc(32);
    sprintf(messages_topic, "n/%s/messages", appctx->phone_number);

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(mqtt_tag, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, "ihc/bcast", 2);
            ESP_LOGI(mqtt_tag, "sent subscribe successful, msg_id=%d", msg_id);
            msg_id = esp_mqtt_client_subscribe(client, messages_topic, 2);
            ESP_LOGI(mqtt_tag, "sent subscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(mqtt_tag, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(mqtt_tag, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(mqtt_tag, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(mqtt_tag, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(mqtt_tag, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            struct Message *msg = calloc(1, sizeof(struct Message));
            msg->topic = strdup(event->topic);
            msg->topic_len = event->topic_len;
            msg->data = strdup(event->data);
            msg->data_len = event->data_len;
            linkedlist_append(&appctx->msgs, &msg->message_list_head);
            uart_write_bytes(UART_NUM, SMS_NOTIF_CMD, strlen(SMS_NOTIF_CMD));
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(mqtt_tag, "MQTT_EVENT_ERROR");
            break;
    }
    return ESP_OK;
}

static void mqtt_app_start()
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_MQTT_BROKER_URI,
        .event_handle = mqtt_event_handler,
        .user_context = appctx,
        .client_id = appctx->serial_number,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
}

static void uart_event_task(void *pvParameters)
{

    uart_event_t event;
    size_t buffered_size;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
    while (1) {
        //Waiting for UART event.
        if(xQueueReceive(uart0_queue, (void * )&event, (portTickType)portMAX_DELAY)) {
            memset(dtmp, 0, RD_BUF_SIZE);
            ESP_LOGI(uart_tag, "uart[%d] event:", UART_NUM);
            switch(event.type) {
                //UART_PATTERN_DET
                case UART_PATTERN_DET:
                    uart_get_buffered_data_len(UART_NUM, &buffered_size);
                    int pos = uart_pattern_pop_pos(UART_NUM);
                    if (pos < -1) {
                        // There used to be a UART_PATTERN_DET event, but the pattern position queue is full so that it can not
                        // record the position. We should set a larger queue size.
                        // As an example, we directly flush the rx buffer here.
                        uart_flush_input(UART_NUM);
                    } else {
                        uart_read_bytes(UART_NUM, dtmp, pos + PATTERN_CHR_NUM, 100 / portTICK_PERIOD_MS);
                        handle_msg(dtmp, pos + PATTERN_CHR_NUM);
                    }
                    break;
                case UART_DATA:
                    // If it's not a pattern we just read it and throw it away
                    uart_read_bytes(UART_NUM, dtmp, event.size, portMAX_DELAY);
                    break;
                //Event of HW FIFO overflow detected
                case UART_FIFO_OVF:
                    ESP_LOGI(uart_tag, "hw fifo overflow");
                    // If fifo overflow happened, you should consider adding flow control for your application.
                    // The ISR has already reset the rx FIFO,
                    // As an example, we directly flush the rx buffer here in order to read more data.
                    uart_flush_input(UART_NUM);
                    xQueueReset(uart0_queue);
                    break;
                //Event of UART ring buffer full
                case UART_BUFFER_FULL:
                    ESP_LOGI(uart_tag, "ring buffer full");
                    // If buffer full happened, you should consider encreasing your buffer size
                    // As an example, we directly flush the rx buffer here in order to read more data.
                    uart_flush_input(UART_NUM);
                    xQueueReset(uart0_queue);
                    break;
                //Event of UART RX break detected
                case UART_BREAK:
                    ESP_LOGI(uart_tag, "uart rx break");
                    break;
                //Event of UART parity check error
                case UART_PARITY_ERR:
                    ESP_LOGI(uart_tag, "uart parity error");
                    break;
                //Event of UART frame error
                case UART_FRAME_ERR:
                    ESP_LOGI(uart_tag, "uart frame error");
                    break;
                //Others
                default:
                    ESP_LOGI(uart_tag, "uart event type: %d", event.type);
                    break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

static void init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            ESP_LOGI(wifi_tag, "SYSTEM_EVENT_STA_START received.");
            esp_wifi_connect();
            break;

        case SYSTEM_EVENT_STA_GOT_IP:
            ESP_LOGI(wifi_tag, "SYSTEM_EVENT_STA_GOT_IP received");
            ESP_LOGI(wifi_tag, "IP: %s", inet_ntoa(event->event_info.got_ip.ip_info.ip));
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

            ip_addr_t dns_server;
            inet_pton(AF_INET, "8.8.8.8", &dns_server);
            dns_setserver(0, &dns_server);
            break;

        case SYSTEM_EVENT_STA_CONNECTED:
            ESP_LOGI(wifi_tag, "SYSTEM_EVENT_STA_CONNECTED received.");
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            ESP_LOGI(wifi_tag, "SYSTEM_EVENT_STA_DISCONNECTED received.");
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;

        default:
            ESP_LOGI(wifi_tag, "Unhandled wifi event: %i.", event->event_id);
            break;
    }
    return ESP_OK;
}

static void init_wifi(void)
{
    char rand_ip[20];
    tcpip_adapter_init();
    tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
    tcpip_adapter_ip_info_t ipInfo;

    sprintf(rand_ip, "10.0.%i.%i", 40 + (rand() % 4), 1 + (rand() % 250));

    inet_pton(AF_INET, rand_ip, &ipInfo.ip);
    inet_pton(AF_INET, DEVICE_GW, &ipInfo.gw);
    inet_pton(AF_INET, DEVICE_NETMASK, &ipInfo.netmask);
    tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            // NOT SET: .password = CONFIG_ESP_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(wifi_tag, "start the WIFI SSID:[%s] password:[%s]", CONFIG_ESP_WIFI_SSID, "******");
    ESP_ERROR_CHECK(esp_wifi_start());

    // Use following snippet to wait for connection:
    // xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

void app_main()
{
    const char tag[] = "app_main";

    ESP_LOGI(tag, "*Nokia tune intensifies*");

    key_events_queue = xQueueCreate(KEY_EVENTS_QUEUE_LEN, sizeof(struct KeyEvent));
    if (!key_events_queue) {
        ESP_LOGE(tag, "Key events queue creation failed");
    }

    struct HWContext *hw_context = malloc(sizeof(struct HWContext));
    hw_context->key_events_queue = key_events_queue;
    init_display(hw_context);
    init_serial();
    init_nvs();
    srand(esp_random());
    init_wifi();

    appctx = malloc(sizeof(struct AppContext));
    appctx->hwcontext = hw_context;
    appctx->user_name = NULL;
    appctx->msgs = NULL;
    appctx->serial_number = NULL;
    appctx->phone_number = NULL;

    hwcontext_send_command(hw_context, SN_CMD, "0");

    xTaskCreatePinnedToCore(shell_main, "shell_main", 8192, appctx, 5, NULL, 1);
    xTaskCreatePinnedToCore(uart_event_task, "uart_event_task", 2048, NULL, 5, NULL, 1);
}
