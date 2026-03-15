#include "mqtt_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "MQTT"

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();
    mbedtls_aes_init(&aes_ctx_);

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            MqttProtocol* protocol = (MqttProtocol*)arg;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                ESP_LOGI(TAG, "Reconnecting to MQTT server");
                auto alive = protocol->alive_;  // Capture alive flag
                app.Schedule([protocol, alive]() {
                    if (*alive) {
                        protocol->StartMqttClient(false);
                    }
                });
            }
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    
    // Mark as dead first to prevent any pending scheduled tasks from executing
    *alive_ = false;
    
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }
    mbedtls_aes_free(&aes_ctx_);

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            const char* remote_session_id = cJSON_IsString(session_id) ? session_id->valuestring : "null";
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", remote_session_id);
            if (session_id == nullptr || (cJSON_IsString(session_id) && session_id_ == session_id->valuestring)) {
                auto alive = alive_;  // Capture alive flag
                Application::GetInstance().Schedule([this, alive]() {
                    if (*alive) {
                        CloseAudioChannel();
                    }
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address = endpoint;
    int broker_port = 8883;
    std::string endpoint_host = endpoint;
    size_t scheme_pos = endpoint_host.find("://");
    if (scheme_pos != std::string::npos) {
        endpoint_host = endpoint_host.substr(scheme_pos + 3);
    }
    size_t colon_pos = endpoint_host.rfind(':');
    if (colon_pos != std::string::npos) {
        std::string host_part = endpoint_host.substr(0, colon_pos);
        const auto port_text = endpoint_host.substr(colon_pos + 1);
        char* end_ptr = nullptr;
        long parsed_port = std::strtol(port_text.c_str(), &end_ptr, 10);
        if (end_ptr != port_text.c_str() && end_ptr != nullptr && *end_ptr == '\0' &&
            parsed_port > 0 && parsed_port <= 65535) {
            broker_address = host_part;
            broker_port = static_cast<int>(parsed_port);
        } else {
            ESP_LOGW(TAG, "Invalid MQTT endpoint port: %s, fallback to %d", port_text.c_str(), broker_port);
            broker_address = endpoint_host;
        }
    } else {
        broker_address = endpoint_host;
    }
    ESP_LOGI(TAG, "MQTT connect target: host=%s, port=%d", broker_address.c_str(), broker_port);
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint, code=%d", mqtt_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return false;
    }
    if (aes_nonce_.size() < 16) {
        ESP_LOGE(TAG, "Invalid AES nonce length: %u", static_cast<unsigned>(aes_nonce_.size()));
        return false;
    }
    if (packet->payload.size() > UINT16_MAX) {
        ESP_LOGE(TAG, "Audio payload too large: %u", static_cast<unsigned>(packet->payload.size()));
        return false;
    }

    std::string nonce(aes_nonce_.data(), 16);
    uint16_t net_payload_size = htons(static_cast<uint16_t>(packet->payload.size()));
    uint32_t net_timestamp = htonl(packet->timestamp);
    uint32_t net_sequence = htonl(++local_sequence_);
    memcpy(&nonce[2], &net_payload_size, sizeof(net_payload_size));
    memcpy(&nonce[8], &net_timestamp, sizeof(net_timestamp));
    memcpy(&nonce[12], &net_sequence, sizeof(net_sequence));

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet->payload.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)packet->payload.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }

    return udp_->Send(encrypted) > 0;
}

void MqttProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }

    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_ = network->CreateUdp(2);
    // Simple jitter buffer to reorder small gaps
    static constexpr size_t kJitterBufferSize = 5;

    udp_->OnMessage([this](const std::string& data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        if (data.size() < aes_nonce_.size()) {
            ESP_LOGE(TAG, "Invalid audio packet size: %u", static_cast<unsigned>(data.size()));
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        uint32_t net_timestamp = 0;
        uint32_t net_sequence = 0;
        memcpy(&net_timestamp, data.data() + 8, sizeof(net_timestamp));
        memcpy(&net_sequence, data.data() + 12, sizeof(net_sequence));
        uint32_t timestamp = ntohl(net_timestamp);
        uint32_t sequence = ntohl(net_sequence);
        if (sequence < remote_sequence_) {
            // drop old packet silently
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            uint32_t gap = (sequence > remote_sequence_) ? sequence - (remote_sequence_ + 1) : 0;
            if (gap > 5) {
                ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu", sequence, remote_sequence_ + 1);
            }
        }

        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)packet->payload.data());
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(jitter_mutex_);
            // Jitter buffer: push, then emit in order while contiguous
            jitter_buffer_.push_back(BufferedPacket{sequence, std::move(packet)});
            // sort by sequence
            std::sort(jitter_buffer_.begin(), jitter_buffer_.end(),
                      [](const BufferedPacket& a, const BufferedPacket& b) { return a.seq < b.seq; });

            while (!jitter_buffer_.empty()) {
                auto& front = jitter_buffer_.front();
                if (remote_sequence_ == 0 || front.seq == remote_sequence_ + 1) {
                    if (on_incoming_audio_ != nullptr) {
                        on_incoming_audio_(std::move(front.pkt));
                    }
                    remote_sequence_ = front.seq;
                    jitter_buffer_.pop_front();
                } else {
                    break;
                }
            }

            // Cap buffer size: flush oldest
            while (jitter_buffer_.size() > kJitterBufferSize) {
                auto front = std::move(jitter_buffer_.front());
                jitter_buffer_.pop_front();
                if (on_incoming_audio_ != nullptr) {
                    on_incoming_audio_(std::move(front.pkt));
                }
                remote_sequence_ = front.seq;
            }
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    udp_->Connect(udp_server_, udp_port_);

    // Mark channel as fresh so IsAudioChannelOpened() does not instantly fail
    // before the first downstream audio packet arrives.
    last_incoming_time_ = std::chrono::steady_clock::now();

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || transport->valuestring == nullptr ||
        strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport in MQTT hello");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    auto server = cJSON_GetObjectItem(udp, "server");
    auto port = cJSON_GetObjectItem(udp, "port");
    auto key = cJSON_GetObjectItem(udp, "key");
    auto nonce = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(server) || server->valuestring == nullptr ||
        !cJSON_IsNumber(port) ||
        !cJSON_IsString(key) || key->valuestring == nullptr ||
        !cJSON_IsString(nonce) || nonce->valuestring == nullptr) {
        ESP_LOGE(TAG, "Invalid UDP config in MQTT hello");
        return;
    }
    udp_server_ = server->valuestring;
    udp_port_ = port->valueint;
    if (udp_port_ <= 0 || udp_port_ > 65535) {
        ESP_LOGE(TAG, "Invalid UDP port in MQTT hello: %d", udp_port_);
        return;
    }

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    auto decoded_nonce = DecodeHexString(nonce->valuestring);
    auto decoded_key = DecodeHexString(key->valuestring);
    if (decoded_nonce.size() < 16 || decoded_key.size() != 16) {
        ESP_LOGE(TAG, "Invalid UDP key/nonce size in MQTT hello");
        return;
    }
    aes_nonce_.assign(decoded_nonce.data(), 16);
    if (mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)decoded_key.data(), 128) != 0) {
        ESP_LOGE(TAG, "Failed to set AES key from MQTT hello");
        return;
    }

    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    if ((hex_string.size() % 2) != 0) {
        ESP_LOGW(TAG, "Invalid hex string length: %u", static_cast<unsigned>(hex_string.size()));
        return {};
    }

    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
