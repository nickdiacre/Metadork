#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Minimal IPv4 UDP sender (unicast or broadcast). Not thread-safe; OutputManager serialises access.
class UdpSender {
public:
    UdpSender();
    ~UdpSender();
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool open(const std::string& host, uint16_t port, std::string& err);
    void close();
    bool isOpen() const { return m_sock >= 0; }
    bool send(const uint8_t* data, size_t size);
    const std::string& lastError() const { return m_lastError; }  // reason of the most recent failed send

private:
    long long m_sock = -1;  // SOCKET on Windows, int elsewhere
    unsigned char m_addr[16] = {};  // sockaddr_in
    std::string m_lastError;
};
