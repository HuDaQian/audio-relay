#include "audio_relay_server.h"
#include "crypto.h"
#include <chrono>
#include <sstream>
#include <fstream>
#include <random>
#include <iomanip>
#include <iostream>

namespace audio_relay {

namespace {

// Simple JSON extraction helpers
std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    size_t end_pos = json.find('"', pos + 1);
    if (end_pos == std::string::npos) return "";
    return json.substr(pos + 1, end_pos - pos - 1);
}

int64_t ExtractJsonInt64(const std::string& json, const std::string& key, int64_t default_val = 0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return default_val;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return default_val;
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) {
        pos++;
    }
    try {
        return std::stoll(json.substr(pos));
    } catch (...) {
        return default_val;
    }
}

int ExtractJsonInt(const std::string& json, const std::string& key, int default_val = 0) {
    return (int)ExtractJsonInt64(json, key, default_val);
}

std::string ToHex(const uint8_t* data, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string res;
    res.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        res.push_back(hex_chars[(data[i] >> 4) & 0x0F]);
        res.push_back(hex_chars[data[i] & 0x0F]);
    }
    return res;
}

std::vector<uint8_t> FromHex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

std::string GetConfigPath() {
    char localAppData[MAX_PATH];
    DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return "audio_relay_config.json";
    }
    std::string dir = std::string(localAppData) + "\\AudioRelay";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\config.json";
}

} // anon namespace

WindowsAudioRelayServer& WindowsAudioRelayServer::Instance() {
    static WindowsAudioRelayServer server;
    return server;
}

WindowsAudioRelayServer::WindowsAudioRelayServer() {
    GenerateNewPairCode();

    char compName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(compName);
    if (GetComputerNameA(compName, &size)) {
        device_name_ = std::string(compName);
    } else {
        device_name_ = "Windows PC";
    }

    LoadConfig();

    if (device_id_.empty()) {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        uint64_t r1 = gen();
        uint64_t r2 = gen();
        device_id_ = "win-" + ToHex((const uint8_t*)&r1, 8) + ToHex((const uint8_t*)&r2, 8);
        SaveConfig();
    }
}

WindowsAudioRelayServer::~WindowsAudioRelayServer() {
    Stop();
}

void WindowsAudioRelayServer::LoadConfig() {
    std::ifstream file(GetConfigPath());
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::string id = ExtractJsonString(content, "device_id");
    if (!id.empty()) {
        device_id_ = id;
    }

    // Parse simple JSON list for paired_keys
    // Format: {"device_id":"...", "paired_keys":[{"device_id":"...", "key":"..."}, ...]}
    size_t pos = 0;
    while ((pos = content.find("\"device_id\":", pos)) != std::string::npos) {
        if (pos > 0 && content.substr(0, pos).find("\"paired_keys\"") != std::string::npos) {
            std::string d_id = ExtractJsonString(content.substr(pos), "device_id");
            std::string k_hex = ExtractJsonString(content.substr(pos), "key");
            if (!d_id.empty() && k_hex.size() == 64) {
                paired_keys_[d_id] = FromHex(k_hex);
            }
        }
        pos += 12;
    }
}

void WindowsAudioRelayServer::SaveConfig() {
    std::ofstream file(GetConfigPath(), std::ios::trunc);
    if (!file.is_open()) return;

    file << "{\n  \"device_id\": \"" << device_id_ << "\",\n  \"paired_keys\": [\n";
    size_t idx = 0;
    for (const auto& pair : paired_keys_) {
        file << "    {\"device_id\": \"" << pair.first << "\", \"key\": \"" << ToHex(pair.second.data(), pair.second.size()) << "\"}";
        if (++idx < paired_keys_.size()) file << ",";
        file << "\n";
    }
    file << "  ]\n}\n";
    file.close();
}

void WindowsAudioRelayServer::GenerateNewPairCode() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 999999);
    char buf[16];
    snprintf(buf, sizeof(buf), "%06d", dis(gen));
    pair_code_ = buf;
}

void WindowsAudioRelayServer::InitNetwork() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // 1. TCP Control Listener on 45108
    tcp_control_listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL opt = TRUE;
    setsockopt(tcp_control_listen_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(45108);

    bind(tcp_control_listen_sock_, (sockaddr*)&addr, sizeof(addr));
    listen(tcp_control_listen_sock_, 5);

    // 2. TCP Audio Stream Listener on 45109 (for ADB reverse streaming)
    tcp_audio_listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(tcp_audio_listen_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in audio_addr{};
    audio_addr.sin_family = AF_INET;
    audio_addr.sin_addr.s_addr = INADDR_ANY;
    audio_addr.sin_port = htons(45109);

    bind(tcp_audio_listen_sock_, (sockaddr*)&audio_addr, sizeof(audio_addr));
    listen(tcp_audio_listen_sock_, 5);

    // 3. UDP socket on 45108 for Wi-Fi streaming
    udp_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in udp_bind_addr{};
    udp_bind_addr.sin_family = AF_INET;
    udp_bind_addr.sin_addr.s_addr = INADDR_ANY;
    udp_bind_addr.sin_port = htons(45108);
    bind(udp_sock_, (sockaddr*)&udp_bind_addr, sizeof(udp_bind_addr));
}

void WindowsAudioRelayServer::Start() {
    if (is_running_.load()) return;

    InitNetwork();
    is_running_.store(true);

    if (status_callback_) {
        status_callback_("listening", "");
    }

    tcp_control_thread_ = std::thread(&WindowsAudioRelayServer::TcpControlLoop, this);
    tcp_audio_thread_ = std::thread(&WindowsAudioRelayServer::TcpAudioLoop, this);
    adb_thread_ = std::thread(&WindowsAudioRelayServer::AdbSupervisorLoop, this);

    TriggerStartCapture();
}

void WindowsAudioRelayServer::Stop() {
    if (!is_running_.load()) return;

    is_running_.store(false);
    capture_.Stop();

    if (tcp_control_listen_sock_ != INVALID_SOCKET) {
        closesocket(tcp_control_listen_sock_);
        tcp_control_listen_sock_ = INVALID_SOCKET;
    }
    if (tcp_audio_listen_sock_ != INVALID_SOCKET) {
        closesocket(tcp_audio_listen_sock_);
        tcp_audio_listen_sock_ = INVALID_SOCKET;
    }
    if (active_audio_tcp_sock_ != INVALID_SOCKET) {
        closesocket(active_audio_tcp_sock_);
        active_audio_tcp_sock_ = INVALID_SOCKET;
    }
    if (udp_sock_ != INVALID_SOCKET) {
        closesocket(udp_sock_);
        udp_sock_ = INVALID_SOCKET;
    }

    if (tcp_control_thread_.joinable()) tcp_control_thread_.join();
    if (tcp_audio_thread_.joinable()) tcp_audio_thread_.join();
    if (adb_thread_.joinable()) adb_thread_.join();

    WSACleanup();
}

void WindowsAudioRelayServer::TriggerStartCapture() {
    if (capture_.IsRunning()) return;

    capture_.Start([this](const std::vector<uint8_t>& pcm) {
        this->SendAudioFrame(pcm);
    });
}

void WindowsAudioRelayServer::SendJson(SOCKET sock, const std::string& json_str) {
    std::string msg = json_str + "\n";
    send(sock, msg.c_str(), (int)msg.size(), 0);
}

void WindowsAudioRelayServer::TcpControlLoop() {
    while (is_running_.load()) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET client = accept(tcp_control_listen_sock_, (sockaddr*)&client_addr, &addr_len);
        if (client == INVALID_SOCKET) {
            break;
        }

        std::thread([this, client, client_addr]() {
            this->HandleControlClient(client, client_addr);
        }).detach();
    }
}

void WindowsAudioRelayServer::HandleControlClient(SOCKET client_sock, sockaddr_in client_addr) {
    char buffer[2048];
    std::string line_accum;

    while (is_running_.load()) {
        int bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) break;

        buffer[bytes] = '\0';
        line_accum.append(buffer, bytes);

        size_t newline_pos;
        while ((newline_pos = line_accum.find('\n')) != std::string::npos) {
            std::string line = line_accum.substr(0, newline_pos);
            line_accum.erase(0, newline_pos + 1);

            if (line.empty()) continue;

            std::string type = ExtractJsonString(line, "type");
            if (type == "HELLO") {
                std::string client_name = ExtractJsonString(line, "device_name");
                if (client_name.empty()) client_name = "Android";
                int client_audio_port = ExtractJsonInt(line, "audio_port", 45108);
                client_device_id_ = ExtractJsonString(line, "device_id");

                std::random_device rd;
                std::mt19937_64 gen(rd());
                uint64_t nonce_val = gen();
                current_nonce_ = ToHex((const uint8_t*)&nonce_val, 8);

                bool is_paired = false;
                {
                    std::lock_guard<std::mutex> lock(net_mutex_);
                    is_paired = (paired_keys_.find(client_device_id_) != paired_keys_.end());
                }

                std::ostringstream oss;
                oss << "{\"type\":\"HELLO_ACK\","
                    << "\"protocol_version\":2,"
                    << "\"device_id\":\"" << device_id_ << "\","
                    << "\"device_name\":\"" << device_name_ << "\","
                    << "\"paired\":" << (is_paired ? "true" : "false") << ","
                    << "\"nonce\":\"" << current_nonce_ << "\"}";
                SendJson(client_sock, oss.str());

                {
                    std::lock_guard<std::mutex> lock(net_mutex_);
                    active_udp_addr_ = client_addr;
                    active_udp_addr_.sin_port = htons((u_short)client_audio_port);
                    has_active_udp_ = true;
                }

                if (status_callback_) {
                    status_callback_("pairing", client_name);
                }
            } else if (type == "PAIR_REQUEST") {
                // Protocol v2: proof = HMAC-SHA256(code, phone_device_id || nonce_from_HELLO_ACK)
                std::string client_proof = ExtractJsonString(line, "proof");

                std::string expected_proof = hmac_sha256_hex(
                    (const uint8_t*)pair_code_.data(), pair_code_.size(),
                    client_device_id_ + current_nonce_
                );

                if (!client_proof.empty() && constant_time_eq_str(client_proof, expected_proof)) {
                    std::string sess_hex;
                    {
                        std::lock_guard<std::mutex> lock(net_mutex_);
                        sequence_ = 0; // Reset sequence per session

                        std::random_device rd;
                        std::mt19937_64 gen(rd());
                        uint64_t sess_val = gen();
                        session_id_.assign((const uint8_t*)&sess_val, (const uint8_t*)&sess_val + 8);
                        sess_hex = ToHex(session_id_.data(), 8);

                        std::string salt = client_device_id_ + device_id_;
                        session_key_.resize(32);
                        hkdf_sha256_32(
                            (const uint8_t*)salt.data(), salt.size(),
                            (const uint8_t*)pair_code_.data(), pair_code_.size(),
                            (const uint8_t*)"audio-relay-session-v1", 22,
                            session_key_.data()
                        );
                        paired_keys_[client_device_id_] = session_key_;
                        SaveConfig();
                    }

                    std::ostringstream oss;
                    oss << "{\"type\":\"PAIR_OK\",\"session_id\":\"" << sess_hex << "\"}";
                    SendJson(client_sock, oss.str());
                    SendJson(client_sock, "{\"type\":\"CAPABILITIES\",\"sample_rate\":48000,\"channels\":2}");

                    TriggerStartCapture();
                    if (status_callback_) {
                        status_callback_("streaming", "Android 手机");
                    }
                } else {
                    SendJson(client_sock, "{\"type\":\"PAIR_FAIL\",\"reason\":\"invalid_code\"}");
                }
            } else if (type == "REPAIR") {
                // Protocol v2: proof = HMAC-SHA256(persisted_key, device_id || nonce_from_HELLO_ACK)
                std::string req_device_id = ExtractJsonString(line, "device_id");
                if (req_device_id.empty()) req_device_id = client_device_id_;
                std::string client_proof = ExtractJsonString(line, "proof");

                bool success = false;
                std::string sess_hex;

                {
                    std::lock_guard<std::mutex> lock(net_mutex_);
                    auto it = paired_keys_.find(req_device_id);
                    if (it != paired_keys_.end()) {
                        std::string expected_proof = hmac_sha256_hex(
                            it->second.data(), it->second.size(),
                            req_device_id + current_nonce_
                        );

                        if (!client_proof.empty() && constant_time_eq_str(client_proof, expected_proof)) {
                            success = true;
                            sequence_ = 0; // Reset sequence per session
                            session_key_ = it->second;

                            std::random_device rd;
                            std::mt19937_64 gen(rd());
                            uint64_t sess_val = gen();
                            session_id_.assign((const uint8_t*)&sess_val, (const uint8_t*)&sess_val + 8);
                            sess_hex = ToHex(session_id_.data(), 8);
                        }
                    }
                }

                if (success) {
                    std::ostringstream oss;
                    oss << "{\"type\":\"PAIR_OK\",\"session_id\":\"" << sess_hex << "\"}";
                    SendJson(client_sock, oss.str());
                    SendJson(client_sock, "{\"type\":\"CAPABILITIES\",\"sample_rate\":48000,\"channels\":2}");

                    TriggerStartCapture();
                    if (status_callback_) {
                        status_callback_("streaming", "Android 手机");
                    }
                } else {
                    SendJson(client_sock, "{\"type\":\"PAIR_FAIL\",\"reason\":\"key_expired_or_not_found\"}");
                }
            } else if (type == "PING") {
                int64_t t = ExtractJsonInt64(line, "t", 0);
                std::ostringstream oss;
                oss << "{\"type\":\"PONG\",\"t\":" << t << "}";
                SendJson(client_sock, oss.str());
            } else if (type == "BYE") {
                break;
            }
        }
    }

    closesocket(client_sock);
}

void WindowsAudioRelayServer::TcpAudioLoop() {
    while (is_running_.load()) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET sock = accept(tcp_audio_listen_sock_, (sockaddr*)&client_addr, &addr_len);
        if (sock == INVALID_SOCKET) {
            break;
        }

        // Set TCP_NODELAY for minimum latency
        BOOL nodelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        {
            std::lock_guard<std::mutex> lock(net_mutex_);
            if (active_audio_tcp_sock_ != INVALID_SOCKET) {
                closesocket(active_audio_tcp_sock_);
            }
            active_audio_tcp_sock_ = sock;
        }
    }
}

void WindowsAudioRelayServer::SendAudioFrame(const std::vector<uint8_t>& pcm) {
    std::lock_guard<std::mutex> lock(net_mutex_);

    if (session_key_.size() != 32 || session_id_.size() != 8) {
        return;
    }

    sequence_++;

    // Monotonic timestamp in milliseconds
    static const auto start_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
    uint32_t ts_ms = (uint32_t)(elapsed & 0xFFFFFFFF);

    // Protocol Header (13 bytes):
    // [0] = 0x00
    // [1..4] = sequence (big endian)
    // [5..8] = timestamp ms (big endian)
    // [9] = 0x01 (PCM_S16LE)
    // [10] = 0x02 (Stereo)
    // [11..12] = 0x0000 (48000Hz)
    uint8_t header[13];
    header[0] = 0x00;
    header[1] = (uint8_t)(sequence_ >> 24);
    header[2] = (uint8_t)(sequence_ >> 16);
    header[3] = (uint8_t)(sequence_ >> 8);
    header[4] = (uint8_t)(sequence_);

    header[5] = (uint8_t)(ts_ms >> 24);
    header[6] = (uint8_t)(ts_ms >> 16);
    header[7] = (uint8_t)(ts_ms >> 8);
    header[8] = (uint8_t)(ts_ms);

    header[9] = 0x01;
    header[10] = 0x02;
    header[11] = 0x00;
    header[12] = 0x00;

    // Nonce (12 bytes): session_id (8B) + sequence_be (4B)
    uint8_t nonce[12];
    std::memcpy(nonce, session_id_.data(), 8);
    nonce[8] = header[1];
    nonce[9] = header[2];
    nonce[10] = header[3];
    nonce[11] = header[4];

    // Encrypt with ChaCha20-Poly1305 (AAD = header 13B)
    std::vector<uint8_t> ciphertext(pcm.size());
    uint8_t tag[16];
    chacha20_poly1305_seal(
        session_key_.data(),
        nonce,
        header, 13,
        pcm.data(), pcm.size(),
        ciphertext.data(),
        tag
    );

    // Assemble Datagram: header (13B) + ciphertext + tag (16B)
    std::vector<uint8_t> datagram;
    datagram.reserve(13 + ciphertext.size() + 16);
    datagram.insert(datagram.end(), header, header + 13);
    datagram.insert(datagram.end(), ciphertext.begin(), ciphertext.end());
    datagram.insert(datagram.end(), tag, tag + 16);

    // 1. Send over UDP (Wi-Fi)
    if (has_active_udp_ && udp_sock_ != INVALID_SOCKET) {
        sendto(udp_sock_, (const char*)datagram.data(), (int)datagram.size(), 0,
               (sockaddr*)&active_udp_addr_, sizeof(active_udp_addr_));
    }

    // 2. Send over TCP (USB ADB cable) with 2-byte length prefix
    if (active_audio_tcp_sock_ != INVALID_SOCKET) {
        uint16_t len = (uint16_t)datagram.size();
        uint8_t len_prefix[2];
        len_prefix[0] = (uint8_t)(len >> 8);
        len_prefix[1] = (uint8_t)(len & 0xFF);

        int sent1 = send(active_audio_tcp_sock_, (const char*)len_prefix, 2, 0);
        int sent2 = send(active_audio_tcp_sock_, (const char*)datagram.data(), (int)datagram.size(), 0);
        if (sent1 <= 0 || sent2 <= 0) {
            closesocket(active_audio_tcp_sock_);
            active_audio_tcp_sock_ = INVALID_SOCKET;
        }
    }
}

void WindowsAudioRelayServer::AdbSupervisorLoop() {
    while (is_running_.load()) {
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};

        char cmd1[] = "adb reverse tcp:45108 tcp:45108";
        if (CreateProcessA(nullptr, cmd1, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 1000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        char cmd2[] = "adb reverse tcp:45109 tcp:45109";
        if (CreateProcessA(nullptr, cmd2, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 1000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

} // namespace audio_relay
