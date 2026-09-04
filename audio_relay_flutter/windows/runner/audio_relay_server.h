#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include "wasapi_capture.h"

#pragma comment(lib, "ws2_32.lib")

namespace audio_relay {

class WindowsAudioRelayServer {
public:
    using StatusCallback = std::function<void(const std::string& status, const std::string& client_name)>;

    static WindowsAudioRelayServer& Instance();

    void Start();
    void Stop();

    void SetStatusCallback(StatusCallback cb) { status_callback_ = std::move(cb); }
    void GenerateNewPairCode();
    void TriggerStartCapture();

    std::string GetPairCode() const { return pair_code_; }
    int GetPort() const { return 45108; }
    std::string GetDeviceName() const { return device_name_; }
    bool IsCapturing() const { return capture_.IsRunning(); }

private:
    WindowsAudioRelayServer();
    ~WindowsAudioRelayServer();

    void InitNetwork();
    void TcpControlLoop();
    void HandleControlClient(SOCKET client_sock, sockaddr_in client_addr);
    void TcpAudioLoop();
    void AdbSupervisorLoop();
    void MdnsLoop();

    void SendJson(SOCKET sock, const std::string& json_str);
    void SendAudioFrame(const std::vector<uint8_t>& pcm);
    void LoadConfig();
    void SaveConfig();

    std::string pair_code_;
    std::string device_name_;
    std::string device_id_;

    std::vector<uint8_t> session_id_;
    std::vector<uint8_t> session_key_;
    std::map<std::string, std::vector<uint8_t>> paired_keys_;

    uint32_t sequence_{0};

    WasapiCapture capture_;
    StatusCallback status_callback_;

    std::atomic<bool> is_running_{false};
    std::thread tcp_control_thread_;
    std::thread tcp_audio_thread_;
    std::thread adb_thread_;
    std::thread mdns_thread_;

    SOCKET tcp_control_listen_sock_{INVALID_SOCKET};
    SOCKET tcp_audio_listen_sock_{INVALID_SOCKET};
    SOCKET active_audio_tcp_sock_{INVALID_SOCKET};
    SOCKET udp_sock_{INVALID_SOCKET};
    SOCKET mdns_sock_{INVALID_SOCKET};

    sockaddr_in active_udp_addr_{};
    bool has_active_udp_{false};
    std::mutex net_mutex_;
};

} // namespace audio_relay
