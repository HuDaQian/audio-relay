import 'dart:async';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import '../models/models.dart';

class RelayPlatformService {
  static const MethodChannel _methodChannel =
      MethodChannel('com.audiorelay.flutter/relay');
  static const EventChannel _eventChannel =
      EventChannel('com.audiorelay.flutter/events');

  final ValueNotifier<ConnectionStatus> statusNotifier =
      ValueNotifier(ConnectionStatus(type: ConnectionStateType.idle));
  final ValueNotifier<List<DiscoveredDevice>> discoveredNotifier =
      ValueNotifier([]);
  final ValueNotifier<double> audioLevelNotifier = ValueNotifier(0.0);
  final ValueNotifier<bool> hasUsbTetherNotifier = ValueNotifier(false);

  StreamSubscription? _eventSubscription;

  void init() {
    if (Platform.isAndroid || Platform.isIOS) {
      _startService();
      _listenEvents();
      checkWiredNetwork();
    }
  }

  void dispose() {
    _eventSubscription?.cancel();
    statusNotifier.dispose();
    discoveredNotifier.dispose();
    audioLevelNotifier.dispose();
    hasUsbTetherNotifier.dispose();
  }

  Future<void> _startService() async {
    try {
      await _methodChannel.invokeMethod('startService');
    } catch (e) {
      debugPrint('startService error: $e');
    }
  }

  void _listenEvents() {
    _eventSubscription = _eventChannel.receiveBroadcastStream().listen(
      (event) {
        if (event is Map) {
          final type = event['event'] as String?;
          final data = event['data'];
          if (type == 'status' && data is Map) {
            statusNotifier.value = ConnectionStatus.fromMap(data);
          } else if (type == 'discovered' && data is List) {
            final devices = data
                .map((e) => DiscoveredDevice.fromMap(e as Map))
                .toList();
            // Prioritize USB connections at the top
            devices.sort((a, b) {
              if (a.isUsb && !b.isUsb) return -1;
              if (!a.isUsb && b.isUsb) return 1;
              return a.name.compareTo(b.name);
            });
            discoveredNotifier.value = devices;
          } else if (type == 'audioLevel' && data is num) {
            audioLevelNotifier.value = data.toDouble();
          }
        }
      },
      onError: (err) {
        debugPrint('EventChannel error: $err');
      },
    );
  }

  Future<void> checkWiredNetwork() async {
    if (!Platform.isAndroid) return;
    try {
      final res = await _methodChannel.invokeMethod<Map>('checkWiredNetwork');
      if (res != null) {
        final hasTether = res['hasUsbTether'] as bool? ?? false;
        hasUsbTetherNotifier.value = hasTether;
      }
    } catch (e) {
      debugPrint('checkWiredNetwork error: $e');
    }
  }

  Future<void> connect(String host, int port) async {
    try {
      await _methodChannel.invokeMethod('connect', {
        'host': host,
        'port': port,
      });
    } catch (e) {
      debugPrint('connect error: $e');
    }
  }

  Future<void> disconnect() async {
    try {
      await _methodChannel.invokeMethod('disconnect');
    } catch (e) {
      debugPrint('disconnect error: $e');
    }
  }

  Future<void> submitPairingCode(String code) async {
    try {
      await _methodChannel.invokeMethod('submitPairingCode', {
        'code': code,
      });
    } catch (e) {
      debugPrint('submitPairingCode error: $e');
    }
  }
}
