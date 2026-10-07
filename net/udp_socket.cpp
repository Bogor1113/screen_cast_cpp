// net/udp_socket.cpp
#include "net/udp_socket.h"
#include <ws2tcpip.h>
#include <cstring>

bool UDPSocket::initWinsock() {
    static bool inited = [] {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return inited;
}

bool UDPSocket::createSocket() {
    if (!initWinsock()) return false;
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    return sock_ != INVALID_SOCKET;
}

void UDPSocket::close() {
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
}

bool UDPSocket::setupSender(const std::string&, uint16_t, int ttl, const std::string& localIp) {
    if (!valid()) return false;
    int t = ttl;
    setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&t, sizeof(t));
    int loop = 1;   // allow local loopback, convenient for single-machine testing
    setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    // pin the multicast egress interface: without this, a multi-NIC machine (VPN/VM owning the
    // default route) may send the stream out the wrong adapter. Invalid/empty IP is ignored so the
    // OS default routing still applies as a fallback.
    if (!localIp.empty()) {
        IN_ADDR ifaddr{};
        if (inet_pton(AF_INET, localIp.c_str(), &ifaddr) == 1) {
            setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifaddr, sizeof(ifaddr));
        }
    }
    return true;
}

std::string UDPSocket::detectLocalIPv4() {
    if (!initWinsock()) return "";
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);                        // UDP port is irrelevant, no packet is sent
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);    // public addr -> OS picks the default-route NIC
    std::string out;
    if (connect(s, (const sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        if (getsockname(s, (sockaddr*)&local, &len) == 0) {
            char ip[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip))) {
                std::string a = ip;
                if (!a.empty() && a != "0.0.0.0" && a != "127.0.0.1") out = a;
            }
        }
    }
    closesocket(s);
    return out;
}

int UDPSocket::sendTo(const std::string& group, uint16_t port, const void* data, int len) {
    if (!valid()) return -1;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, group.c_str(), &dst.sin_addr) != 1) return -1;   // invalid IP: fail instead of sending to 0.0.0.0
    return (int)sendto(sock_, (const char*)data, len, 0, (const sockaddr*)&dst, sizeof(dst));
}

bool UDPSocket::bindAndJoinGroup(const std::string& group, uint16_t port, const std::string& localIp) {
    if (!valid()) return false;
    BOOL reuse = TRUE;  // on Windows SO_REUSEADDR lets multiple processes bind the same port
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(sock_, (const sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) return false;
    ip_mreq mreq{};
    if (inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) return false;   // invalid group address
    // pin membership to a single NIC when one is supplied (avoids duplicate delivery on multi-NIC
    // / single-machine loopback); an invalid/empty IP falls back to the default interface.
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (!localIp.empty())
        inet_pton(AF_INET, localIp.c_str(), &mreq.imr_interface);
    return setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char*)&mreq, sizeof(mreq)) == 0;
}

bool UDPSocket::bindUnicast(uint16_t port) {
    if (!valid()) return false;
    BOOL reuse = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    return bind(sock_, (const sockaddr*)&local, sizeof(local)) == 0;
}

int UDPSocket::recvFrom(void* buf, int len, std::string* srcIP, uint16_t* srcPort) {
    sockaddr_in from{};
    int flen = sizeof(from);
    int n = (int)recvfrom(sock_, (char*)buf, len, 0, (sockaddr*)&from, &flen);
    if (n > 0 && srcIP) {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        *srcIP = ip;
    }
    if (n > 0 && srcPort) *srcPort = ntohs(from.sin_port);
    return n;
}

void UDPSocket::setRecvBuffer(int bytes) {
    if (valid()) setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof(bytes));
}

void UDPSocket::setSendBuffer(int bytes) {
    if (valid()) setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof(bytes));
}

void UDPSocket::setRecvTimeout(int ms) {
    if (valid()) {
        DWORD t = (DWORD)ms;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    }
}
