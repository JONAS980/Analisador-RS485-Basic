App de interface Do ESP 32 para realizar teste de rede Modbus RTU RS485 Comunicando Via Bluetooth com O ESP 32
Pasta "Codigo_para_o_ESP32_VS_Code" contem o codigo que deve ir no ESP 32 DevKit + Placa TTL 485
Foi feito uma comunicação via Bluetooth do APP do Celular com o ESP32, Que podemos configurar um Tipo de sinal padrão Do RS485, e realizar a laitura através de um Osciloscopio, para Verificar a degradação e interferencias  no Sinal.
Como o sinal é gerado através de uma placa TTL to 485 podemos injetar esse sinal na rede serial garantindo que não irá danificar possiveis equipamentos intalados na mesma rede, pois a placa TTL to 485 já atende os padrões eletricos da Norma RS485.
Outra vantagem de utilizar dessa forma os testes, pois se For simplesmente capturar dispositivos comunicando na rede, não temos a previsibilidade(numero de Bytes intervalos de transmição Etc.) do sinal que deve Chegar no Osciloscopio. Utilizando do Injetor Podemos ter Certeza de como o sinal está saindo e deveria vai chegar, e tambem podemos criar um padrão de repete indefinidamente facilitando o Scan e comparar a qualidade do sinal.
<img width="1080" height="2315" alt="image" src="https://github.com/user-attachments/assets/32dd6c93-3efc-4b0a-b0fe-234c163bad5a" />
<img width="1703" height="2272" alt="image" src="https://github.com/user-attachments/assets/d21dc9b4-36da-4019-ac86-069b5178ed26" />
<img width="1958" height="903" alt="image" src="https://github.com/user-attachments/assets/63961a70-a1c6-44ed-b154-c0ae5a35bf6b" />



