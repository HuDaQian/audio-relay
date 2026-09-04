#include "audio_relay_server.h"
#include "crypto.h"
#include <chrono>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace audio_relay {

namespace {

static void GetSecureRandomBytes(uint8_t* buf, size_t len) {
    BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

static std::string GenerateSixDigitCodeSecure() {
    uint32_t val = 0;
    do {
        GetSecureRandomBytes((uint8_t*)&val, sizeof(val));
    } while (val >= 4294000000ULL);
    uint32_t code_num = val % 1000000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%06u", code_num);
    return std::string(buf);
}

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
        uint8_t r[16];
        GetSecureRandomBytes(r, sizeof(r));
        device_id_ = "win-" + ToHex(r, sizeof(r));
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

void WindowsAudioRelayServer::GenerateNewPairCodeLocked() {
    pair_code_ = GenerateSixDigitCodeSecure();
    pair_code_created_at_ = std::chrono::steady_clock::now();
}

void WindowsAudioRelayServer::GenerateNewPairCode() {
    std::lock_guard<std::mutex> lock(pair_code_mutex_);
    GenerateNewPairCodeLocked();
}

std::string WindowsAudioRelayServer::GetPairCode() {
    std::lock_guard<std::mutex> lock(pair_code_mutex_);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - pair_code_created_at_).count();
    if (elapsed > 300 || pair_code_.empty()) {
        GenerateNewPairCodeLocked();
    }
    return pair_code_;
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
    mdns_thread_ = std::thread(&WindowsAudioRelayServer::MdnsLoop, this);

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
    if (mdns_sock_ != INVALID_SOCKET) {
        closesocket(mdns_sock_);
        mdns_sock_ = INVALID_SOCKET;
    }

    if (tcp_control_thread_.joinable()) tcp_control_thread_.join();
    if (tcp_audio_thread_.joinable()) tcp_audio_thread_.join();
    if (adb_thread_.joinable()) adb_thread_.join();
    if (mdns_thread_.joinable()) mdns_thread_.join();

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

    std::string local_client_device_id;
    std::string local_current_nonce;

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
                local_client_device_id = ExtractJsonString(line, "device_id");

                uint8_t nonce_bytes[8];
                GetSecureRandomBytes(nonce_bytes, sizeof(nonce_bytes));
                local_current_nonce = ToHex(nonce_bytes, sizeof(nonce_bytes));

                bool is_paired = false;
                {
                    std::lock_guard<std::mutex> lock(net_mutex_);
                    is_paired = (paired_keys_.find(local_client_device_id) != paired_keys_.end());
                }

                std::ostringstream oss;
                oss << "{\"type\":\"HELLO_ACK\","
                    << "\"protocol_version\":2,"
                    << "\"device_id\":\"" << device_id_ << "\","
                    << "\"device_name\":\"" << device_name_ << "\","
                    << "\"paired\":" << (is_paired ? "true" : "false") << ","
                    << "\"nonce\":\"" << local_current_nonce << "\"}";
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
                std::string current_pair_code;
                bool is_expired = false;
                {
                    std::lock_guard<std::mutex> lock(pair_code_mutex_);
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - pair_code_created_at_).count();
                    if (elapsed > 300 || pair_code_.empty()) {
                        GenerateNewPairCodeLocked();
                        is_expired = true;
                    } else {
                        current_pair_code = pair_code_;
                    }
                }

                if (is_expired) {
                    SendJson(client_sock, "{\"type\":\"PAIR_FAIL\",\"reason\":\"code_expired\"}");
                    continue;
                }

                // Protocol v2: proof = HMAC-SHA256(code, phone_device_id || nonce_from_HELLO_ACK)
                std::string client_proof = ExtractJsonString(line, "proof");

                std::string expected_proof = hmac_sha256_hex(
                    (const uint8_t*)current_pair_code.data(), current_pair_code.size(),
                    local_client_device_id + local_current_nonce
                );

                if (!client_proof.empty() && constant_time_eq_str(client_proof, expected_proof)) {
                    std::string sess_hex;
                    {
                        std::lock_guard<std::mutex> lock(net_mutex_);
                        sequence_ = 0; // Reset sequence per session

                        session_id_.resize(8);
                        GetSecureRandomBytes(session_id_.data(), 8);
                        sess_hex = ToHex(session_id_.data(), 8);

                        std::string salt = local_client_device_id + device_id_;
                        session_key_.resize(32);
                        hkdf_sha256_32(
                            (const uint8_t*)salt.data(), salt.size(),
                            (const uint8_t*)current_pair_code.data(), current_pair_code.size(),
                            (const uint8_t*)"audio-relay-session-v1", 22,
                            session_key_.data()
                        );
                        paired_keys_[local_client_device_id] = session_key_;
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
                if (req_device_id.empty()) req_device_id = local_client_device_id;
                std::string client_proof = ExtractJsonString(line, "proof");

                bool success = false;
                std::string sess_hex;

                {
                    std::lock_guard<std::mutex> lock(net_mutex_);
                    auto it = paired_keys_.find(req_device_id);
                    if (it != paired_keys_.end()) {
                        std::string expected_proof = hmac_sha256_hex(
                            it->second.data(), it->second.size(),
                            req_device_id + local_current_nonce
                        );

                        if (!client_proof.empty() && constant_time_eq_str(client_proof, expected_proof)) {
                            success = true;
                            sequence_ = 0; // Reset sequence per session
                            session_key_ = it->second;

                            session_id_.resize(8);
                            GetSecureRandomBytes(session_id_.data(), 8);
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

    {
        std::lock_guard<std::mutex> lock(net_mutex_);
        has_active_udp_ = false;
        session_key_.clear();
        session_id_.clear();
    }

    closesocket(client_sock);

    if (status_callback_) {
        status_callback_("listening", "");
    }
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
    // [0] = 0x00 (codec_id: 0x00 = raw PCM_S16LE)
    // [1..4] = sequence (big endian)
    // [5..8] = timestamp ms (big endian)
    // [9] = 0x01 (sample_rate_id: 0 = 44100Hz, 1 = 48000Hz)
    // [10] = 0x02 (channels: 1 = mono, 2 = stereo)
    // [11..12] = 0x0000 (reserved)
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

// Lightweight DNS-SD / mDNS service announcement responder
// Advertises _audiorelay._udp.local. on 224.0.0.251:5353
void WindowsAudioRelayServer::MdnsLoop() {
    mdns_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mdns_sock_ == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(mdns_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(5353);

    if (bind(mdns_sock_, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        closesocket(mdns_sock_);
        mdns_sock_ = INVALID_SOCKET;
        return;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(mdns_sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

    sockaddr_in mdns_dest{};
    mdns_dest.sin_family = AF_INET;
    mdns_dest.sin_addr.s_addr = inet_addr("224.0.0.251");
    mdns_dest.sin_port = htons(5353);

    auto send_dns_sd_announcement = [this, &mdns_dest]() {
        if (mdns_sock_ == INVALID_SOCKET) return;

        std::vector<uint8_t> packet;
        // 1. DNS Header: ID=0, Flags=0x8400 (Authoritative response), QDCOUNT=0, ANCOUNT=4, NSCOUNT=0, ARCOUNT=0
        uint8_t header[] = {
            0x00, 0x00, 0x84, 0x00,
            0x00, 0x00, // QDCOUNT = 0
            0x00, 0x04, // ANCOUNT = 4 (PTR, SRV, TXT, A)
            0x00, 0x00, // NSCOUNT = 0
            0x00, 0x00  // ARCOUNT = 0
        };
        packet.insert(packet.end(), header, header + sizeof(header));

        auto append_domain = [&packet](const std::string& domain) {
            size_t start = 0;
            while (start < domain.size()) {
                size_t dot = domain.find('.', start);
                if (dot == std::string::npos) dot = domain.size();
                size_t len = dot - start;
                packet.push_back((uint8_t)len);
                packet.insert(packet.end(), domain.begin() + start, domain.begin() + dot);
                start = dot + 1;
            }
            packet.push_back(0x00);
        };

        std::string service_type = "_audiorelay._udp.local";
        std::string instance_name = device_name_ + "." + service_type;
        std::string target_host = device_name_ + ".local";

        // Record 1: PTR Record: _audiorelay._udp.local -> <instance_name>
        append_domain(service_type);
        // Type = PTR (0x000c), Class = IN (0x0001) | Cache-Flush (0x8000)
        uint8_t ptr_meta[] = { 0x00, 0x0C, 0x80, 0x01, 0x00, 0x00, 0x00, 0x78 }; // TTL = 120s
        packet.insert(packet.end(), ptr_meta, ptr_meta + sizeof(ptr_meta));

        std::vector<uint8_t> rdata_ptr;
        {
            size_t start = 0;
            while (start < instance_name.size()) {
                size_t dot = instance_name.find('.', start);
                if (dot == std::string::npos) dot = instance_name.size();
                size_t len = dot - start;
                rdata_ptr.push_back((uint8_t)len);
                rdata_ptr.insert(rdata_ptr.end(), instance_name.begin() + start, instance_name.begin() + dot);
                start = dot + 1;
            }
            rdata_ptr.push_back(0x00);
        }
        uint16_t ptr_rdlen = (uint16_t)rdata_ptr.size();
        packet.push_back((uint8_t)(ptr_rdlen >> 8));
        packet.push_back((uint8_t)(ptr_rdlen & 0xFF));
        packet.insert(packet.end(), rdata_ptr.begin(), rdata_ptr.end());

        // Record 2: SRV Record: <instance_name> -> port 45108, target: <target_host>
        append_domain(instance_name);
        uint8_t srv_meta[] = { 0x00, 0x21, 0x80, 0x01, 0x00, 0x00, 0x00, 0x78 }; // TTL = 120s
        packet.insert(packet.end(), srv_meta, srv_meta + sizeof(srv_meta));

        std::vector<uint8_t> rdata_srv;
        rdata_srv.push_back(0x00); rdata_srv.push_back(0x00); // Priority = 0
        rdata_srv.push_back(0x00); rdata_srv.push_back(0x00); // Weight = 0
        rdata_srv.push_back((uint8_t)(45108 >> 8));
        rdata_srv.push_back((uint8_t)(45108 & 0xFF)); // Port = 45108
        {
            size_t start = 0;
            while (start < target_host.size()) {
                size_t dot = target_host.find('.', start);
                if (dot == std::string::npos) dot = target_host.size();
                size_t len = dot - start;
                rdata_srv.push_back((uint8_t)len);
                rdata_srv.insert(rdata_srv.end(), target_host.begin() + start, target_host.begin() + dot);
                start = dot + 1;
            }
            rdata_srv.push_back(0x00);
        }
        uint16_t srv_rdlen = (uint16_t)rdata_srv.size();
        packet.push_back((uint8_t)(srv_rdlen >> 8));
        packet.push_back((uint8_t)(srv_rdlen & 0xFF));
        packet.insert(packet.end(), rdata_srv.begin(), rdata_srv.end());

        // Record 3: TXT Record: id=<device_id>, name=<device_name>, protocol_version=2
        append_domain(instance_name);
        uint8_t txt_meta[] = { 0x00, 0x10, 0x80, 0x01, 0x00, 0x00, 0x00, 0x78 }; // TTL = 120s
        packet.insert(packet.end(), txt_meta, txt_meta + sizeof(txt_meta));

        std::vector<std::string> txt_entries = {
            "id=" + device_id_,
            "name=" + device_name_,
            "protocol_version=2"
        };
        std::vector<uint8_t> rdata_txt;
        for (const auto& entry : txt_entries) {
            rdata_txt.push_back((uint8_t)entry.size());
            rdata_txt.insert(rdata_txt.end(), entry.begin(), entry.end());
        }
        uint16_t txt_rdlen = (uint16_t)rdata_txt.size();
        packet.push_back((uint8_t)(txt_rdlen >> 8));
        packet.push_back((uint8_t)(txt_rdlen & 0xFF));
        packet.insert(packet.end(), rdata_txt.begin(), rdata_txt.end());

        // Record 4: A Record for target_host
        append_domain(target_host);
        uint8_t a_meta[] = { 0x00, 0x01, 0x80, 0x01, 0x00, 0x00, 0x00, 0x78, 0x00, 0x04 };
        packet.insert(packet.end(), a_meta, a_meta + sizeof(a_meta));

        // Get local LAN IP (skip loopback and prefer private subnets 192.168.x.x, 10.x.x.x, 172.16-31.x.x)
        uint32_t local_ip = INADDR_ANY;
        char host_name[256];
        if (gethostname(host_name, sizeof(host_name)) == 0) {
            hostent* he = gethostbyname(host_name);
            if (he && he->h_addr_list) {
                uint32_t candidate = INADDR_ANY;
                for (int i = 0; he->h_addr_list[i] != nullptr; ++i) {
                    uint32_t ip = *(uint32_t*)he->h_addr_list[i];
                    uint8_t b0 = (uint8_t)(ip & 0xFF);
                    uint8_t b1 = (uint8_t)((ip >> 8) & 0xFF);
                    if (b0 == 127) {
                        continue; // Skip loopback
                    }
                    if (b0 == 192 && b1 == 168) {
                        candidate = ip;
                        break; // Highest preference: common home LAN
                    }
                    if (b0 == 10) {
                        candidate = ip;
                    } else if (b0 == 172 && (b1 >= 16 && b1 <= 31) && candidate == INADDR_ANY) {
                        candidate = ip;
                    } else if (candidate == INADDR_ANY) {
                        candidate = ip;
                    }
                }
                if (candidate != INADDR_ANY) {
                    local_ip = candidate;
                } else if (he->h_addr_list[0]) {
                    local_ip = *(uint32_t*)he->h_addr_list[0];
                }
            }
        }
        packet.push_back((uint8_t)(local_ip & 0xFF));
        packet.push_back((uint8_t)((local_ip >> 8) & 0xFF));
        packet.push_back((uint8_t)((local_ip >> 16) & 0xFF));
        packet.push_back((uint8_t)((local_ip >> 24) & 0xFF));

        sendto(mdns_sock_, (const char*)packet.data(), (int)packet.size(), 0,
               (sockaddr*)&mdns_dest, sizeof(mdns_dest));
    };

    // Broadcast immediately on start, and listen for incoming queries
    send_dns_sd_announcement();

    u_long non_blocking = 1;
    ioctlsocket(mdns_sock_, FIONBIO, &non_blocking);

    char recv_buf[1500];
    auto last_announce = std::chrono::steady_clock::now();

    while (is_running_.load()) {
        sockaddr_in src_addr{};
        int src_len = sizeof(src_addr);
        int r = recvfrom(mdns_sock_, recv_buf, sizeof(recv_buf), 0, (sockaddr*)&src_addr, &src_len);
        if (r > 0) {
            // Check if query contains _audiorelay
            std::string q(recv_buf, r);
            if (q.find("_audiorelay") != std::string::npos) {
                send_dns_sd_announcement();
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_announce).count() >= 5) {
            send_dns_sd_announcement();
            last_announce = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

} // namespace audio_relay
