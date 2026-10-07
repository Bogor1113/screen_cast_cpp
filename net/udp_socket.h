#pragma once
// net/udp_socket.h -- minimal UDP wrapper (shared by unicast/multicast)
#include <winsock2.h>
#include <cstdint>
#include <string>

class UDPSocket {
public:
    UDPSocket() = default;
    ~UDPSocket() { close(); }

    static bool initWinsock();              // WSAStartup, done once per process
    bool createSocket();                    // socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
    void close();

    // ===== Sender side (teacher) =====
    // group: multicast group addr ("239.255.1.1") or target unicast IP; ttl=1 means local subnet only
    // localIp: (optional) the local interface IPv4 to bind the multicast egress to. On multi-NIC
    // teacher machines (VPN/VM adapters owning the default route), the OS would otherwise send
    // multicast out the wrong adapter and every client gets nothing.
    bool setupSender(const std::string& group, uint16_t port, int ttl = 1, const std::string& localIp = "");
    int  sendTo(const std::string& group, uint16_t port, const void* data, int len);

    // best-effort detection of the machine's outbound IPv4 (connects a UDP socket to a public
    // address WITHOUT sending any packet, then reads the local address the OS picked). Returns "" on
    // failure. Used as the multicast egress interface when the caller didn't supply one explicitly.
    static std::string detectLocalIPv4();

    // ===== Receiver side (client) =====
    // bind + join the multicast group. localIp: (optional) the local interface IPv4 to receive the
    // group on. Empty (default) = INADDR_ANY, i.e. join on EVERY interface -- on a multi-NIC machine
    // (or single-machine loopback where the stream arrives via both the NIC and the loopback) the
    // client then receives each packet twice. Pin to the outbound NIC to receive exactly one copy.
    bool bindAndJoinGroup(const std::string& group, uint16_t port, const std::string& localIp = "");
    bool bindUnicast(uint16_t port);                                // just bind the port
    int  recvFrom(void* buf, int len, std::string* srcIP = nullptr, uint16_t* srcPort = nullptr);

    void setRecvBuffer(int bytes);
    void setSendBuffer(int bytes);   // send buffer: keyframe bursts (hundreds of fragments) should not block the capture thread
    void setRecvTimeout(int ms);   // receive timeout (ms), 0 = block forever
    SOCKET raw() const { return sock_; }
    bool   valid() const { return sock_ != INVALID_SOCKET; }

private:
    SOCKET sock_ = INVALID_SOCKET;
};
