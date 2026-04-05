package com.example.analisadorrs485basic

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MainScreen()
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen() {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    val scrollState = rememberScrollState()

    // --- VARIÁVEIS DE ESTADO DO BLUETOOTH ---
    var isConnected by remember { mutableStateOf(false) }
    var pairedDevicesList by remember { mutableStateOf(listOf<String>()) }
    var selectedDevice by remember { mutableStateOf("Selecione o ESP32...") }
    var expandedDeviceMenu by remember { mutableStateOf(false) }

    // --- LÓGICA DE PERMISSÕES ---
    val permissionsToRequest = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
    } else {
        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    val permissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestMultiplePermissions()
    ) { permissionsMap ->
        val allGranted = permissionsMap.values.reduce { acc, next -> acc && next }
        if (allGranted) {
            Toast.makeText(context, "Permissões concedidas! Clique em conectar novamente.", Toast.LENGTH_SHORT).show()
        } else {
            Toast.makeText(context, "Permissão negada. O app precisa do Bluetooth para funcionar.", Toast.LENGTH_LONG).show()
        }
    }

    fun carregarDispositivosPareados() {
        try {
            val bluetoothManager = context.getSystemService(android.content.Context.BLUETOOTH_SERVICE) as android.bluetooth.BluetoothManager
            val adapter = bluetoothManager.adapter
            val dispositivos = adapter?.bondedDevices

            if (dispositivos != null && dispositivos.isNotEmpty()) {
                pairedDevicesList = dispositivos.map { it.name ?: "Dispositivo Desconhecido" }
                if (selectedDevice == "Selecione o ESP32...") {
                    selectedDevice = pairedDevicesList.first() // Seleciona o primeiro da lista por padrão
                }
            } else {
                Toast.makeText(context, "Nenhum dispositivo pareado encontrado.", Toast.LENGTH_SHORT).show()
            }
        } catch (e: SecurityException) {
            Toast.makeText(context, "Permissão de Bluetooth necessária para listar.", Toast.LENGTH_SHORT).show()
        }
    }

    fun checkPermissionsAndConnect() {
        val allPermissionsGranted = permissionsToRequest.all {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        }

        if (allPermissionsGranted) {
            // Se já tem as permissões, primeiro carrega a lista para o menu (caso ainda não tenha carregado)
            if (pairedDevicesList.isEmpty()) carregarDispositivosPareados()

            // Só tenta conectar se o usuário não estiver na opção padrão
            if (selectedDevice != "Selecione o ESP32...") {
                coroutineScope.launch(Dispatchers.IO) {
                    // Usa a variável em vez do nome fixo!
                    val success = BTManager.connect(context, selectedDevice)
                    isConnected = success
                    withContext(Dispatchers.Main) {
                        if (success) Toast.makeText(context, "Conectado a $selectedDevice!", Toast.LENGTH_SHORT).show()
                        else Toast.makeText(context, "Falha na conexão com $selectedDevice.", Toast.LENGTH_SHORT).show()
                    }
                }
            } else {
                Toast.makeText(context, "Por favor, selecione um dispositivo na lista primeiro.", Toast.LENGTH_SHORT).show()
            }
        } else {
            permissionLauncher.launch(permissionsToRequest)
        }
    }

    // --- VARIÁVEIS DE ESTADO ---
    var delayValue by remember { mutableStateOf("100") }
    var repsValue by remember { mutableStateOf("5") }

    val baudOptions = listOf("9600", "19200", "38400", "57600", "115200")
    var expandedBaud by remember { mutableStateOf(false) }
    var selectedBaud by remember { mutableStateOf(baudOptions[0]) }

    val parityOptions = listOf("None", "Even (Par)", "Odd (Impar)")
    var expandedParity by remember { mutableStateOf(false) }
    var selectedParity by remember { mutableStateOf(parityOptions[0]) }

    val stopOptions = listOf("1 Bit", "2 Bits")
    var expandedStop by remember { mutableStateOf(false) }
    var selectedStop by remember { mutableStateOf(stopOptions[0]) }

    val bitStates = remember { mutableStateListOf(false, true, false, true, false, true, false, true) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
            .verticalScroll(scrollState),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = "Gerador Teste RS485",
            fontSize = 24.sp,
            fontWeight = FontWeight.Bold,
            color = Color.Blue
        )
        Spacer(modifier = Modifier.height(16.dp))

        // --- MENU: SELECIONAR DISPOSITIVO BLUETOOTH ---
        Row(
            modifier = Modifier.fillMaxWidth(0.9f),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Dropdown de Dispositivos
            ExposedDropdownMenuBox(
                expanded = expandedDeviceMenu,
                onExpandedChange = {
                    expandedDeviceMenu = !expandedDeviceMenu
                    if (expandedDeviceMenu && pairedDevicesList.isEmpty()) {
                        // Tenta carregar os dispositivos ao abrir o menu
                        carregarDispositivosPareados()
                    }
                },
                modifier = Modifier.weight(1f).padding(end = 8.dp)
            ) {
                OutlinedTextField(
                    value = selectedDevice,
                    onValueChange = {},
                    readOnly = true,
                    label = { Text("Dispositivo Pareado") },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expandedDeviceMenu) },
                    modifier = Modifier.menuAnchor(type = MenuAnchorType.PrimaryNotEditable).fillMaxWidth()
                )
                ExposedDropdownMenu(
                    expanded = expandedDeviceMenu,
                    onDismissRequest = { expandedDeviceMenu = false }
                ) {
                    pairedDevicesList.forEach { option ->
                        DropdownMenuItem(
                            text = { Text(option) },
                            onClick = { selectedDevice = option; expandedDeviceMenu = false }
                        )
                    }
                }
            }

            // Botão de Atualizar a Lista (Recarregar)
            IconButton(onClick = { carregarDispositivosPareados() }) {
                Text("🔄", fontSize = 24.sp) // Pode trocar por um ícone do Material Design se preferir
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // --- BOTÃO DE CONECTAR ---
        Button(
            onClick = { checkPermissionsAndConnect() },
            modifier = Modifier.fillMaxWidth(0.9f).height(50.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = if (isConnected) Color(0xFF4CAF50) else Color(0xFF2196F3)
            )
        ) {
            Text(if (isConnected) "CONECTADO A $selectedDevice" else "CONECTAR")
        }

        Spacer(modifier = Modifier.height(16.dp))
        HorizontalDivider()
        Spacer(modifier = Modifier.height(16.dp))

        // --- MENU: BAUD RATE ---
        ExposedDropdownMenuBox(
            expanded = expandedBaud,
            onExpandedChange = { expandedBaud = !expandedBaud }
        ) {
            OutlinedTextField(
                value = selectedBaud,
                onValueChange = {},
                readOnly = true,
                label = { Text("Baud Rate") },
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expandedBaud) },
                modifier = Modifier.menuAnchor(type = MenuAnchorType.PrimaryNotEditable).fillMaxWidth(0.9f)
            )
            ExposedDropdownMenu(
                expanded = expandedBaud,
                onDismissRequest = { expandedBaud = false }
            ) {
                baudOptions.forEach { option ->
                    DropdownMenuItem(
                        text = { Text(option) },
                        onClick = { selectedBaud = option; expandedBaud = false }
                    )
                }
            }
        }
        Spacer(modifier = Modifier.height(8.dp))

        // --- MENUS LADO A LADO: PARIDADE E STOP BITS ---
        Row(
            modifier = Modifier.fillMaxWidth(0.9f),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            ExposedDropdownMenuBox(
                expanded = expandedParity,
                onExpandedChange = { expandedParity = !expandedParity },
                modifier = Modifier.weight(1f).padding(end = 4.dp)
            ) {
                OutlinedTextField(
                    value = selectedParity,
                    onValueChange = {},
                    readOnly = true,
                    label = { Text("Paridade") },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expandedParity) },
                    modifier = Modifier.menuAnchor(type = MenuAnchorType.PrimaryNotEditable).fillMaxWidth(0.9f)
                )
                ExposedDropdownMenu(
                    expanded = expandedParity,
                    onDismissRequest = { expandedParity = false }
                ) {
                    parityOptions.forEach { option ->
                        DropdownMenuItem(
                            text = { Text(option) },
                            onClick = { selectedParity = option; expandedParity = false }
                        )
                    }
                }
            }

            ExposedDropdownMenuBox(
                expanded = expandedStop,
                onExpandedChange = { expandedStop = !expandedStop },
                modifier = Modifier.weight(1f).padding(start = 4.dp)
            ) {
                OutlinedTextField(
                    value = selectedStop,
                    onValueChange = {},
                    readOnly = true,
                    label = { Text("Stop Bits") },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expandedStop) },
                    modifier = Modifier.menuAnchor(type = MenuAnchorType.PrimaryNotEditable).fillMaxWidth(0.9f)
                )
                ExposedDropdownMenu(
                    expanded = expandedStop,
                    onDismissRequest = { expandedStop = false }
                ) {
                    stopOptions.forEach { option ->
                        DropdownMenuItem(
                            text = { Text(option) },
                            onClick = { selectedStop = option; expandedStop = false }
                        )
                    }
                }
            }
        }
        Spacer(modifier = Modifier.height(8.dp))

        // --- CAMPOS: DELAY E REPS ---
        Row(
            modifier = Modifier.fillMaxWidth(0.9f),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            OutlinedTextField(
                value = delayValue,
                onValueChange = { delayValue = it },
                label = { Text("Delay (ms)") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.weight(1f).padding(end = 4.dp)
            )
            OutlinedTextField(
                value = repsValue,
                onValueChange = { repsValue = it },
                label = { Text("Repetições") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.weight(1f).padding(start = 4.dp)
            )
        }

        Spacer(modifier = Modifier.height(24.dp))
        Text(text = "Mensagem (Byte a Injetar):", fontWeight = FontWeight.SemiBold)
        Spacer(modifier = Modifier.height(16.dp))

        // --- CHAVES EM GRID (2 COLUNAS x 4 LINHAS) ---
        Row(
            modifier = Modifier.fillMaxWidth(0.9f),
            horizontalArrangement = Arrangement.SpaceEvenly
        ) {
            Column {
                for (i in 0..3) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.padding(vertical = 4.dp)
                    ) {
                        Text(text = "Bit ${7 - i}:", fontSize = 16.sp, modifier = Modifier.width(45.dp))
                        Switch(checked = bitStates[i], onCheckedChange = { bitStates[i] = it })
                    }
                }
            }
            Column {
                for (i in 4..7) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.padding(vertical = 4.dp)
                    ) {
                        Text(text = "Bit ${7 - i}:", fontSize = 16.sp, modifier = Modifier.width(45.dp))
                        Switch(checked = bitStates[i], onCheckedChange = { bitStates[i] = it })
                    }
                }
            }
        }

        Spacer(modifier = Modifier.height(32.dp))

        // --- BOTÃO DE APLICAR ---
        Button(
            onClick = {
                val binaryString = bitStates.joinToString("") { if (it) "1" else "0" }
                val comandoCompleto = "BAUD:$selectedBaud\n" +
                        "PARITY:$selectedParity\n" +
                        "STOP:$selectedStop\n" +
                        "DELAY:$delayValue\n" +
                        "REPS:$repsValue\n" +
                        "MSG_BIN:$binaryString\n"

                BTManager.send(comandoCompleto)
            },
            modifier = Modifier.fillMaxWidth(0.9f).height(50.dp),
            enabled = isConnected
        ) {
            Text("ENVIAR PARA O GERADOR")
        }
        Spacer(modifier = Modifier.height(32.dp))
    }
}

@Preview
@Composable
fun TelaPreview() {
    MainScreen()
}