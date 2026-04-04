package com.example.analisadorrs485basic

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Context
import java.io.OutputStream
import java.util.UUID

// O @SuppressLint avisa o Android Studio para parar de sublinhar de vermelho pedindo checagens
// rigorosas de permissão. Como estamos na fase de protótipo, vamos ignorar os alertas de lint.
@SuppressLint("MissingPermission")
object BTManager {
    private var socket: BluetoothSocket? = null
    private var outStream: OutputStream? = null

    // Esse é o UUID "Mágico" padrão mundial para comunicação Serial Bluetooth Classic (SPP)
    private val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805f9b34fb")

    // Retorna true se conectou, false se falhou
    fun connect(context: Context, deviceName: String = "ESP_SPP_ACCEPTOR"): Boolean {
        try {
            val btManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
            val adapter = btManager.adapter ?: return false

            // Vasculha a lista de dispositivos que VOCÊ JÁ PAREOU no menu do celular
            val pairedDevices = adapter.bondedDevices
            val espDevice = pairedDevices.find { it.name == deviceName }

            if (espDevice != null) {
                // Cria o "cabo virtual"
                socket = espDevice.createRfcommSocketToServiceRecord(SPP_UUID)
                socket?.connect() // Tenta conectar fisicamente
                outStream = socket?.outputStream
                println("Conectado com sucesso ao $deviceName!")
                return true
            } else {
                println("Dispositivo $deviceName não encontrado na lista de pareados.")
            }
        } catch (e: Exception) {
            println("Erro ao conectar: ${e.message}")
            e.printStackTrace()
        }
        return false
    }

    // Envia o texto pela porta serial virtual
    fun send(message: String) {
        try {
            outStream?.write(message.toByteArray())
            println("Enviado: $message")
        } catch (e: Exception) {
            println("Erro ao enviar: ${e.message}")
            e.printStackTrace()
        }
    }

    // Desconecta e libera a porta
    fun disconnect() {
        try {
            outStream?.close()
            socket?.close()
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }
}