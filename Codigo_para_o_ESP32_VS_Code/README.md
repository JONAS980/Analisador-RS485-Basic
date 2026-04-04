| Supported Targets | ESP32 |
| ----------------- | ----- |

Utilizado como Base de inicio o Tamplate Demo
## ESP-IDF BT-SPP-ACCEPTOR demo

O Codigo para ESP32 realiza a função de Injetor de Sinal No padrão Serial RS485.
Foi feito uma comunicação via Bluetooth do APP do Celular com o ESP32, Que podemos configurar um Tipo de sinal padrão Do RS485, e realizar a laitura através de um Osciloscopio, para Verificar a degradação e interferencias  no Sinal.
Como o sinal é gerado através de uma placa TTL to 485 podemos injetar esse sinal na rede serial garantindo que não irá danificar possiveis equipamentos intalados na mesma rede, pois a placa TTL to 485 já atende os padrões eletricos da Norma RS485.
Outra vantagem de utilizar dessa forma os testes, pois se For simplesmente capturar dispositivos comunicando na rede, não temos a previsibilidade(numero de Bytes intervalos de transmição Etc.) do sinal que deve Chegar no Osciloscopio. Utilizando do Injetor Podemos ter Certeza de como o sinal está saindo e deveria vai chegar, e tambem podemos criar um padrão de repete indefinidamente facilitando o Scan e comparar a qualidade do sinal.

![alt text](image.png)
