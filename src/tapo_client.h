#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <vector>

// Minimal local-LAN client for Tapo smart plugs (P100 / P105 / P110 / P115 ...).
// Speaks KLAP (current firmware) and falls back to the older securePassthrough protocol.
class TapoClient {
public:
    enum class Protocol { None, Klap, Passthrough };

    void begin(const char* ip, const char* email, const char* password);

    // Performs the handshake/login. Safe to call repeatedly; returns true when a session exists.
    bool connect();
    bool connected() const { return protocol_ != Protocol::None; }
    void disconnect();

    bool setPower(bool on);
    // 1 = on, 0 = off, -1 = could not read.
    int getPower();

    Protocol protocol() const { return protocol_; }
    const char* protocolName() const;
    const String& lastError() const { return lastError_; }

private:
    bool call(const char* method, const String& paramsJson, String& resultJson);
    bool rawRequest(const String& json, String& response);

    bool klapHandshake();
    bool klapRequest(const String& json, String& response);

    bool passthroughHandshake();
    bool passthroughLogin();
    bool passthroughRequest(const String& json, String& response);
    bool ensureRsaKey();

    bool post(const String& path, const uint8_t* body, size_t len, const char* contentType,
              bool captureCookie, int& status, std::vector<uint8_t>& out);
    void fail(const String& message);

    // Kept open across handshake and requests: the plug ties a KLAP session to
    // the TCP connection it was created on.
    WiFiClient conn_;
    String ip_, email_, password_;
    String cookie_, token_, lastError_;
    Protocol protocol_ = Protocol::None;

    uint8_t klapKey_[16] = {};
    uint8_t klapIvBase_[12] = {};
    uint8_t klapSig_[28] = {};
    int32_t klapSeq_ = 0;

    uint8_t ptKey_[16] = {};
    uint8_t ptIv_[16] = {};
    void* rsa_ = nullptr;  // mbedtls_pk_context*, generated once and reused
    String rsaPublicPem_;
};
