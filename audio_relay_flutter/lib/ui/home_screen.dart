import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../models/models.dart';
import '../services/relay_service.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final RelayPlatformService _service = RelayPlatformService();
  final TextEditingController _codeController = TextEditingController();

  static const MethodChannel _desktopChannel = MethodChannel('com.audiorelay.flutter/desktop');
  String _desktopPairCode = '......';
  String _desktopStatus = '正在启动音频服务...';
  String? _desktopConnectedClient;
  bool _hasAudioPermission = true;
  Timer? _permissionTimer;

  @override
  void initState() {
    super.initState();
    _service.init();
    _service.statusNotifier.addListener(_onStatusChanged);

    if (Platform.isMacOS || Platform.isWindows) {
      _initDesktop();
    }
  }

  Future<void> _initDesktop() async {
    _desktopChannel.setMethodCallHandler((call) async {
      if (call.method == 'onStatusChanged') {
        final args = call.arguments as Map?;
        final status = args?['status'] as String? ?? 'listening';
        final client = args?['clientName'] as String?;
        if (mounted) {
          setState(() {
            if (status == 'streaming') {
              _desktopStatus = '正在向 ${client ?? "手机"} 串流音频';
              _desktopConnectedClient = client;
            } else if (status == 'pairing') {
              _desktopStatus = '正在与 ${client ?? "手机"} 配对握手...';
            } else {
              _desktopStatus = '音频广播服务运行中 (等待设备连接)';
              _desktopConnectedClient = null;
            }
          });
        }
      }
    });

    try {
      final info = await _desktopChannel.invokeMethod<Map>('getServerInfo');
      if (info != null && mounted) {
        setState(() {
          _desktopPairCode = info['pairCode'] as String? ?? '123456';
          _desktopStatus = '音频广播服务运行中 (等待设备连接)';
          _hasAudioPermission = info['hasPermission'] as bool? ?? true;
        });
      }
    } catch (e) {
      debugPrint('Desktop channel error: $e');
    }

    if (Platform.isMacOS) {
      _permissionTimer?.cancel();
      _permissionTimer = Timer.periodic(const Duration(seconds: 2), (t) async {
        if (!mounted) {
          t.cancel();
          return;
        }
        try {
          final granted = await _desktopChannel.invokeMethod<bool>('checkPermission') ?? true;
          if (granted != _hasAudioPermission && mounted) {
            setState(() {
              _hasAudioPermission = granted;
            });
          }
        } catch (_) {}
      });
    }
  }

  @override
  void dispose() {
    _permissionTimer?.cancel();
    _service.statusNotifier.removeListener(_onStatusChanged);
    _service.dispose();
    _codeController.dispose();
    super.dispose();
  }

  void _onStatusChanged() {
    final status = _service.statusNotifier.value;
    if (status.type == ConnectionStateType.awaitingPairingCode) {
      _showPairingCodeDialog(status.deviceName ?? '电脑');
    }
  }

  void _showPairingCodeDialog(String deviceName) {
    _codeController.clear();
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (ctx) {
        return AlertDialog(
          title: Text('配对认证 - $deviceName'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text('请输入电脑屏幕上显示的 6 位配对码：'),
              const SizedBox(height: 16),
              TextField(
                controller: _codeController,
                keyboardType: TextInputType.number,
                maxLength: 6,
                autofocus: true,
                textAlign: TextAlign.center,
                style: const TextStyle(
                  fontSize: 28,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 8,
                ),
                decoration: const InputDecoration(
                  hintText: '000000',
                  border: OutlineInputBorder(),
                  counterText: '',
                ),
              ),
            ],
          ),
          actions: [
            TextButton(
              onPressed: () {
                Navigator.of(ctx).pop();
                _service.disconnect();
              },
              child: const Text('取消'),
            ),
            FilledButton(
              onPressed: () {
                final code = _codeController.text.trim();
                if (code.length == 6) {
                  Navigator.of(ctx).pop();
                  _service.submitPairingCode(code);
                }
              },
              child: const Text('确认配对'),
            ),
          ],
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final isDesktop = Platform.isWindows || Platform.isMacOS || Platform.isLinux;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Audio Relay'),
        centerTitle: true,
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: '刷新有线与网络状态',
            onPressed: () => _service.checkWiredNetwork(),
          ),
        ],
      ),
      body: isDesktop ? _buildDesktopSenderView() : _buildMobileReceiverView(),
    );
  }

  // --- Mobile Receiver UI ---
  Widget _buildMobileReceiverView() {
    return ValueListenableBuilder<ConnectionStatus>(
      valueListenable: _service.statusNotifier,
      builder: (context, status, _) {
        if (status.type == ConnectionStateType.streaming) {
          return _buildStreamingView(status);
        } else if (status.type == ConnectionStateType.connecting) {
          return _buildConnectingView(status);
        }
        return _buildDiscoveryView();
      },
    );
  }

  Widget _buildStreamingView(ConnectionStatus status) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24.0),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const Icon(
              Icons.headphones_rounded,
              size: 72,
              color: Colors.green,
            ),
            const SizedBox(height: 16),
            Text(
              '正在接收音频',
              style: Theme.of(context).textTheme.headlineSmall?.copyWith(
                    fontWeight: FontWeight.bold,
                  ),
            ),
            const SizedBox(height: 8),
            Text(
              '来源：${status.deviceName ?? "电脑"}',
              style: Theme.of(context).textTheme.bodyLarge?.copyWith(
                    color: Colors.grey[700],
                  ),
            ),
            const SizedBox(height: 24),
            // Live Audio Level Meter
            ValueListenableBuilder<double>(
              valueListenable: _service.audioLevelNotifier,
              builder: (context, level, _) {
                return Column(
                  children: [
                    ClipRRect(
                      borderRadius: BorderRadius.circular(8),
                      child: LinearProgressIndicator(
                        value: level,
                        minHeight: 16,
                        backgroundColor: Colors.grey[300],
                        valueColor: const AlwaysStoppedAnimation(Colors.green),
                      ),
                    ),
                    const SizedBox(height: 6),
                    Text(
                      '音量电平: ${(level * 100).toInt()}%',
                      style: Theme.of(context).textTheme.bodySmall,
                    ),
                  ],
                );
              },
            ),
            const SizedBox(height: 36),
            FilledButton.tonalIcon(
              onPressed: () => _service.disconnect(),
              icon: const Icon(Icons.stop_rounded),
              label: const Text('断开连接'),
              style: FilledButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 32, vertical: 16),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildConnectingView(ConnectionStatus status) {
    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const CircularProgressIndicator(),
          const SizedBox(height: 24),
          Text(
            '正在连接 ${status.deviceName ?? "电脑"}...',
            style: Theme.of(context).textTheme.titleMedium,
          ),
          const SizedBox(height: 24),
          OutlinedButton(
            onPressed: () => _service.disconnect(),
            child: const Text('取消'),
          ),
        ],
      ),
    );
  }

  Widget _buildDiscoveryView() {
    return ValueListenableBuilder<bool>(
      valueListenable: _service.hasUsbTetherNotifier,
      builder: (context, hasUsbTether, _) {
        return ListView(
          padding: const EdgeInsets.all(16),
          children: [
            if (Platform.isIOS) ...[
              Card(
                color: Colors.amber.shade50,
                elevation: 0,
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(16),
                  side: BorderSide(color: Colors.amber.shade300),
                ),
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: Row(
                    children: [
                      Icon(Icons.info_outline_rounded, color: Colors.amber.shade800),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Text(
                          'iOS 暂不支持音频接收功能。目前移动端仅支持 Android 设备的低延迟 AudioTrack 播放。',
                          style: TextStyle(color: Colors.amber.shade900, fontSize: 13),
                        ),
                      ),
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 12),
            ],
            // USB Tethering Banner (Option 1)
            Card(
              color: hasUsbTether
                  ? Colors.green.shade50
                  : Colors.blueGrey.shade50,
              elevation: 0,
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(16),
                side: BorderSide(
                  color: hasUsbTether
                      ? Colors.green.shade300
                      : Colors.blueGrey.shade200,
                ),
              ),
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Icon(
                          Icons.cable_rounded,
                          color: hasUsbTether
                              ? Colors.green.shade800
                              : Colors.blueGrey.shade700,
                        ),
                        const SizedBox(width: 8),
                        Text(
                          hasUsbTether ? 'USB 共享网络已就绪' : '数据线有线模式',
                          style: TextStyle(
                            fontSize: 16,
                            fontWeight: FontWeight.bold,
                            color: hasUsbTether
                                ? Colors.green.shade900
                                : Colors.blueGrey.shade900,
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 8),
                    Text(
                      hasUsbTether
                          ? '检测到 USB 网络共享接口，可享受 <5ms 极低延迟与零抖动串流！'
                          : '插上数据线后，可开启手机“USB 网络共享”或使用 ADB 调试以获得最佳音画同步体验。',
                      style: TextStyle(
                        color: hasUsbTether
                            ? Colors.green.shade900
                            : Colors.blueGrey.shade700,
                        fontSize: 13,
                      ),
                    ),
                    const SizedBox(height: 12),
                    Wrap(
                      spacing: 8,
                      children: [
                        if (hasUsbTether)
                          FilledButton.icon(
                            onPressed: () => _service.connect('192.168.42.1', 45108),
                            icon: const Icon(Icons.flash_on_rounded, size: 18),
                            label: const Text('USB 极速连接 (192.168.42.1)'),
                            style: FilledButton.styleFrom(
                              backgroundColor: Colors.green.shade700,
                            ),
                          ),
                        OutlinedButton.icon(
                          onPressed: () => _service.connect('127.0.0.1', 45108),
                          icon: const Icon(Icons.usb_rounded, size: 18),
                          label: const Text('ADB 端口直连 (127.0.0.1)'),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 16),
            Text(
              '局域网已发现的电脑',
              style: Theme.of(context).textTheme.titleSmall?.copyWith(
                    fontWeight: FontWeight.bold,
                    color: Colors.grey[700],
                  ),
            ),
            const SizedBox(height: 8),
            ValueListenableBuilder<List<DiscoveredDevice>>(
              valueListenable: _service.discoveredNotifier,
              builder: (context, list, _) {
                if (list.isEmpty) {
                  return const Padding(
                    padding: EdgeInsets.symmetric(vertical: 36),
                    child: Center(
                      child: Text(
                        '正在扫描局域网中的电脑...\n请确保电脑端应用正在运行且在同一局域网或插好数据线',
                        textAlign: TextAlign.center,
                        style: TextStyle(color: Colors.grey),
                      ),
                    ),
                  );
                }
                return Column(
                  children: list.map((dev) {
                    return Card(
                      margin: const EdgeInsets.only(bottom: 8),
                      child: ListTile(
                        leading: CircleAvatar(
                          backgroundColor: dev.isUsb
                              ? Colors.green.shade100
                              : Colors.blue.shade100,
                          child: Icon(
                            dev.isUsb ? Icons.cable_rounded : Icons.computer_rounded,
                            color: dev.isUsb
                                ? Colors.green.shade800
                                : Colors.blue.shade800,
                          ),
                        ),
                        title: Text(
                          dev.name,
                          style: const TextStyle(fontWeight: FontWeight.bold),
                        ),
                        subtitle: Text('${dev.host}:${dev.port} ${dev.paired ? "• 已配对" : ""}'),
                        trailing: FilledButton.tonal(
                          onPressed: () => _service.connect(dev.host, dev.port),
                          child: const Text('连接'),
                        ),
                      ),
                    );
                  }).toList(),
                );
              },
            ),
          ],
        );
      },
    );
  }

  // --- Desktop Sender UI ---
  Widget _buildDesktopSenderView() {
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 540),
        child: Padding(
          padding: const EdgeInsets.all(24.0),
          child: Card(
            elevation: 2,
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
            child: Padding(
              padding: const EdgeInsets.all(24.0),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Container(
                        width: 12,
                        height: 12,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          color: _desktopConnectedClient != null ? Colors.blue : Colors.green,
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Text(
                          _desktopStatus,
                          style: const TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 16),
                  const Text('音频中继端口：45108 (TCP 控制 / UDP 音频)'),
                  const Text('捕获后端：ScreenCaptureKit (macOS) / WASAPI (Windows)'),
                  const SizedBox(height: 8),
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    crossAxisAlignment: WrapCrossAlignment.center,
                    children: [
                      if (_hasAudioPermission)
                        Container(
                          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                          decoration: BoxDecoration(
                            color: Colors.green.shade50,
                            borderRadius: BorderRadius.circular(8),
                            border: Border.all(color: Colors.green.shade300),
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Icon(Icons.check_circle_rounded, color: Colors.green.shade700, size: 16),
                              const SizedBox(width: 6),
                              Text(
                                '系统音频捕获：正在运行',
                                style: TextStyle(fontSize: 13, color: Colors.green.shade900, fontWeight: FontWeight.bold),
                              ),
                            ],
                          ),
                        )
                      else ...[
                        FilledButton.tonalIcon(
                          onPressed: () => _desktopChannel.invokeMethod('startCapture'),
                          icon: const Icon(Icons.play_circle_outline_rounded, size: 18),
                          label: const Text('启动系统内录'),
                        ),
                        OutlinedButton.icon(
                          onPressed: () => _desktopChannel.invokeMethod('openPermissionSettings'),
                          icon: const Icon(Icons.settings_suggest_rounded, size: 18),
                          label: const Text('系统设置授权'),
                        ),
                      ],
                    ],
                  ),
                  const Divider(height: 32),
                  const Text('配对验证码 (请在手机端输入)：', style: TextStyle(fontWeight: FontWeight.bold)),
                  const SizedBox(height: 8),
                  Container(
                    width: double.infinity,
                    padding: const EdgeInsets.symmetric(vertical: 16),
                    decoration: BoxDecoration(
                      color: Colors.teal.shade50,
                      borderRadius: BorderRadius.circular(12),
                      border: Border.all(color: Colors.teal.shade200),
                    ),
                    child: Text(
                      _desktopPairCode.split('').join(' '),
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        fontSize: 36,
                        fontWeight: FontWeight.bold,
                        letterSpacing: 8,
                        color: Colors.teal.shade900,
                      ),
                    ),
                  ),
                  const SizedBox(height: 16),
                  Row(
                    children: [
                      Icon(
                        _desktopConnectedClient != null ? Icons.cable_rounded : Icons.check_circle_rounded,
                        color: _desktopConnectedClient != null ? Colors.blue : Colors.green,
                        size: 20,
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Text(
                          _desktopConnectedClient != null
                              ? '已连接设备：$_desktopConnectedClient (音频实时传输中)'
                              : 'ADB 自动反向代理已就绪：插入 Android 手机将自动映射 127.0.0.1',
                          style: const TextStyle(fontSize: 13, color: Colors.black87),
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
