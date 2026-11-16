#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "input_logic.h"

/* 
 * LED: GPIO -> resistor de 330 ohms -> LED -> GND.
 * Botão: GPIO -> botão -> GND; o pull-up interno mantém a entrada em 1.
 */
#define APP_WIFI_SSID     ""
#define APP_WIFI_PASSWORD ""
#define APP_MQTT_URI      "mqtt://broker.hivemq.com:1883"
#define APP_LED_GPIO      2
#define APP_BUTTON_GPIO   4
#define APP_STATUS_TOPIC  "room1/esp32/status"

/* Bit compartilhado entre os callbacks de rede e as tarefas do botão. */
#define MQTT_CONNECTED BIT0
static const char *TAG = "mqtt_prototype";
static EventGroupHandle_t connection;
static esp_mqtt_client_handle_t mqtt;
static esp_timer_handle_t wifi_retry;
static bool mqtt_started;
static char client_id[32];
static button_state_t button;
static QueueHandle_t button_events;
/* Cada desconexão incrementa este contador. A fila leva uma cópia do valor
 * para identificar eventos antigos. O acesso é atômico pois há várias tarefas.
 */
static atomic_uint connection_epoch;
/* Último estado aplicado ao LED; o botão alterna a partir dele. */
static atomic_bool led_on;

/* Agenda uma tentativa única em 5 segundos, sem bloquear o loop de eventos.
 * INVALID_STATE significa que já existe uma tentativa agendada.
 */
static void schedule_wifi_retry(void)
{
    esp_err_t err = esp_timer_start_once(wifi_retry, 5000000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Cannot schedule Wi-Fi retry: %s", esp_err_to_name(err));
    }
}

/* Inicia a associação ao ponto de acesso. O resultado da conexão chega
 * depois, pelos eventos Wi-Fi/IP; falhas imediatas também agendam nova tentativa.
 */
static void connect_wifi(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(err));
        schedule_wifi_retry();
    }
}

/* O MQTT só inicia após obter IP. Depois disso, sua própria biblioteca cuida
 * das reconexões; a aplicação continua responsável por reconectar o Wi-Fi.
 */
static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        connect_wifi(NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(connection, MQTT_CONNECTED);
        atomic_fetch_add(&connection_epoch, 1);
        ESP_LOGW(TAG, "Wi-Fi disconnected; retry in 5 seconds");
        schedule_wifi_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        (void)esp_timer_stop(wifi_retry);
        ESP_LOGI(TAG, "Wi-Fi has IP");
        if (!mqtt_started) {
            ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
            mqtt_started = true;
        }
    }
}

/* Callback do cliente MQTT: renova a assinatura, acompanha desconexões
 * e aplica ao LED apenas comandos completos e válidos.
 */
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED: {
        xEventGroupSetBits(connection, MQTT_CONNECTED);
        /* Assinar novamente também cobre a perda da sessão no broker. */
        int msg_id = esp_mqtt_client_subscribe(mqtt, "room1/light", 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "Subscription failed; reconnecting to retry");
            xEventGroupClearBits(connection, MQTT_CONNECTED);
            atomic_fetch_add(&connection_epoch, 1);
            ESP_ERROR_CHECK(esp_mqtt_client_disconnect(mqtt));
        } else {
            ESP_LOGI(TAG, "MQTT connected; subscription requested (%d)", msg_id);
            /* Substitui o "offline" retido pelo LWT de uma queda anterior. */
            if (esp_mqtt_client_publish(mqtt, APP_STATUS_TOPIC, "online", 0, 1, 1) < 0) {
                ESP_LOGW(TAG, "Status publish failed");
            }
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(connection, MQTT_CONNECTED);
        atomic_fetch_add(&connection_epoch, 1);
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA: {
        /* Os buffers MQTT têm tamanho explícito e podem não terminar em NUL. */
        int level = light_command(event->topic, event->topic_len,
                                  event->data, event->data_len,
                                  event->total_data_len, event->current_data_offset);
        if (level < 0) {
            ESP_LOGW(TAG, "Ignoring invalid topic, payload or fragmented command");
        } else {
            ESP_ERROR_CHECK(gpio_set_level(APP_LED_GPIO, level));
            atomic_store(&led_on, level);
            ESP_LOGI(TAG, "LED %s", level ? "ON" : "OFF");
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT transport/protocol error");
        break;
    default:
        break;
    }
}

/* Publicar pode aguardar a rede. Esta tarefa separada permite que a leitura
 * do botão continue a cada 10 ms, mesmo quando o broker demora a responder.
 */
static void publish_task(void *arg)
{
    (void)arg;
    unsigned epoch;
    for (;;) {
        /* Dorme sem consumir CPU até chegar um pressionamento confirmado. */
        if (xQueueReceive(button_events, &epoch, portMAX_DELAY) != pdTRUE) continue;
        if (epoch != atomic_load(&connection_epoch) ||
            !(xEventGroupGetBits(connection) & MQTT_CONNECTED)) {
            ESP_LOGI(TAG, "Button event discarded: connection changed");
            continue;
        }
        /* Publica o estado oposto em room1/light. O ESP32 assina esse tópico,
         * então o LED só muda quando o comando volta do broker, como qualquer
         * outro comando. QoS 0: sem confirmação nem reenvio; retain = 0.
         * ponytail: dois toques antes do retorno do broker repetem o mesmo
         * comando; inverta led_on aqui se isso incomodar.
         */
        const char *command = atomic_load(&led_on) ? "OFF" : "ON";
        int result = esp_mqtt_client_publish(mqtt, "room1/light", command,
                                             strlen(command), 0, 0);
        if (result < 0) ESP_LOGW(TAG, "Button publish failed (%d)", result);
        else ESP_LOGI(TAG, "Published %s", command);
    }
}

/* A entrada é ativa em nível baixo. O filtro de debounce só devolve true
 * uma vez por pressionamento estável, após uma soltura também estável.
 */
static void button_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        bool pressed = gpio_get_level(APP_BUTTON_GPIO) == 0;
        if (button_sample(&button, pressed, esp_timer_get_time() / 1000)) {
            unsigned epoch = atomic_load(&connection_epoch);
            if (xEventGroupGetBits(connection) & MQTT_CONNECTED) {
                /* Espera zero: fila cheia descarta o evento sem atrasar a leitura. */
                if (xQueueSend(button_events, &epoch, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Button event discarded: publisher busy");
                }
            } else {
                ESP_LOGI(TAG, "Button event discarded: MQTT offline");
            }
        }
        /* Usa a referência anterior para manter a cadência de amostragem. */
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    /* 1. Prepara o hardware antes de iniciar a rede. No ESP32 clássico,
     * GPIOs somente de entrada (34–39) não oferecem o pull-up necessário.
     * ESP_ERROR_CHECK interrompe a execução quando uma inicialização falha.
     */
    ESP_ERROR_CHECK((GPIO_IS_VALID_OUTPUT_GPIO(APP_LED_GPIO) &&
                     GPIO_IS_VALID_OUTPUT_GPIO(APP_BUTTON_GPIO) &&
                     APP_LED_GPIO != APP_BUTTON_GPIO) ? ESP_OK : ESP_ERR_INVALID_ARG);
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << APP_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_set_level(APP_LED_GPIO, 0));
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_config_t input = {
        .pin_bit_mask = 1ULL << APP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input));
    /* Captura o estado inicial para não gerar evento se ligar segurando o botão. */
    button = button_init(gpio_get_level(APP_BUTTON_GPIO) == 0,
                         esp_timer_get_time() / 1000);

    if (strlen(APP_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "Configure APP_WIFI_SSID in main/main.c");
        return;
    }
    /* 2. Inicializa a NVS usada pelo ESP-IDF. Se estiver incompatível ou sem
     * páginas livres, apaga a partição NVS e tenta inicializá-la novamente.
     */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    /* 3. Cria a interface station (cliente Wi-Fi) e o loop de eventos. */
    connection = xEventGroupCreate();
    ESP_ERROR_CHECK(connection ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* Evita persistência adicional das credenciais pelo driver Wi-Fi. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* 4. O MAC gera um client ID diferente para cada placa no mesmo broker. */
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(client_id, sizeof(client_id), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = APP_MQTT_URI,
        .credentials.client_id = client_id,
        /* LWT: se a placa cair sem desconectar, o broker publica "offline"
         * por ela após o keepalive expirar. Retido para novos assinantes.
         */
        .session.last_will = {
            .topic = APP_STATUS_TOPIC,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 1000,
    };
    mqtt = esp_mqtt_client_init(&config);
    ESP_ERROR_CHECK(mqtt ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL));
    /* 5. Registra timer e callbacks antes de disparar a conexão. */
    const esp_timer_create_args_t retry_args = {
        .callback = connect_wifi,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &wifi_retry));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    /* Os campos do driver têm tamanho fixo: validar antes de copiar evita
     * truncar silenciosamente SSID/senha ou escrever além do buffer.
     */
    wifi_config_t wifi = {0};
    size_t ssid_len = strlen(APP_WIFI_SSID);
    size_t password_len = strlen(APP_WIFI_PASSWORD);
    ESP_ERROR_CHECK((ssid_len <= sizeof(wifi.sta.ssid) &&
                     password_len <= sizeof(wifi.sta.password)) ? ESP_OK : ESP_ERR_INVALID_ARG);
    memcpy(wifi.sta.ssid, APP_WIFI_SSID, ssid_len);
    memcpy(wifi.sta.password, APP_WIFI_PASSWORD, password_len);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    /* 6. Cria a fila e as tarefas antes de habilitar os eventos de rede. */
    button_events = xQueueCreate(1, sizeof(unsigned));
    ESP_ERROR_CHECK(button_events ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(publish_task, "button_publish", 4096, NULL, 5, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(button_task, "button", 4096, NULL, 5, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_wifi_start());
}
