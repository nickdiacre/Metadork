#include "out/UdpSender.h"
#include <cstring>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  static void closeSock(sock_t s) { closesocket(s); }
  static bool winsockReady() {
      static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
      return ok;
  }
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using sock_t = int;
  static void closeSock(sock_t s) { ::close(s); }
  static bool winsockReady() { return true; }
#endif

static_assert(sizeof(sockaddr_in) <= 16, "sockaddr_in must fit m_addr");

UdpSender::UdpSender() = default;
UdpSender::~UdpSender() { close(); }

bool UdpSender::open(const std::string& host, uint16_t port, std::string& err) {
    close();
    if (!winsockReady()) { err = "Winsock initialisation failed"; return false; }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        err = "Cannot resolve host '" + host + "'";
        return false;
    }
    sockaddr_in addr;
    std::memcpy(&addr, res->ai_addr, sizeof addr);
    freeaddrinfo(res);
    addr.sin_port = htons(port);

    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) { err = "socket() failed"; return false; }
#else
    if (s < 0) { err = "socket() failed"; return false; }
#endif
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);  // allow x.x.x.255

    std::memcpy(m_addr, &addr, sizeof addr);
    m_sock = static_cast<long long>(s);
    return true;
}

void UdpSender::close() {
    if (m_sock >= 0) closeSock(static_cast<sock_t>(m_sock));
    m_sock = -1;
}

bool UdpSender::send(const uint8_t* data, size_t size) {
    if (m_sock < 0) return false;
    const auto n = sendto(static_cast<sock_t>(m_sock), reinterpret_cast<const char*>(data), int(size), 0,
                          reinterpret_cast<const sockaddr*>(m_addr), sizeof(sockaddr_in));
    return n == static_cast<decltype(n)>(size);
}
