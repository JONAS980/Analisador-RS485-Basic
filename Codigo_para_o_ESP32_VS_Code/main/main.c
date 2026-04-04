/*
 * Analisador RS485 - Modbus RTU (Módulo Injetor Escravo)
 * Comunicação de Parametrização via Bluetooth SPP
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "time.h"
#include "sys/time.h"

// --- Bibliotecas adicionais para o RS485 ---
#include "driver/uart.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h" // Para ets_delay_us

// --- Definições do Bluetooth ---
#define SPP_TAG "SPP_ACCEPTOR_DEMO"
#define SPP_SERVER_NAME "SPP_SERVER"
static const char local_device_name[] = CONFIG_EXAMPLE_LOCAL_DEVICE_NAME;
static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB;
static const bool esp_spp_enable_l2cap_ertm = true;
static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

// --- Definições do RS485 ---
#define UART_TESTE_NUM    (UART_NUM_2) 
#define TXD_PIN           (GPIO_NUM_17) 
#define RXD_PIN           (GPIO_NUM_16) 
#define DERE_PIN          (GPIO_NUM_4)  // Pino de Controle Direção (DE/RE) 
#define BUF_SIZE          (1024) 
#define TASK_STACK_SIZE   (2048) 
#define MAX_REPS          (32) // Buffer de mensagem tem 32 bytes 

static const char *RS_TAG = "RS485_TESTER";

// --- Variáveis Globais de Configuração ---
typedef struct {
    int baud_rate;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    int frame_delay_ms;
    uint8_t base_byte; // O byte selecionado (ex: 0x55) 
    int     repetitions; // Quantas vezes repetir 
    uint8_t message[MAX_REPS]; // Buffer da mensagem final 
    int     message_len; // Tamanho da mensagem final 
} rs485_config_t;

// Configuração inicializada com padrões 
static volatile rs485_config_t g_config = {
    .baud_rate = 115200,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .frame_delay_ms = 100,
    .base_byte = 0x55,
    .repetitions = 5,
    .message = {0x55, 0x55, 0x55, 0x55, 0x55},
    .message_len = 5
};

static TaskHandle_t g_generator_task_handle = NULL; 

// ==============================================================================
// FUNÇÕES DO RS485
// ==============================================================================

// Tarefa Geradora (Envia o sinal RS485 continuamente)
static void rs485_test_task(void *arg) {
    ESP_LOGI(RS_TAG, "Tarefa Geradora iniciada.");
    while (1) { 
        gpio_set_level(DERE_PIN, 1);
        ets_delay_us(50);
        // Escreve a mensagem final 
        uart_write_bytes(UART_TESTE_NUM, (const char*)g_config.message, g_config.message_len); 
        uart_wait_tx_done(UART_TESTE_NUM, pdMS_TO_TICKS(100)); 
        gpio_set_level(DERE_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(g_config.frame_delay_ms));
    }
}

// Reconfigura a UART dinamicamente
void reconfigure_test_uart() {
    ESP_LOGI(RS_TAG, "Reconfigurando UART2 para %d baud", g_config.baud_rate);
    if (g_generator_task_handle != NULL) vTaskSuspend(g_generator_task_handle); 
    
    uart_driver_delete(UART_TESTE_NUM);
    uart_config_t uart_config = {
        .baud_rate = g_config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = g_config.parity,
        .stop_bits = g_config.stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_TESTE_NUM, BUF_SIZE * 2, 0, 0, NULL, 0); 
    uart_param_config(UART_TESTE_NUM, &uart_config);
    uart_set_pin(UART_TESTE_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (g_generator_task_handle != NULL) vTaskResume(g_generator_task_handle); 
}

// Converte string binária ("10101010") para uint8_t
uint8_t parse_binary_string(char* s) {
    uint8_t byte_val = 0;
    for (int i = 0; i < 8; i++) { 
        if (s[i] == '1') {
            byte_val |= (1 << (7 - i)); 
        }
    }
    ESP_LOGI(RS_TAG, "Binario %s -> 0x%02X", s, byte_val);
    return byte_val; 
}

static void init_gpio(void) {
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT; 
    io_conf.pin_bit_mask = (1ULL << DERE_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(DERE_PIN, 0); 
}

// ==============================================================================
// FUNÇÕES DO BLUETOOTH
// ==============================================================================

static char *bda2str(uint8_t * bda, char *str, size_t size) {
    if (bda == NULL || str == NULL || size < 18) return NULL;
    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            esp_spp_start_srv(sec_mask, role_slave, 0, SPP_SERVER_NAME);
        }
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            esp_bt_gap_set_device_name(local_device_name);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    
    // --- O CORAÇÃO DO PARSER: ONDE OS DADOS CHEGAM DO CELULAR ---
    case ESP_SPP_DATA_IND_EVT:
        ESP_LOGI(SPP_TAG, "Recebido %d bytes", param->data_ind.len);
        
        // Copia os dados para uma string segura com terminador nulo
        char cmd[256];
        int len = param->data_ind.len;
        if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
        memcpy(cmd, param->data_ind.data, len);
        cmd[len] = '\0';
        
        bool needs_reconfig = false;
        char *token = strtok(cmd, ":\n"); 

        while (token != NULL) { 
            char* key = token;
            token = strtok(NULL, ":\n"); 
            if (token == NULL) break;
            char* value = token;

            ESP_LOGI(SPP_TAG, "Comando BT: %s=%s", key, value);

            if (strcmp(key, "BAUD") == 0) { 
                g_config.baud_rate = atoi(value);
                needs_reconfig = true; 
            } 
            else if (strcmp(key, "DELAY") == 0) {
                g_config.frame_delay_ms = atoi(value);
            } 
            else if (strcmp(key, "STOP") == 0) { 
                if (strcmp(value, "2 Bits") == 0) g_config.stop_bits = UART_STOP_BITS_2;
                else g_config.stop_bits = UART_STOP_BITS_1; 
                needs_reconfig = true;
            } 
            else if (strcmp(key, "PARITY") == 0) {
                if (strcmp(value, "Even (Par)") == 0) g_config.parity = UART_PARITY_EVEN;
                else if (strcmp(value, "Odd (Impar)") == 0) g_config.parity = UART_PARITY_ODD; 
                else g_config.parity = UART_PARITY_DISABLE;
                needs_reconfig = true;
            }
            else if (strcmp(key, "REPS") == 0) { 
                g_config.repetitions = atoi(value);
                if (g_config.repetitions < 1) g_config.repetitions = 1; 
                if (g_config.repetitions > MAX_REPS) g_config.repetitions = MAX_REPS; 
                
                g_config.message_len = g_config.repetitions;
                for(int i=0; i < g_config.repetitions; i++) { 
                    g_config.message[i] = g_config.base_byte; 
                }
            }
            else if (strcmp(key, "MSG_BIN") == 0) {
                if (strlen(value) == 8) { 
                    g_config.base_byte = parse_binary_string(value);
                    g_config.message_len = g_config.repetitions; 
                    for(int i=0; i < g_config.repetitions; i++) { 
                        g_config.message[i] = g_config.base_byte; 
                    }
                }
            }
            token = strtok(NULL, ":\n");
        }
        
        if (needs_reconfig) {
            reconfigure_test_uart();
        }
        break;
        
    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGI(SPP_TAG, "Cliente Conectado! bda:[%s]", bda2str(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));
        break;
    default:
        break;
    }
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(SPP_TAG, "authentication success: %s", param->auth_cmpl.device_name);
        }
        break;
    default:
        break;
    }
}

// ==============================================================================
// MAIN
// ==============================================================================
void app_main(void) {
    // 1. Inicializa Memória NVS (Obrigatório para BT)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Inicializa o Stack Bluetooth Classic SPP
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(esp_bt_gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(esp_spp_cb));
    
    esp_spp_cfg_t bt_spp_cfg = {
        .mode = esp_spp_mode,
        .enable_l2cap_ertm = esp_spp_enable_l2cap_ertm,
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&bt_spp_cfg));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    // 3. Inicializa Hardware RS485
    init_gpio();
    for(int i=0; i < g_config.repetitions; i++) { 
        g_config.message[i] = g_config.base_byte; 
    }
    reconfigure_test_uart(); 
    
    // 4. Inicia a Tarefa Geradora
    xTaskCreate(rs485_test_task, "rs485_test_task", TASK_STACK_SIZE, NULL, 10, &g_generator_task_handle); 
    ESP_LOGI(RS_TAG, "Sistema pronto! Aguardando conexao Bluetooth...");
}