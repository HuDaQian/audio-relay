import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate, FlutterStreamHandler {
  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    let controller: FlutterViewController = window?.rootViewController as! FlutterViewController
    let relayChannel = FlutterMethodChannel(name: "com.audiorelay.flutter/relay",
                                            binaryMessenger: controller.binaryMessenger)
    relayChannel.setMethodCallHandler { (call, result) in
      switch call.method {
      case "startService", "disconnect", "submitPairingCode", "connect":
        result(FlutterError(code: "UNSUPPORTED_PLATFORM",
                            message: "Audio receiver service is currently supported on Android devices.",
                            details: nil))
      case "checkWiredNetwork":
        result(["hasUsbTether": false])
      default:
        result(FlutterMethodNotImplemented)
      }
    }

    let eventChannel = FlutterEventChannel(name: "com.audiorelay.flutter/events",
                                           binaryMessenger: controller.binaryMessenger)
    eventChannel.setStreamHandler(self)

    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)
  }

  // FlutterStreamHandler stubs
  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    return nil
  }
}

