/*
 * Analisador RS485 - Modbus RTU (Módulo Injetor Escravo)
 * Comunicação de Parametrização via Bluetooth SPP
 */


//----------------------------------------------------------------------------//
// --------------------- Inclusão das Bibliotecas -----------------------------//
//----------------------------------------------------------------------------//

// --- 1. Bibliotecas Padrão do C (Matemática, Memória e Textos) ---
#include <stdint.h>   // Variáveis de tamanho exato (ex: uint8_t para o pacote Modbus)
#include <string.h>   // Manipulação de strings e blocos de memória (memcpy)
#include <stdbool.h>  // Suporte a valores booleanos (true/false)
#include <stdio.h>    // Entrada e saída padrão (printf, sprintf)
#include <inttypes.h> // Auxilia a imprimir variáveis exatas de forma segura no printf
#include <stdlib.h>   // Utilitários gerais (alocação de memória e conversão texto->número)

// --- 2. Memória Não Volátil (Obrigatória para o Bluetooth) ---
#include "nvs.h"       // Acesso à memória interna (EEPROM/Flash) do ESP32
#include "nvs_flash.h" // Salva chaves de pareamento para não pedir permissão toda vez

// --- 3. Sistema Operacional (FreeRTOS) e Sincronização ---
#include "freertos/FreeRTOS.h" // Biblioteca base do sistema operacional (multitarefas)
#include "freertos/task.h"     // Permite rodar rotinas simultâneas (ex: Bluetooth e RS485 juntos)
#include "freertos/semphr.h"   // Semáforos e Mutex: Impede que duas tarefas alterem a mesma variável ao mesmo tempo (Thread-safety)

// --- 4. Logs e Mensagens do Sistema ---
#include "esp_log.h" // Sistema profissional de mensagens de debug no terminal (ESP_LOGI)

// --- 5. Pilha de Comunicação Bluetooth ---
#include "esp_bt.h"         // Liga a antena e inicializa o controlador físico de rádio
#include "esp_bt_main.h"    // Inicializa o "cérebro" lógico do Bluetooth (Bluedroid)
#include "esp_gap_bt_api.h" // (GAP) Controla a vitrine: Nome do aparelho e visibilidade
#include "esp_bt_device.h"  // Pega informações do hardware (ex: ler o Endereço MAC da placa)
#include "esp_spp_api.h"    // (SPP) A mais importante: Simula o cabo serial recebendo os dados do Android

// --- 6. Controle de Tempo ---
#include "time.h"     // Funções padrão de tempo
#include "sys/time.h" // Medição de tempo para gerenciar milissegundos

// --- 7. Bibliotecas Adicionais Específicas para o RS485 (Hardware) ---
#include "driver/uart.h" // Driver da porta Serial: Configura Baud rate, Paridade e Stop bits do barramento
#include "driver/gpio.h" // Driver dos pinos: Usado para controlar o pino de direção (RE/DE) do módulo MAX485
#include "rom/ets_sys.h" // Funções de baixo nível da ROM: Fornece o 'ets_delay_us' para pausas exatas de microssegundos (vital para o timing do Modbus)


// ==============================================================================
// --- Configurações e Regras do Bluetooth SPP (Serial Port Profile) ---
// ==============================================================================

// Tag usada pela função ESP_LOGI para identificar que a mensagem no terminal veio do Bluetooth
#define SPP_TAG "SPP_ACCEPTOR_DEMO" 
// Nome do serviço interno do Bluetooth (O Android procura por esse nome para abrir a porta serial)
#define SPP_SERVER_NAME "SPP_SERVER" 
// Nome público do aparelho (O nome que vai aparecer na tela do celular, ex: "ESP_SPP_ACCEPTOR")
static const char local_device_name[] = CONFIG_EXAMPLE_LOCAL_DEVICE_NAME; 
// Define que o Bluetooth vai rodar no modo "Callback" (reage a eventos como 'Conectou' ou 'Recebeu Dado')
static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB; 
// Ativa a retransmissão de pacotes perdidos (Garante que nenhum byte da sua string se perca pelo ar)
static const bool esp_spp_enable_l2cap_ertm = true; 
// Máscara de segurança: Exige que o celular faça a autenticação (pareamento) para poder conectar
static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE; 
// Define o papel do ESP32 na rede como "Escravo" (Ele fica parado ouvindo, esperando o celular "Mestre" se conectar a ele)
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;


// ==============================================================================
// --- Definições e Pinos do Barramento RS485 ---
// ==============================================================================

// Escolhe a porta Serial 2 do ESP32 (Evita conflitar com a Serial 0, que é usada pelo cabo USB para gravar/debug)
#define UART_TESTE_NUM    (UART_NUM_2)
// Pino TX (Transmissão): Onde o ESP32 envia os bits que vão entrar no pino DI (Driver Input) do módulo MAX485
#define TXD_PIN           (GPIO_NUM_17)
// Pino RX (Recepção): Onde o ESP32 lê os bits que chegam do pino RO (Receiver Output) do módulo MAX485
#define RXD_PIN           (GPIO_NUM_16)
// Pino de Controle de Direção (DE/RE): O pino mais importante do RS485 (Half-Duplex). Nível ALTO = Envia dados / Nível BAIXO = Escuta dados
#define DERE_PIN          (GPIO_NUM_4)
// Tamanho da memória temporária (Ring Buffer) da porta Serial. 1024 bytes é espaço de sobra para armazenar pacotes Modbus grandes
#define BUF_SIZE          (1024)
// Memória RAM (Stack) reservada no sistema operacional (FreeRTOS) para a rotina que vai rodar as operações do RS485 (2048 bytes = 2KB)
#define TASK_STACK_SIZE   (2048)
// Limite máximo de bytes da mensagem (ou quantidade de repetições) para o buffer de injeção de dados
#define MAX_REPS          (32)
// Atraso de hardware (50 microssegundos). Garante que o último pulso elétrico terminou de viajar pelo longo cabo antes de virar a chave DE/RE para o modo escuta
#define RS485_TX_DELAY_US (50)
// Tag usada pela função ESP_LOGI para identificar que a mensagem de diagnóstico no terminal do VS Code veio do módulo RS485
static const char *RS_TAG = "RS485_TESTER";


// ==============================================================================
// --- Estrutura de Variáveis Globais (Configurações do Gerador RS485) ---
// ==============================================================================

// Cria um "pacote" (estrutura) que agrupa todos os parâmetros da injeção de dados. 
// Isso mantém o código organizado e facilita atualizar tudo junto quando o celular manda um comando.
typedef struct {
    int baud_rate;               // Velocidade da rede em bits por segundo (ex: 9600, 19200, 115200)
    uart_parity_t parity;        // Bit de verificação de erro do protocolo (None, Even/Par, Odd/Ímpar)
    uart_stop_bits_t stop_bits;  // Quantidade de bits (1 ou 2) que sinalizam o fim de cada caractere na serial
    int frame_delay_ms;          // Tempo de pausa/intervalo (em milissegundos) programado pelo app antes do envio
    uint8_t base_byte;           // O byte exato (de 0 a 255 / 0x00 a 0xFF) formado pelas 8 chaves lá do aplicativo
    int repetitions;             // Quantidade de vezes que o 'base_byte' será repetido e enviado na mesma rajada
    uint8_t message[MAX_REPS];   // Memória (Array) que guarda a rajada de dados já montada antes de jogar no cabo
    int message_len;             // O tamanho real (quantidade de bytes) que a mensagem final ocupou no buffer
} rs485_config_t;                // Nome deste novo "tipo" de variável personalizada


//----------------------------------------------------------------------------//
// ------------------Configuração inicializada com padrões--------------------//
//----------------------------------------------------------------------------//
static rs485_config_t g_config = {
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
static SemaphoreHandle_t g_config_mutex = NULL;


//****************************************************************************//
// ------------------------FUNÇÕES DO RS485-----------------------------------//
//****************************************************************************//

// Atualiza o buffer da mensagem com base na configuração atual
static void update_message_buffer() {
    if (g_config.repetitions < 1) g_config.repetitions = 1;
    if (g_config.repetitions > MAX_REPS) g_config.repetitions = MAX_REPS;

    g_config.message_len = g_config.repetitions;
    for (int i = 0; i < g_config.repetitions; i++) {
        g_config.message[i] = g_config.base_byte;
    }
}

// Tarefa Geradora (Envia o sinal RS485 continuamente)
static void rs485_test_task(void *arg) {
    ESP_LOGI(RS_TAG, "Tarefa Geradora iniciada.");
    
    // Variáveis locais para minimizar o tempo de bloqueio do mutex
    uint8_t local_message[MAX_REPS];
    int local_message_len;
    int local_delay_ms;

    while (1) {
        // Copia a configuração de forma segura para variáveis locais
        xSemaphoreTake(g_config_mutex, portMAX_DELAY);
        memcpy(local_message, g_config.message, g_config.message_len);
        local_message_len = g_config.message_len;
        local_delay_ms = g_config.frame_delay_ms;
        xSemaphoreGive(g_config_mutex);

        gpio_set_level(DERE_PIN, 1);
        ets_delay_us(RS485_TX_DELAY_US);
        uart_write_bytes(UART_TESTE_NUM, (const char*)local_message, local_message_len);
        uart_wait_tx_done(UART_TESTE_NUM, pdMS_TO_TICKS(100)); 
        gpio_set_level(DERE_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(local_delay_ms));
    }
}

// Reconfigura a UART dinamicamente
void reconfigure_test_uart() {
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

    // Log com a configuração que será aplicada
    ESP_LOGI(RS_TAG, "Reconfigurando UART2 para %d baud, Parity: %d, Stop bits: %d", uart_config.baud_rate, uart_config.parity, uart_config.stop_bits);

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



//****************************************************************************//
//------------------------FUNÇÕES DO BLUETOOTH--------------------------------//
//****************************************************************************//

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
    
    // --- ONDE OS DADOS CHEGAM DO CELULAR ---
    case ESP_SPP_DATA_IND_EVT:
        ESP_LOGI(SPP_TAG, "Recebido %d bytes", param->data_ind.len);
        
        // Copia os dados para uma string segura com terminador nulo
        char cmd[256];
        int len = param->data_ind.len;
        if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
        memcpy(cmd, param->data_ind.data, len);
        cmd[len] = '\0';
        
        bool needs_reconfig = false;
        char *saveptr; // Para strtok_r

        xSemaphoreTake(g_config_mutex, portMAX_DELAY);

        char *token = strtok_r(cmd, ":\n", &saveptr); 

        while (token != NULL) { 
            char* key = token;
            token = strtok_r(NULL, ":\n", &saveptr); 
            if (token == NULL) break; // Sai se não houver valor para a chave
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
                update_message_buffer();
            }
            else if (strcmp(key, "MSG_BIN") == 0) {
                if (strlen(value) == 8) { 
                    g_config.base_byte = parse_binary_string(value);
                    update_message_buffer();
                }
            }
            token = strtok_r(NULL, ":\n", &saveptr);
        }

        xSemaphoreGive(g_config_mutex);
        
        // A reconfiguração é chamada fora da seção crítica do mutex
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





//============================================================================//
//-------------------------------MAIN-----------------------------------------//
//============================================================================//

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
    memset(pin_code, 0, sizeof(esp_bt_pin_code_t)); // Zera o código PIN para evitar lixo de memória
    esp_bt_gap_set_pin(pin_type, 0, pin_code); // Define que não há um PIN fixo, permitindo pareamento "Just Works"

    // 3. Inicializa Hardware RS485
    g_config_mutex = xSemaphoreCreateMutex();
    init_gpio();

    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    update_message_buffer(); // Monta a mensagem inicial
    xSemaphoreGive(g_config_mutex);

    reconfigure_test_uart(); 
    
    // 4. Inicia a Tarefa Geradora
    xTaskCreate(rs485_test_task, "rs485_test_task", TASK_STACK_SIZE, NULL, 10, &g_generator_task_handle); 
    ESP_LOGI(RS_TAG, "Sistema pronto! Aguardando conexao Bluetooth...");
}