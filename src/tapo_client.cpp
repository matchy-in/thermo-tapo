#include "tapo_client.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <time.h>

namespace {

constexpr uint32_t kHttpTimeoutMs = 6000;

struct Bytes {
    const uint8_t* data;
    size_t len;
};

Bytes str(const char* s) { return {reinterpret_cast<const uint8_t*>(s), strlen(s)}; }
Bytes str(const String& s) { return {reinterpret_cast<const uint8_t*>(s.c_str()), s.length()}; }

void digest(mbedtls_md_type_t type, std::initializer_list<Bytes> parts, uint8_t* out) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(type), 0);
    mbedtls_md_starts(&ctx);
    for (const Bytes& p : parts) mbedtls_md_update(&ctx, p.data, p.len);
    mbedtls_md_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

void sha256(std::initializer_list<Bytes> parts, uint8_t out[32]) { digest(MBEDTLS_MD_SHA256, parts, out); }
void sha1(std::initializer_list<Bytes> parts, uint8_t out[20]) { digest(MBEDTLS_MD_SHA1, parts, out); }

String toHex(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    String s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s += hex[data[i] >> 4];
        s += hex[data[i] & 0x0f];
    }
    return s;
}

String sha1Hex(const String& s) {
    uint8_t h[20];
    sha1({str(s)}, h);
    return toHex(h, sizeof(h));
}

// KLAP v2 auth hash: sha256(sha1(username) + sha1(password))
void klapAuthHash(const String& user, const String& pass, uint8_t out[32]) {
    uint8_t u[20], p[20];
    sha1({str(user)}, u);
    sha1({str(pass)}, p);
    sha256({{u, 20}, {p, 20}}, out);
}

String base64Encode(const uint8_t* data, size_t len) {
    size_t outLen = 0;
    mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
    std::vector<uint8_t> buf(outLen + 1);
    mbedtls_base64_encode(buf.data(), buf.size(), &outLen, data, len);
    return String(reinterpret_cast<const char*>(buf.data()));
}

bool base64Decode(const String& in, std::vector<uint8_t>& out) {
    size_t outLen = 0;
    const auto* src = reinterpret_cast<const uint8_t*>(in.c_str());
    mbedtls_base64_decode(nullptr, 0, &outLen, src, in.length());
    out.resize(outLen);
    if (mbedtls_base64_decode(out.data(), out.size(), &outLen, src, in.length()) != 0) return false;
    out.resize(outLen);
    return true;
}

std::vector<uint8_t> aesEncrypt(const uint8_t key[16], const uint8_t ivIn[16], const uint8_t* data, size_t len) {
    size_t pad = 16 - (len % 16);
    std::vector<uint8_t> buf(data, data + len);
    buf.insert(buf.end(), pad, static_cast<uint8_t>(pad));
    std::vector<uint8_t> out(buf.size());
    uint8_t iv[16];
    memcpy(iv, ivIn, 16);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, buf.size(), iv, buf.data(), out.data());
    mbedtls_aes_free(&aes);
    return out;
}

bool aesDecrypt(const uint8_t key[16], const uint8_t ivIn[16], const uint8_t* data, size_t len, String& out) {
    if (len == 0 || len % 16 != 0) return false;
    std::vector<uint8_t> buf(len);
    uint8_t iv[16];
    memcpy(iv, ivIn, 16);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, len, iv, data, buf.data());
    mbedtls_aes_free(&aes);
    uint8_t pad = buf.back();
    if (pad == 0 || pad > 16) return false;
    out = String();
    out.reserve(len - pad);
    for (size_t i = 0; i < len - pad; i++) out += static_cast<char>(buf[i]);
    return true;
}

int fillRandom(void*, unsigned char* buf, size_t len) {
    esp_fill_random(buf, len);
    return 0;
}

// Renders a short, printable preview of an HTTP body for diagnostics.
String preview(const std::vector<uint8_t>& body) {
    String s;
    for (size_t i = 0; i < body.size() && i < 120; i++) {
        char c = static_cast<char>(body[i]);
        s += (c >= 32 && c < 127) ? c : '.';
    }
    return s;
}

bool before(uint32_t deadline) { return static_cast<int32_t>(deadline - millis()) > 0; }

bool readLine(WiFiClient& c, String& line, uint32_t deadline) {
    line = "";
    while (before(deadline)) {
        if (c.available()) {
            char ch = static_cast<char>(c.read());
            if (ch == '\n') {
                if (line.endsWith("\r")) line.remove(line.length() - 1);
                return true;
            }
            line += ch;
            if (line.length() > 2048) return false;
        } else if (!c.connected()) {
            return line.length() > 0;
        } else {
            delay(1);
        }
    }
    return false;
}

// Reads exactly n bytes, or until the connection closes when n is SIZE_MAX.
bool readBytes(WiFiClient& c, std::vector<uint8_t>& out, size_t n, uint32_t deadline) {
    uint8_t buf[256];
    size_t got = 0;
    while (got < n && before(deadline)) {
        int avail = c.available();
        if (avail > 0) {
            size_t want = std::min(static_cast<size_t>(avail), std::min(sizeof(buf), n - got));
            int r = c.read(buf, want);
            if (r > 0) {
                out.insert(out.end(), buf, buf + r);
                got += r;
            }
        } else if (!c.connected()) {
            return n == SIZE_MAX;
        } else {
            delay(1);
        }
    }
    return got == n || n == SIZE_MAX;
}

uint64_t nowMillis() {
    time_t t = time(nullptr);
    return t > 1600000000 ? static_cast<uint64_t>(t) * 1000ULL : 0;
}

String explainTapoError(int code) {
    switch (code) {
        case -1501: return "invalid Tapo email/password (-1501)";
        case -1010: return "invalid public key (-1010)";
        case -1012: return "invalid terminal UUID (-1012)";
        case 1002:  return "incorrect request (1002)";
        case 1003:  return "protocol not supported by this firmware (1003)";
        case 9999:  return "session timed out (9999)";
        default:    return "Tapo error " + String(code);
    }
}

}  // namespace

void TapoClient::begin(const char* ip, const char* email, const char* password) {
    ip_ = ip;
    email_ = email;
    password_ = password;
    disconnect();
}

void TapoClient::disconnect() {
    protocol_ = Protocol::None;
    cookie_ = "";
    token_ = "";
}

const char* TapoClient::protocolName() const {
    switch (protocol_) {
        case Protocol::Klap: return "KLAP";
        case Protocol::Passthrough: return "securePassthrough";
        default: return "none";
    }
}

void TapoClient::fail(const String& message) {
    lastError_ = message;
    Serial.println("[Tapo] " + message);
}

bool TapoClient::connect() {
    if (connected()) return true;
    if (WiFi.status() != WL_CONNECTED) {
        fail("Wi-Fi not connected");
        return false;
    }

    if (klapHandshake()) return true;
    String klapError = lastError_;

    Serial.println("[Tapo] KLAP failed, trying legacy securePassthrough protocol...");
    if (passthroughHandshake() && passthroughLogin()) return true;

    fail("Could not connect. KLAP: " + klapError + " | Legacy: " + lastError_);
    disconnect();
    return false;
}

bool TapoClient::post(const String& path, const uint8_t* body, size_t len, const char* contentType,
                      bool captureCookie, int& status, std::vector<uint8_t>& out) {
    // Hand-rolled HTTP: the plug sends TP_SESSIONID and TIMEOUT cookies, and only
    // TP_SESSIONID may be sent back. HTTPClient keeps just one Set-Cookie header.
    out.clear();
    status = 0;
    WiFiClient client;
    if (!client.connect(ip_.c_str(), 80, kHttpTimeoutMs)) {
        fail("cannot reach plug at " + ip_ + ":80");
        return false;
    }

    String head = "POST " + path + " HTTP/1.1\r\nHost: " + ip_ + "\r\nContent-Type: " + contentType +
                  "\r\nAccept: */*\r\nContent-Length: " + String(len) + "\r\nConnection: close\r\n";
    if (cookie_.length()) head += "Cookie: " + cookie_ + "\r\n";
    head += "\r\n";
    client.write(reinterpret_cast<const uint8_t*>(head.c_str()), head.length());
    if (len) client.write(body, len);

    uint32_t deadline = millis() + kHttpTimeoutMs;
    String line;
    if (!readLine(client, line, deadline) || !line.startsWith("HTTP/")) {
        fail("no HTTP response from plug for " + path);
        client.stop();
        return false;
    }
    status = line.substring(line.indexOf(' ') + 1).toInt();

    long contentLength = -1;
    bool chunked = false;
    while (readLine(client, line, deadline) && line.length()) {
        int colon = line.indexOf(':');
        if (colon < 0) continue;
        String name = line.substring(0, colon);
        name.trim();
        name.toLowerCase();
        String value = line.substring(colon + 1);
        value.trim();
        if (name == "content-length") {
            contentLength = value.toInt();
        } else if (name == "transfer-encoding") {
            value.toLowerCase();
            chunked = value.indexOf("chunked") >= 0;
        } else if (name == "set-cookie" && captureCookie) {
            int start = 0;
            while (start < static_cast<int>(value.length())) {
                int end = value.indexOf(';', start);
                if (end < 0) end = value.length();
                String part = value.substring(start, end);
                part.trim();
                if (part.startsWith("TP_SESSIONID=")) cookie_ = part;
                start = end + 1;
            }
        }
    }

    bool ok = true;
    if (chunked) {
        while (ok && readLine(client, line, deadline)) {
            size_t size = strtoul(line.c_str(), nullptr, 16);
            if (size == 0) break;
            ok = readBytes(client, out, size, deadline) && readLine(client, line, deadline);
        }
    } else if (contentLength > 0) {
        ok = readBytes(client, out, static_cast<size_t>(contentLength), deadline);
    } else if (contentLength < 0) {
        readBytes(client, out, SIZE_MAX, deadline);
    }
    client.stop();
    if (!ok) fail("timed out reading reply for " + path);
    return ok;
}

// ---------------------------------------------------------------- KLAP

bool TapoClient::klapHandshake() {
    cookie_ = "";
    uint8_t localSeed[16];
    esp_fill_random(localSeed, sizeof(localSeed));

    int status = 0;
    std::vector<uint8_t> resp;
    if (!post("/app/handshake1", localSeed, sizeof(localSeed), "application/octet-stream", true, status, resp))
        return false;

    if (status != 200 || resp.size() != 48) {
        String hint = status == 401 ? " -> plug refused local login: check Third-Party Compatibility in the "
                                      "Tapo app, power-cycle the plug, and confirm TAPO_IP is the plug"
                                    : "";
        fail("KLAP handshake1: HTTP " + String(status) + ", " + String(resp.size()) +
             " bytes (expected 48). Body: \"" + preview(resp) + "\"" + hint);
        return false;
    }

    if (cookie_.length() == 0) Serial.println("[Tapo] Warning: plug sent no TP_SESSIONID cookie");

    const uint8_t* remoteSeed = resp.data();
    const uint8_t* serverHash = resp.data() + 16;

    // The plug proves which credentials it holds; check ours before handshake2
    // so a credential problem is reported clearly instead of as a generic 403.
    uint8_t auth[32], candidate[32];
    klapAuthHash(email_, password_, auth);
    sha256({{localSeed, 16}, {remoteSeed, 16}, {auth, 32}}, candidate);
    if (memcmp(candidate, serverHash, 32) != 0) {
        uint8_t blank[32];
        klapAuthHash("", "", blank);
        sha256({{localSeed, 16}, {remoteSeed, 16}, {blank, 32}}, candidate);
        if (memcmp(candidate, serverHash, 32) == 0) {
            Serial.println("[Tapo] Plug is not bound to a Tapo account (blank credentials). Using those.");
            memcpy(auth, blank, 32);
        } else {
            fail("KLAP: plug rejected our credentials. Check TAPO_EMAIL/TAPO_PASSWORD exactly as in the "
                 "Tapo app (case-sensitive), and that the plug is owned by that account.");
            return false;
        }
    }

    uint8_t h2[32];
    sha256({{remoteSeed, 16}, {localSeed, 16}, {auth, 32}}, h2);
    if (!post("/app/handshake2", h2, sizeof(h2), "application/octet-stream", false, status, resp)) return false;
    if (status != 200) {
        fail("KLAP handshake2: HTTP " + String(status));
        return false;
    }

    uint8_t k[32];
    sha256({str("lsk"), {localSeed, 16}, {remoteSeed, 16}, {auth, 32}}, k);
    memcpy(klapKey_, k, 16);
    sha256({str("iv"), {localSeed, 16}, {remoteSeed, 16}, {auth, 32}}, k);
    memcpy(klapIvBase_, k, 12);
    klapSeq_ = static_cast<int32_t>((uint32_t(k[28]) << 24) | (uint32_t(k[29]) << 16) |
                                    (uint32_t(k[30]) << 8) | uint32_t(k[31]));
    sha256({str("ldk"), {localSeed, 16}, {remoteSeed, 16}, {auth, 32}}, k);
    memcpy(klapSig_, k, 28);

    protocol_ = Protocol::Klap;
    Serial.println("[Tapo] KLAP session established.");
    return true;
}

bool TapoClient::klapRequest(const String& json, String& response) {
    klapSeq_ = static_cast<int32_t>(static_cast<uint32_t>(klapSeq_) + 1);
    uint8_t seqBytes[4] = {
        uint8_t(uint32_t(klapSeq_) >> 24), uint8_t(uint32_t(klapSeq_) >> 16),
        uint8_t(uint32_t(klapSeq_) >> 8), uint8_t(uint32_t(klapSeq_))};
    uint8_t iv[16];
    memcpy(iv, klapIvBase_, 12);
    memcpy(iv + 12, seqBytes, 4);

    std::vector<uint8_t> cipher =
        aesEncrypt(klapKey_, iv, reinterpret_cast<const uint8_t*>(json.c_str()), json.length());
    uint8_t sig[32];
    sha256({{klapSig_, 28}, {seqBytes, 4}, {cipher.data(), cipher.size()}}, sig);

    std::vector<uint8_t> body(sig, sig + 32);
    body.insert(body.end(), cipher.begin(), cipher.end());

    int status = 0;
    std::vector<uint8_t> resp;
    if (!post("/app/request?seq=" + String(klapSeq_), body.data(), body.size(), "application/octet-stream",
              false, status, resp))
        return false;
    if (status != 200) {
        fail("KLAP request: HTTP " + String(status) + " (session expired?)");
        return false;
    }
    if (resp.size() < 48 || !aesDecrypt(klapKey_, iv, resp.data() + 32, resp.size() - 32, response)) {
        fail("KLAP request: could not decrypt response");
        return false;
    }
    return true;
}

// ------------------------------------------------------ securePassthrough

bool TapoClient::ensureRsaKey() {
    if (rsa_) return true;
    Serial.println("[Tapo] Generating RSA-1024 key (one-time, a few seconds)...");
    auto* pk = new mbedtls_pk_context;
    mbedtls_pk_init(pk);
    int rc = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (rc == 0) rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(*pk), fillRandom, nullptr, 1024, 65537);
    unsigned char pem[512];
    if (rc == 0) rc = mbedtls_pk_write_pubkey_pem(pk, pem, sizeof(pem));
    if (rc != 0) {
        mbedtls_pk_free(pk);
        delete pk;
        fail("RSA key generation failed: " + String(rc));
        return false;
    }
    rsaPublicPem_ = reinterpret_cast<const char*>(pem);
    rsa_ = pk;
    return true;
}

bool TapoClient::passthroughHandshake() {
    cookie_ = "";
    token_ = "";
    if (!ensureRsaKey()) return false;

    JsonDocument req;
    req["method"] = "handshake";
    req["params"]["key"] = rsaPublicPem_;
    req["requestTimeMils"] = nowMillis();
    String body;
    serializeJson(req, body);

    int status = 0;
    std::vector<uint8_t> resp;
    if (!post("/app", reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), "application/json", true,
              status, resp))
        return false;

    JsonDocument doc;
    if (status != 200 || deserializeJson(doc, resp.data(), resp.size())) {
        fail("legacy handshake: HTTP " + String(status) + ", body \"" + preview(resp) + "\"");
        return false;
    }
    int code = doc["error_code"] | -1;
    if (code != 0) {
        fail("legacy handshake: " + explainTapoError(code));
        return false;
    }

    std::vector<uint8_t> encrypted;
    if (!base64Decode(doc["result"]["key"].as<String>(), encrypted)) {
        fail("legacy handshake: bad key encoding");
        return false;
    }
    uint8_t plain[128];
    size_t plainLen = 0;
    int rc = mbedtls_pk_decrypt(static_cast<mbedtls_pk_context*>(rsa_), encrypted.data(), encrypted.size(), plain,
                                &plainLen, sizeof(plain), fillRandom, nullptr);
    if (rc != 0 || plainLen != 32) {
        fail("legacy handshake: RSA decrypt failed (" + String(rc) + ")");
        return false;
    }
    memcpy(ptKey_, plain, 16);
    memcpy(ptIv_, plain + 16, 16);
    return true;
}

bool TapoClient::passthroughLogin() {
    // Older firmware wants the plain password; newer legacy firmware wants login_version 2.
    for (int version = 1; version <= 2; version++) {
        JsonDocument req;
        req["method"] = "login_device";
        req["params"]["username"] = base64Encode(
            reinterpret_cast<const uint8_t*>(sha1Hex(email_).c_str()), 40);
        if (version == 1) {
            req["params"]["password"] =
                base64Encode(reinterpret_cast<const uint8_t*>(password_.c_str()), password_.length());
        } else {
            String ph = sha1Hex(password_);
            req["params"]["password2"] = base64Encode(reinterpret_cast<const uint8_t*>(ph.c_str()), 40);
            req["login_version"] = 2;
        }
        req["requestTimeMils"] = nowMillis();
        String json, response;
        serializeJson(req, json);
        if (!passthroughRequest(json, response)) return false;

        JsonDocument doc;
        if (deserializeJson(doc, response)) {
            fail("legacy login: bad response");
            return false;
        }
        int code = doc["error_code"] | -1;
        if (code == 0) {
            token_ = doc["result"]["token"].as<String>();
            protocol_ = Protocol::Passthrough;
            Serial.println("[Tapo] securePassthrough session established.");
            return true;
        }
        fail("legacy login v" + String(version) + ": " + explainTapoError(code));
    }
    return false;
}

bool TapoClient::passthroughRequest(const String& json, String& response) {
    std::vector<uint8_t> cipher =
        aesEncrypt(ptKey_, ptIv_, reinterpret_cast<const uint8_t*>(json.c_str()), json.length());

    JsonDocument req;
    req["method"] = "securePassthrough";
    req["params"]["request"] = base64Encode(cipher.data(), cipher.size());
    String body;
    serializeJson(req, body);

    String path = token_.length() ? "/app?token=" + token_ : "/app";
    int status = 0;
    std::vector<uint8_t> resp;
    if (!post(path, reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), "application/json", false,
              status, resp))
        return false;

    JsonDocument doc;
    if (status != 200 || deserializeJson(doc, resp.data(), resp.size())) {
        fail("legacy request: HTTP " + String(status));
        return false;
    }
    int code = doc["error_code"] | -1;
    if (code != 0) {
        fail("legacy request: " + explainTapoError(code));
        return false;
    }
    std::vector<uint8_t> inner;
    if (!base64Decode(doc["result"]["response"].as<String>(), inner) ||
        !aesDecrypt(ptKey_, ptIv_, inner.data(), inner.size(), response)) {
        fail("legacy request: could not decrypt response");
        return false;
    }
    return true;
}

// ------------------------------------------------------------- commands

bool TapoClient::rawRequest(const String& json, String& response) {
    return protocol_ == Protocol::Klap ? klapRequest(json, response) : passthroughRequest(json, response);
}

bool TapoClient::call(const char* method, const String& paramsJson, String& resultJson) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!connect()) return false;

        String json = String("{\"method\":\"") + method + "\"";
        if (paramsJson.length()) json += ",\"params\":" + paramsJson;
        json += ",\"requestTimeMils\":" + String(static_cast<unsigned long long>(nowMillis())) + "}";

        String response;
        if (!rawRequest(json, response)) {
            disconnect();  // session probably expired; re-handshake once
            continue;
        }
        JsonDocument doc;
        if (deserializeJson(doc, response)) {
            fail(String(method) + ": unparseable response");
            return false;
        }
        int code = doc["error_code"] | -1;
        if (code != 0) {
            fail(String(method) + ": " + explainTapoError(code));
            if (code == 9999) {
                disconnect();
                continue;
            }
            return false;
        }
        resultJson = "";
        serializeJson(doc["result"], resultJson);
        return true;
    }
    return false;
}

bool TapoClient::setPower(bool on) {
    String result;
    return call("set_device_info", on ? "{\"device_on\":true}" : "{\"device_on\":false}", result);
}

int TapoClient::getPower() {
    String result;
    if (!call("get_device_info", "", result)) return -1;
    JsonDocument doc;
    if (deserializeJson(doc, result) || !doc["device_on"].is<bool>()) return -1;
    return doc["device_on"].as<bool>() ? 1 : 0;
}
