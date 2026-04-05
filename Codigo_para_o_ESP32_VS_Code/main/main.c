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


// ==============================================================================
// --- Inicialização da Configuração Padrão e Controles do Sistema (FreeRTOS) ---
// ==============================================================================

// Cria a variável global 'g_config' já carregada com valores "de fábrica".
// Isso garante que se o ESP32 ligar e já tentar injetar dados (antes do celular conectar), ele tenha parâmetros seguros para usar.
static rs485_config_t g_config = {
    .baud_rate = 115200,                // Inicia com a velocidade de 115200 bps
    .parity = UART_PARITY_DISABLE,      // Inicia sem bit de paridade (formando o clássico padrão 8N1 junto com o stop bit)
    .stop_bits = UART_STOP_BITS_1,      // 1 Stop bit de encerramento de frame
    .frame_delay_ms = 100,              // Intervalo seguro de 100 milissegundos entre as injeções
    .base_byte = 0x55,                  // 0x55 (binário 01010101): Padrão ideal em onda quadrada para verificação de sinal no osciloscópio
    .repetitions = 5,                   // Vai repetir esse byte de teste 5 vezes
    .message = {0x55, 0x55, 0x55, 0x55, 0x55}, // O buffer pré-carregado com a rajada de teste pronta
    .message_len = 5                    // O tamanho inicial correspondente da mensagem acima
};

// "RG" da Tarefa do Gerador (Handle). O FreeRTOS usa essa variável para saber exatamente qual rotina ele deve pausar, retomar ou deletar da memória. Inicia vazia (NULL).
static TaskHandle_t g_generator_task_handle = NULL;

// A "Chave do Cofre" (Mutex). Uma trava de segurança vital! Ela garante que o Bluetooth não altere a configuração 'g_config' exatamente no mesmo microssegundo que o RS485 estiver lendo ela para transmitir no cabo.
static SemaphoreHandle_t g_config_mutex = NULL;

//****************************************************************************//
// ------------------------FUNÇÕES DO RS485-----------------------------------//
//****************************************************************************//

// Atualiza a memória (buffer) com os novos dados recebidos do celular antes da transmissão.
// O termo 'static' significa que esta função é privada e só existe dentro deste arquivo C.
static void update_message_buffer() {
    
    // --- Travas de Segurança Lógica ---
    // Garante que o sistema nunca tente enviar "zero" ou valores negativos de pacotes, forçando o mínimo de 1.
    if (g_config.repetitions < 1) g_config.repetitions = 1;
    
    // Proteção contra Estouro de Memória (Buffer Overflow).
    // Se o aplicativo Android mandar um comando pedindo 100 repetições, o ESP32 barra e trava 
    // no limite máximo de segurança do hardware (MAX_REPS, que você definiu como 32).
    if (g_config.repetitions > MAX_REPS) g_config.repetitions = MAX_REPS;

    // Atualiza a variável indicadora com o tamanho real que a mensagem terá.
    g_config.message_len = g_config.repetitions;
    
    // Loop de preenchimento (Montagem da rajada):
    // Roda preenchendo as "gavetas" do array (da posição 0 até o limite de repetições),
    // copiando para dentro de cada uma o mesmo 'base_byte' formado pelas chaves lá do seu aplicativo.
    for (int i = 0; i < g_config.repetitions; i++) {
        g_config.message[i] = g_config.base_byte;
    }
}

//------ Tarefa Geradora (Envia o sinal RS485 continuamente)------//
// Assinatura padrão de uma "Task" (Tarefa) no FreeRTOS. 
// Ela roda de forma paralela e independente do Bluetooth, como se fosse um segundo programa dentro do chip.
static void rs485_test_task(void *arg) {
    
    // Imprime no terminal do VS Code (em texto verde, nível INFO) que o motor do RS485 "deu a partida" com sucesso.
    ESP_LOGI(RS_TAG, "Tarefa Geradora iniciada.");
    
    // --- Criação de Variáveis Locais (Técnica de Cópia Sombra) ---
    // Em vez de usar a configuração global e deixar a trava de segurança (Mutex) bloqueada
    // durante todo o tempo de envio (que é lento), a tarefa cria "gavetas" locais.
    // O sistema vai trancar o Mutex apenas por 1 microssegundo para copiar os dados para cá e já destrancar em seguida.
    // Assim, o seu celular nunca "trava" ao tentar enviar um novo comando Bluetooth!
    
    uint8_t local_message[MAX_REPS]; // Cópia local da rajada de bytes que será enviada
    int local_message_len;           // Cópia local do tamanho da mensagem
    int local_delay_ms;              // Cópia local do tempo de pausa entre os disparos


// O FreeRTOS exige que as tarefas rodem em um loop que nunca termina.
    while (1) {
        
        // --- 1. Leitura Segura dos Dados (Operação Relâmpago) ---
        // Pega a "Chave do Cofre" (Mutex). Se o Bluetooth estiver escrevendo novos dados bem neste milissegundo, 
        // a tarefa espera pacientemente (portMAX_DELAY) até o cofre ser liberado.
        xSemaphoreTake(g_config_mutex, portMAX_DELAY);
        
        // Faz a "Cópia Sombra" (copia a mensagem, o tamanho e o delay global para as variáveis locais)
        memcpy(local_message, g_config.message, g_config.message_len);
        local_message_len = g_config.message_len;
        local_delay_ms = g_config.frame_delay_ms;
        
        // Devolve a chave do cofre imediatamente! Assim que chega aqui, o Bluetooth já está livre 
        // para receber novos comandos do celular sem engasgar o sistema.
        xSemaphoreGive(g_config_mutex);

        // --- 2. Preparação Física do Hardware (Transmissão) ---
        // Nível ALTO (1) no pino DE/RE: Avisa o chip MAX485 para virar as chaves internas e ligar o amplificador de transmissão no cabo.
        gpio_set_level(DERE_PIN, 1);
        
        // Atraso de ROM (microssegundos): Dá um tempo microscópico para o silício do MAX485 estabilizar a tensão
        // na rede A/B antes de cuspir os bits. Previne "ruído" fantasma no início do pacote Modbus.
        ets_delay_us(RS485_TX_DELAY_US);
        
        // --- 3. Injeção de Dados ---
        // Despeja todo o buffer (local_message) de uma vez na fila de saída da porta Serial (UART).
        uart_write_bytes(UART_TESTE_NUM, (const char*)local_message, local_message_len);
        
        // Trava do Hardware: O processador para aqui e fica vigiando a porta Serial até o último bit 
        // sair fisicamente pelo fio de cobre. O '100' é um timeout de segurança de 100ms caso algo trave.
        uart_wait_tx_done(UART_TESTE_NUM, pdMS_TO_TICKS(100)); 
        
        // --- 4. Desligamento e Retorno ao Modo Escuta ---
        // Imediatamente após o último bit sair, joga o pino DE/RE para BAIXO (0).
        // Isso desliga a transmissão e volta o módulo para o modo escuta (evitando colisão de dados no barramento).
        gpio_set_level(DERE_PIN, 0);
        
        // --- 5. Intervalo / Ritmo de Injeção ---
        // Pausa a tarefa (motor) pelo tempo de "Delay (ms)" que você configurou na tela do aplicativo.
        // Diferente de um delay comum, o 'vTaskDelay' avisa o FreeRTOS: "Vou dormir, pode usar o processador para o Bluetooth ou outra coisa!".
        vTaskDelay(pdMS_TO_TICKS(local_delay_ms));
    }
} // Fim da função rs485_test_task

//--- Reconfigura a UART dinamicamente (Troca de Parâmetros "A Quente")----//
// Toda vez que o aplicativo Android manda um novo Baud Rate, Paridade ou Stop Bit, 
// o sistema chama essa função para aplicar as mudanças fisicamente no chip ESP32.
void reconfigure_test_uart() {    
    // --- 1. Parada de Segurança ---
    // Verifica se a tarefa do gerador já existe. Se sim, suspende (pausa) ela imediatamente.
    // Isso impede que o ESP32 tente enviar dados pelo RS485 bem na hora em que estamos "desmontando" a porta.
    if (g_generator_task_handle != NULL) vTaskSuspend(g_generator_task_handle); 
    
    // --- 2. Desmontagem ---
    // Remove o driver atual da porta Serial da memória, liberando o hardware para receber a nova configuração.
    uart_driver_delete(UART_TESTE_NUM);
    
    // --- 3. Preparação dos Novos Parâmetros ---//
    // Cria a estrutura do ESP-IDF com os novos dados que vieram do aplicativo (via g_config)
    uart_config_t uart_config = {
        .baud_rate = g_config.baud_rate,           // Nova velocidade (ex: 9600 para 115200)
        .data_bits = UART_DATA_8_BITS,             // Padrão Modbus RTU: Sempre 8 bits de dados
        .parity    = g_config.parity,              // Nova paridade (Par, Ímpar, Nenhuma)
        .stop_bits = g_config.stop_bits,           // Novo limite de fim de pacote (1 ou 2 bits)
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,     // Desativa controle de fluxo via hardware (não usado no RS485 padrão)
        .source_clk = UART_SCLK_DEFAULT,           // Usa o relógio (clock) padrão do microcontrolador
    };

    // Log de diagnóstico no terminal para o desenvolvedor confirmar se o ESP32 entendeu o comando do celular
    ESP_LOGI(RS_TAG, "Reconfigurando UART2 para %d baud, Parity: %d, Stop bits: %d", uart_config.baud_rate, uart_config.parity, uart_config.stop_bits);

    // --- 4. Remontagem do Hardware ---
    // Instala o driver da porta Serial novamente, agora recriando as memórias de recepção e transmissão (Buffer)
    uart_driver_install(UART_TESTE_NUM, BUF_SIZE * 2, 0, 0, NULL, 0); 
    
    // Aplica as novas regras físicas de velocidade e paridade que definimos acima
    uart_param_config(UART_TESTE_NUM, &uart_config);
    
    // Refaz a solda "virtual" dos pinos lógicos (TX no pino 17, RX no pino 16) à nova porta Serial recriada
    uart_set_pin(UART_TESTE_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // --- 5. Retomada da Operação ---
    // Agora que o hardware está 100% configurado com os novos parâmetros, "acorda" a tarefa do gerador
    // para ela voltar a injetar os pulsos na rede RS485.
    if (g_generator_task_handle != NULL) vTaskResume(g_generator_task_handle); 
}

//---------- Converte string binária ("10101010") para uint8_t------//
// Pega o texto gerado pelos 8 switches da tela do aplicativo (ex: "10101010") 
// e o "comprime" em um único byte numérico real de 8 bits (uint8_t) pronto para a rede.
uint8_t parse_binary_string(char* s) {
    
    // Inicia a variável com valor zero (em binário: 00000000)
    uint8_t byte_val = 0; 
    
    // Loop que roda exatamente 8 vezes (para ler os 8 caracteres da string recebida)
    for (int i = 0; i < 8; i++) { 
        
        // Verifica se o caractere na posição atual do texto é o número '1' (Switch ligado)
        if (s[i] == '1') {
            
            // --- Operação Binária (Bitwise OR e Shift) ---
            // Como lemos o texto da esquerda para a direita (índice 0, 1, 2...), mas os bits são contados
            // da direita para a esquerda, usamos (7 - i) para inverter. 
            // Assim, o primeiro '1' da string liga o Bit 7 (Mais Significativo/MSB), e o último liga o Bit 0 (LSB).
            byte_val |= (1 << (7 - i)); 
        }
    }
    
    // Imprime no terminal o resultado da mágica (ex: "Binario 10101010 -> 0xAA") para facilitar o debug
    ESP_LOGI(RS_TAG, "Binario %s -> 0x%02X", s, byte_val);
    
    // Devolve o byte puramente numérico e pronto para ser copiado para o buffer do RS485
    return byte_val; 
}

// Prepara o pino do ESP32 que vai acionar o gatilho (DE/RE) do chip MAX485,
// definindo fisicamente se ele vai enviar energia para a rede ou ficar apenas escutando.
static void init_gpio(void) {
    
    // Cria a "caixa" (estrutura do ESP-IDF) para guardar os parâmetros do pino
    gpio_config_t io_conf;
    
    // Desativa os alarmes (interrupções). Como este pino é só uma saída de controle, 
    // não precisamos que ele interrompa o processador para avisar nada.
    io_conf.intr_type = GPIO_INTR_DISABLE;
    
    // Define o pino explicitamente como SAÍDA (Output). O ESP32 vai ativamente mandar tensão (3.3V ou 0V) para o módulo MAX485.
    io_conf.mode = GPIO_MODE_OUTPUT; 
    
    // Seleciona exatamente qual pino físico será configurado aplicando uma máscara de bits
    // (apontando para o DERE_PIN que você definiu lá em cima, o pino 4).
    io_conf.pin_bit_mask = (1ULL << DERE_PIN);
    
    // Desliga resistores de aterramento internos (Pull-down)...
    io_conf.pull_down_en = 0;
    
    // ...e desliga resistores de tensão internos (Pull-up). Não precisamos deles aqui 
    // porque o próprio módulo MAX485 externo já cuida da proteção eletrônica.
    io_conf.pull_up_en = 0;
    
    // Pega toda essa configuração que preparamos na memória e grava definitivamente no hardware do ESP32
    gpio_config(&io_conf);
    
    // --- Estado de Partida Seguro (Fail-Safe) ---
    // Assim que o pino é ativado, força imediatamente a tensão para BAIXO (0).
    // Isso liga o RE (Receiver Enable) do MAX485, colocando o módulo em modo de ESCUTA.
    // É uma regra de ouro em redes industriais para não causar curto-circuito lógico logo ao ligar a placa!
    gpio_set_level(DERE_PIN, 0); 
}



//****************************************************************************//
//------------------------FUNÇÕES DO BLUETOOTH--------------------------------//
//****************************************************************************//

// Converte o Endereço Físico (MAC Address) do celular ou do ESP32 de um array de bytes para um texto legível (String).
// Um endereço MAC usa 17 caracteres visíveis + 1 caractere invisível de "fim de texto" ('\0'), exigindo 18 posições de memória.
static char *bda2str(uint8_t * bda, char *str, size_t size) {
    
    // --- Trava de Segurança (Sanity Check) ---
    // Verifica se os dados de origem (bda) e destino (str) não estão vazios (NULL)
    // e se temos pelo menos 18 "gavetas" de tamanho para não causar vazamento de memória. 
    // Se a rede mandar lixo, aborta imediatamente (return NULL).
    if (bda == NULL || str == NULL || size < 18) return NULL;
    
    // Cria um ponteiro auxiliar ('p') para ler os bytes de origem recebidos do rádio Bluetooth
    uint8_t *p = bda;
    
    // --- Montagem do Texto ---
    // Pega os 6 bytes do array (p[0] a p[5]) e converte para número Hexadecimal com dois dígitos (%02x).
    // Em seguida, junta tudo separando com dois pontos ( : ) e salva dentro da variável de texto 'str'.
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
    
    // Devolve o texto finalizado, pronto para ser usado nos relatórios de conexão no terminal
    return str;
}




// ------------- (Callback SPP) ------------------//
// Esta função é o "ouvido" do ESP32. Ela é disparada automaticamente pelo sistema 
// sempre que o rádio Bluetooth liga, conecta ou recebe um pacote pelo ar.
static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    char bda_str[18] = {0}; // Buffer para guardar o endereço MAC legível, se precisar

    switch (event) {
    
    // --- 1. Evento de Inicialização ---
    case ESP_SPP_INIT_EVT:
        // O chip confirmou que a placa de rádio ligou com sucesso. 
        // Agora, mandamos iniciar o "Servidor SPP" (a porta serial invisível).
        if (param->init.status == ESP_SPP_SUCCESS) {
            esp_spp_start_srv(sec_mask, role_slave, 0, SPP_SERVER_NAME);
        }
        break;
        
    // --- 2. Evento de Servidor Pronto ---
    case ESP_SPP_START_EVT:
        // O servidor SPP subiu. Agora configuramos a "vitrine": 
        // Definimos o nome do aparelho e ativamos o modo "Descobrível" para ele aparecer nas buscas do celular.
        if (param->start.status == ESP_SPP_SUCCESS) {
            esp_bt_gap_set_device_name(local_device_name);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
        
    // --- 3. EVENTO PRINCIPAL: RECEPÇÃO DE DADOS DO CELULAR ---//
    case ESP_SPP_DATA_IND_EVT:
        ESP_LOGI(SPP_TAG, "Recebido %d bytes", param->data_ind.len);
        
        // --- Cópia Segura para a Memória (Anti-Crash) ---
        // Pega o pacote cru do Bluetooth e transforma em um texto C válido (com '\0' no final).
        char cmd[256];
        int len = param->data_ind.len;
        if (len >= sizeof(cmd)) len = sizeof(cmd) - 1; // Corta o pacote se for maior que nossa "gaveta"
        memcpy(cmd, param->data_ind.data, len);
        cmd[len] = '\0';
        
        // Flag que avisa se precisaremos mexer nas engrenagens de hardware (UART) no final
        bool needs_reconfig = false;
        char *saveptr; // Ponteiro auxiliar exigido pela função strtok_r

        // --- Início da Seção Crítica ---
        // Pega a chave do cofre (Mutex). O RS485 é proibido de tentar ler as variáveis globais 
        // enquanto estamos atualizando elas com os dados novos do celular.
        xSemaphoreTake(g_config_mutex, portMAX_DELAY);

        // --- O "Fatiador" de Texto (Parser) ---
        // Corta a string recebida (ex: "BAUD:9600\nDELAY:100") usando os dois pontos ':' e a quebra de linha '\n' como tesouras.
        char *token = strtok_r(cmd, ":\n", &saveptr); 

        // Roda o loop até fatiar todas as linhas da mensagem
        while (token != NULL) { 
            char* key = token; // A palavra da esquerda (ex: "BAUD")
            
            token = strtok_r(NULL, ":\n", &saveptr); // Corta novamente para pegar a parte da direita
            if (token == NULL) break; // Sai se a string chegou quebrada (sem o valor)
            char* value = token; // A palavra da direita (ex: "9600")

            ESP_LOGI(SPP_TAG, "Comando BT: %s=%s", key, value); // Log visual no VS Code

            // --- Roteador de Comandos ---
            if (strcmp(key, "BAUD") == 0) { 
                g_config.baud_rate = atoi(value); // Converte texto para número (int)
                needs_reconfig = true;            // Alterar o Baud exige mexer no hardware!
            } 
            else if (strcmp(key, "DELAY") == 0) {
                g_config.frame_delay_ms = atoi(value); // Delay é só software, não exige reconfigurar UART
            } 
            else if (strcmp(key, "STOP") == 0) { 
                if (strcmp(value, "2 Bits") == 0) g_config.stop_bits = UART_STOP_BITS_2;
                else g_config.stop_bits = UART_STOP_BITS_1; 
                needs_reconfig = true; // Exige hardware
            } 
            else if (strcmp(key, "PARITY") == 0) {
                if (strcmp(value, "Even (Par)") == 0) g_config.parity = UART_PARITY_EVEN;
                else if (strcmp(value, "Odd (Impar)") == 0) g_config.parity = UART_PARITY_ODD; 
                else g_config.parity = UART_PARITY_DISABLE;
                needs_reconfig = true; // Exige hardware
            }
            else if (strcmp(key, "REPS") == 0) { 
                g_config.repetitions = atoi(value);
                update_message_buffer(); // Refaz o pacote do Modbus imediatamente
            }
            else if (strcmp(key, "MSG_BIN") == 0) {
                if (strlen(value) == 8) { 
                    g_config.base_byte = parse_binary_string(value); // Chama aquela função de conversão 1010->Byte
                    update_message_buffer(); // Refaz o pacote
                }
            }
            
            // Pula para a próxima linha da string
            token = strtok_r(NULL, ":\n", &saveptr);
        }

        // --- Fim da Seção Crítica ---
        // Tudo atualizado. Devolve a chave do cofre para o RS485 poder voltar a transmitir.
        xSemaphoreGive(g_config_mutex);
        
        // --- Aplicação no Hardware ---
        // Excelente prática: Deixamos a reinicialização da UART (que é uma operação lenta) 
        // para FORA da trava de segurança (Mutex), evitando congelar o sistema.
        if (needs_reconfig) {
            reconfigure_test_uart();
        }
        break;
        
    // --- 4. Evento de Conexão Bem Sucedida ---
    case ESP_SPP_SRV_OPEN_EVT:
        // O aplicativo Android se conectou! Imprime o endereço MAC do celular no terminal.
        ESP_LOGI(SPP_TAG, "Cliente Conectado! bda:[%s]", bda2str(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));
        break;
        
    default:
        break;
    }
}


// --- Segurança e Pareamento (Callback GAP) ---

// Esta função gerencia os eventos "por baixo dos panos" do rádio Bluetooth, 
// lidando especificamente com a camada de acesso, visibilidade e chaves de segurança.
void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    
    switch (event) {    
    // --- Evento de Conclusão de Autenticação ---
    // É disparado no exato momento em que o celular e o ESP32 terminam de negociar
    // e trocar as chaves de segurança (logo após você apertar "Parear" na tela do Android).
    case ESP_BT_GAP_AUTH_CMPL_EVT:        
        // Verifica se o processo de segurança foi aprovado e concluído com sucesso
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {            
            // Imprime no terminal de debug o nome do aparelho que acabou de ganhar
            // permissão definitiva para acessar o gerador (ex: "Galaxy S23", "Pixel 9")
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
    
    // --- 1. Inicializa Memória NVS (O "HD" do ESP32) ---
    // O Bluetooth é obrigado a ter um lugar para salvar as chaves de segurança dos celulares conhecidos.
    esp_err_t ret = nvs_flash_init();
    
    // Se a memória estiver corrompida ou com uma versão muito antiga...
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); // ...formata a memória (apaga tudo)...
        ret = nvs_flash_init();             // ...e tenta inicializar de novo.
    }
    ESP_ERROR_CHECK(ret); // Trava o sistema se der erro crítico no hardware.

    // --- 2. Inicializa o Stack Bluetooth Classic (SPP) ---
    // Otimização de RAM: Como seu projeto usa o Bluetooth Clássico, avisamos o chip 
    // para apagar o módulo "BLE" (Bluetooth Low Energy) da memória RAM, economizando espaço.
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    
    // Prepara e liga o hardware de rádio (A antena física)
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    
    // Prepara e liga o software de controle (O "cérebro" lógico Bluedroid)
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    
    // "Instala os ouvidos": Conecta as funções que criamos lá em cima aos eventos do rádio
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(esp_bt_gap_cb)); // Segurança e Visibilidade
    ESP_ERROR_CHECK(esp_spp_register_callback(esp_spp_cb));       // Troca de Dados (Android <-> ESP32)
    
    // Inicia a porta Serial Invisível (SPP) com retransmissão de pacotes ativada
    esp_spp_cfg_t bt_spp_cfg = {
        .mode = esp_spp_mode,
        .enable_l2cap_ertm = esp_spp_enable_l2cap_ertm,
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&bt_spp_cfg));

    // Configuração de Pareamento Simplificado (Just Works)
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    memset(pin_code, 0, sizeof(esp_bt_pin_code_t)); // Zera a variável de senha para limpar lixo de memória
    esp_bt_gap_set_pin(pin_type, 0, pin_code);      // Avisa que não existe um PIN de 4 números fixo digitável

    // --- 3. Inicializa Hardware RS485 ---
    
    // Cria a "Chave do Cofre" (Mutex). Precisa ser feito antes de qualquer coisa tentar ler/escrever configurações.
    g_config_mutex = xSemaphoreCreateMutex();
    
    // Configura o pino de Direção (DE/RE) e já deixa ele no nível 0 (Modo de Escuta Segura)
    init_gpio();

    // Monta a primeira rajada de bits (0x55 padrão) antes de ligar a transmissão
    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    update_message_buffer(); 
    xSemaphoreGive(g_config_mutex);

    // Liga fisicamente a porta Serial do ESP32 com a velocidade padrão (115200 bps)
    reconfigure_test_uart(); 
    
    // --- 4. Inicia o Sistema Operacional (FreeRTOS) ---
    // Cria o "motor" do gerador. Ele vai rodar a função 'rs485_test_task' de forma totalmente independente 
    // do resto do código, com 2KB de memória RAM exclusiva e Prioridade nível 10.
    xTaskCreate(rs485_test_task, "rs485_test_task", TASK_STACK_SIZE, NULL, 10, &g_generator_task_handle); 
    
    // Imprime no terminal a mensagem de sucesso.
    ESP_LOGI(RS_TAG, "Sistema pronto! Aguardando conexao Bluetooth...");
}