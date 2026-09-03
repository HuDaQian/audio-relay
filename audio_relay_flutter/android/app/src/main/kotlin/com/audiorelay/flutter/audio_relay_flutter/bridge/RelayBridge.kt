package com.audiorelay.flutter.audio_relay_flutter.bridge

import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import com.audiorelay.flutter.audio_relay_flutter.service.RelayService
import com.audiorelay.flutter.audio_relay_flutter.state.ConnectionStatus
import com.audiorelay.flutter.audio_relay_flutter.state.RelayState
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import java.net.NetworkInterface

class RelayBridge(private val context: Context, messenger: BinaryMessenger) : MethodChannel.MethodCallHandler {
    private val methodChannel = MethodChannel(messenger, "com.audiorelay.flutter/relay")
    private val eventChannel = EventChannel(messenger, "com.audiorelay.flutter/events")
    private val mainHandler = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(Dispatchers.Main + Job())

    private var eventSink: EventChannel.EventSink? = null

    init {
        methodChannel.setMethodCallHandler(this)
        eventChannel.setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                eventSink = events
                observeRelayState()
            }

            override fun onCancel(arguments: Any?) {
                eventSink = null
            }
        })
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "startService" -> {
                context.startForegroundService(Intent(context, RelayService::class.java))
                result.success(true)
            }
            "stopService" -> {
                context.stopService(Intent(context, RelayService::class.java))
                result.success(true)
            }
            "connect" -> {
                val host = call.argument<String>("host") ?: "127.0.0.1"
                val port = call.argument<Int>("port") ?: 45108
                val deviceId = call.argument<String>("deviceId") ?: "desktop-$host-$port"
                val name = call.argument<String>("name") ?: (if (host == "127.0.0.1") "USB 有线电脑" else "电脑")
                val intent = Intent(context, RelayService::class.java).apply {
                    action = RelayService.ACTION_CONNECT
                    putExtra(RelayService.EXTRA_DEVICE_ID, deviceId)
                    putExtra(RelayService.EXTRA_NAME, name)
                    putExtra(RelayService.EXTRA_HOST, host)
                    putExtra(RelayService.EXTRA_PORT, port)
                }
                context.startForegroundService(intent)
                result.success(true)
            }
            "disconnect" -> {
                val intent = Intent(context, RelayService::class.java).apply {
                    action = RelayService.ACTION_STOP
                }
                context.startService(intent)
                result.success(true)
            }
            "submitPairingCode" -> {
                val code = call.argument<String>("code") ?: ""
                val intent = Intent(context, RelayService::class.java).apply {
                    action = RelayService.ACTION_SUBMIT_PAIRING_CODE
                    putExtra(RelayService.EXTRA_CODE, code)
                }
                context.startService(intent)
                result.success(true)
            }
            "checkWiredNetwork" -> {
                val wiredInfo = checkWiredOptions()
                result.success(wiredInfo)
            }
            else -> result.notImplemented()
        }
    }

    private fun checkWiredOptions(): Map<String, Any> {
        var hasUsbTether = false
        var usbTetherIp: String? = null

        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val iface = interfaces.nextElement()
                val name = iface.name.lowercase()
                if (name.contains("rndis") || name.contains("usb")) {
                    for (addr in iface.inetAddresses) {
                        if (!addr.isLoopbackAddress && addr.hostAddress?.contains(":") == false) {
                            hasUsbTether = true
                            usbTetherIp = addr.hostAddress
                            break
                        }
                    }
                }
            }
        } catch (e: Exception) {
            // Ignore
        }

        return mapOf(
            "hasUsbTether" to hasUsbTether,
            "usbTetherIp" to (usbTetherIp ?: ""),
            "suggestedHost" to if (hasUsbTether) "192.168.42.1" else "127.0.0.1"
        )
    }

    private fun observeRelayState() {
        scope.launch {
            RelayState.status.collectLatest { status ->
                val connectedName = RelayState.connectedDeviceName.value
                val statusMap = when (status) {
                    ConnectionStatus.IDLE, ConnectionStatus.DISCONNECTED, ConnectionStatus.STOPPED ->
                        mapOf("type" to "idle")
                    ConnectionStatus.DISCOVERING ->
                        mapOf("type" to "idle")
                    ConnectionStatus.CONNECTING, ConnectionStatus.RECONNECTING ->
                        mapOf("type" to "connecting", "deviceName" to (connectedName ?: "电脑"))
                    ConnectionStatus.PAIRING_CODE_REQUIRED ->
                        mapOf("type" to "awaitingPairingCode", "deviceName" to (connectedName ?: "电脑"))
                    ConnectionStatus.STREAMING ->
                        mapOf("type" to "streaming", "deviceName" to (connectedName ?: "电脑"))
                    ConnectionStatus.NETWORK_CHANGED, ConnectionStatus.CONNECTION_TROUBLE ->
                        mapOf("type" to "connecting", "deviceName" to (connectedName ?: "电脑"))
                }
                sendEvent("status", statusMap)
            }
        }

        scope.launch {
            RelayState.discoveredLaptops.collectLatest { list ->
                val laptopList = list.map { l ->
                    mapOf(
                        "id" to l.deviceId,
                        "name" to l.name,
                        "host" to l.host,
                        "port" to l.port,
                        "paired" to false
                    )
                }
                sendEvent("discovered", laptopList)
            }
        }

        scope.launch {
            RelayState.playbackLevel.collectLatest { level ->
                sendEvent("audioLevel", level)
            }
        }
    }

    private fun sendEvent(eventType: String, data: Any) {
        mainHandler.post {
            eventSink?.success(mapOf("event" to eventType, "data" to data))
        }
    }
}
