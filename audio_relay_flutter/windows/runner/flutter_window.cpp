#include "flutter_window.h"

#include <optional>

#include "flutter/generated_plugin_registrant.h"

#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <flutter/encodable_value.h>
#include <shellapi.h>
#include "audio_relay_server.h"

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  // Setup desktop MethodChannels
  auto messenger = flutter_controller_->engine()->messenger();
  desktop_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      messenger, "com.audiorelay.flutter/desktop",
      &flutter::StandardMethodCodec::GetInstance());
  desktop_events_channel_ = std::make_shared<flutter::MethodChannel<flutter::EncodableValue>>(
      messenger, "com.audiorelay.flutter/desktop_events",
      &flutter::StandardMethodCodec::GetInstance());

  auto& server = audio_relay::WindowsAudioRelayServer::Instance();
  auto events_chan = desktop_events_channel_;
  server.SetStatusCallback([events_chan](const std::string& status, const std::string& client_name) {
    flutter::EncodableMap args;
    args[flutter::EncodableValue("status")] = flutter::EncodableValue(status);
    if (!client_name.empty()) {
      args[flutter::EncodableValue("clientName")] = flutter::EncodableValue(client_name);
    }
    events_chan->InvokeMethod("onStatusChanged", std::make_unique<flutter::EncodableValue>(args));
  });

  server.Start();

  desktop_channel_->SetMethodCallHandler(
      [](const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
        auto& s = audio_relay::WindowsAudioRelayServer::Instance();
        if (call.method_name() == "getServerInfo") {
          flutter::EncodableMap res;
          res[flutter::EncodableValue("pairCode")] = flutter::EncodableValue(s.GetPairCode());
          res[flutter::EncodableValue("port")] = flutter::EncodableValue(s.GetPort());
          res[flutter::EncodableValue("deviceName")] = flutter::EncodableValue(s.GetDeviceName());
          res[flutter::EncodableValue("hasPermission")] = flutter::EncodableValue(s.IsCapturing());
          result->Success(flutter::EncodableValue(res));
        } else if (call.method_name() == "checkPermission") {
          result->Success(flutter::EncodableValue(s.IsCapturing()));
        } else if (call.method_name() == "requestPermission") {
          s.TriggerStartCapture();
          result->Success(flutter::EncodableValue(true));
        } else if (call.method_name() == "openPermissionSettings") {
          ShellExecuteA(nullptr, "open", "ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
          result->Success(flutter::EncodableValue(true));
        } else if (call.method_name() == "startCapture") {
          s.TriggerStartCapture();
          result->Success(flutter::EncodableValue(true));
        } else if (call.method_name() == "regenerateCode") {
          s.GenerateNewPairCode();
          result->Success(flutter::EncodableValue(true));
        } else {
          result->NotImplemented();
        }
      });

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  audio_relay::WindowsAudioRelayServer::Instance().Stop();
  desktop_channel_ = nullptr;
  desktop_events_channel_ = nullptr;

  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
