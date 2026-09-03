package com.audiorelay.flutter.audio_relay_flutter

import com.audiorelay.flutter.audio_relay_flutter.bridge.RelayBridge
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine

class MainActivity : FlutterActivity() {
    private var relayBridge: RelayBridge? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        relayBridge = RelayBridge(this, flutterEngine.dartExecutor.binaryMessenger)
    }
}
