#pragma once
// POSIX-socket stand-in for the ESP32 WiFiClient. The plug port can be redirected
// with the TAPO_TEST_PORT environment variable so tests can use a mock plug.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

#include "Arduino.h"

enum { WL_CONNECTED = 3 };

struct HostWiFi {
    int status() { return WL_CONNECTED; }
};
extern HostWiFi WiFi;

class WiFiClient {
public:
    ~WiFiClient() { stop(); }

    int connect(const char* host, uint16_t port, int32_t timeoutMs) {
        if (const char* p = getenv("TAPO_TEST_PORT")) port = static_cast<uint16_t>(atoi(p));
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            stop();
            return 0;
        }
        open_ = true;
        return 1;
    }

    void setNoDelay(bool) {}

    size_t write(const uint8_t* data, size_t len) {
        return fd_ >= 0 ? static_cast<size_t>(::send(fd_, data, len, MSG_NOSIGNAL)) : 0;
    }

    int available() {
        fill();
        return static_cast<int>(buf_.size() - pos_);
    }
    int read() {
        if (available() <= 0) return -1;
        return buf_[pos_++];
    }
    int read(uint8_t* out, size_t n) {
        int avail = available();
        if (avail <= 0) return -1;
        size_t take = std::min(n, static_cast<size_t>(avail));
        memcpy(out, buf_.data() + pos_, take);
        pos_ += take;
        return static_cast<int>(take);
    }
    bool connected() {
        fill();
        return open_ || pos_ < buf_.size();
    }
    void stop() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        open_ = false;
    }

private:
    void fill() {
        if (!open_ || pos_ < buf_.size()) return;
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, 0) <= 0) return;
        uint8_t tmp[1024];
        ssize_t r = recv(fd_, tmp, sizeof(tmp), 0);
        if (r <= 0) {
            open_ = false;
            return;
        }
        buf_.assign(tmp, tmp + r);
        pos_ = 0;
    }

    int fd_ = -1;
    bool open_ = false;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
};
