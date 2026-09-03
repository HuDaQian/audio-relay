enum RelayRole { sender, receiver }

enum ConnectionStateType {
  idle,
  connecting,
  awaitingPairingCode,
  streaming,
}

class ConnectionStatus {
  final ConnectionStateType type;
  final String? deviceName;
  final int? failedAttempts;

  ConnectionStatus({
    required this.type,
    this.deviceName,
    this.failedAttempts,
  });

  factory ConnectionStatus.fromMap(Map<dynamic, dynamic> map) {
    final typeStr = map['type'] as String? ?? 'idle';
    ConnectionStateType t;
    switch (typeStr) {
      case 'connecting':
        t = ConnectionStateType.connecting;
        break;
      case 'awaitingPairingCode':
        t = ConnectionStateType.awaitingPairingCode;
        break;
      case 'streaming':
        t = ConnectionStateType.streaming;
        break;
      default:
        t = ConnectionStateType.idle;
    }
    return ConnectionStatus(
      type: t,
      deviceName: map['deviceName'] as String?,
      failedAttempts: map['failedAttempts'] as int?,
    );
  }
}

class DiscoveredDevice {
  final String id;
  final String name;
  final String host;
  final int port;
  final bool paired;
  final bool isUsb;

  DiscoveredDevice({
    required this.id,
    required this.name,
    required this.host,
    required this.port,
    required this.paired,
    this.isUsb = false,
  });

  factory DiscoveredDevice.fromMap(Map<dynamic, dynamic> map) {
    final host = map['host'] as String? ?? '';
    final isUsb = host.startsWith('192.168.42.') || host == '127.0.0.1';
    return DiscoveredDevice(
      id: map['id'] as String? ?? '',
      name: map['name'] as String? ?? 'Unknown',
      host: host,
      port: map['port'] as int? ?? 45108,
      paired: map['paired'] as bool? ?? false,
      isUsb: isUsb,
    );
  }
}
