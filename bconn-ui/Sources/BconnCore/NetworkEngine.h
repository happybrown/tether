#pragma once
#include "protocol.h"
#include "BconnLog.h"
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <cstring>

// ── 소켓 유틸 ─────────────────────────────────────────────────────────

static inline int make_udp_sock() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    // 송신 버퍼 최소화: 오래된 패킷이 쌓이지 않도록
    // 수신 버퍼는 패킷 유실 방지를 위해 충분히 유지
    int sndbuf = 4096;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // DSCP EF(Expedited Forwarding) — 라우터에서 최우선 처리
    int tos = IPTOS_LOWDELAY;
    setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    return s;
}

// ── NetworkEngine ─────────────────────────────────────────────────────
//
// 연결 구조:
//   host  : bind(port)       → 수신  /  sendto(client_addr) → 송신
//   client: connect(host_ip, port) → 송수신 모두 동일 소켓
//
// host 모드: is_server=true 로 생성, recv 콜백만 등록
// client 모드: is_server=false, host ip/port 전달

class NetworkEngine {
public:
    using MouseRecvCallback   = std::function<void(const MousePacket&)>;
    using KeyRecvCallback     = std::function<void(const KeyPacket&)>;
    using HelloRecvCallback   = std::function<void(const HelloPacket&, const sockaddr_in&)>;
    using AuthAckRecvCallback = std::function<void(const AuthAckPacket&)>;

    // host 모드: ip=nullptr, port=listen port
    // client 모드: ip=host ip, port=host port
    NetworkEngine(const char* ip, int port,
                  MouseRecvCallback   mouse_cb    = nullptr,
                  KeyRecvCallback     key_cb      = nullptr,
                  HelloRecvCallback   hello_cb    = nullptr,
                  AuthAckRecvCallback auth_ack_cb = nullptr)
        : on_mouse(mouse_cb), on_key(key_cb),
          on_hello(hello_cb), on_auth_ack(auth_ack_cb),
          server_mode(ip == nullptr)
    {
        sock = make_udp_sock();
        if (sock < 0) { BCONN_ERROR(CAT_NETWORK, "socket() failed errno=%d", errno); return; }

        if (server_mode) {
            // host: 지정 포트에 바인드
            int yes = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in local{};
            local.sin_family      = AF_INET;
            local.sin_port        = htons(port);
            local.sin_addr.s_addr = INADDR_ANY;
            if (bind(sock, (sockaddr*)&local, sizeof(local)) < 0)
                BCONN_ERROR(CAT_NETWORK, "bind() failed port=%d errno=%d", port, errno);
            else
                BCONN_INFO(CAT_NETWORK, "[host] listening on UDP port %d", port);
        } else {
            // client: host 주소로 connect (이후 send/recv 모두 이 소켓)
            sockaddr_in remote{};
            remote.sin_family      = AF_INET;
            remote.sin_port        = htons(port);
            remote.sin_addr.s_addr = inet_addr(ip);
            if (connect(sock, (sockaddr*)&remote, sizeof(remote)) < 0)
                BCONN_ERROR(CAT_NETWORK, "connect() failed host=%s port=%d errno=%d", ip, port, errno);
            else
                BCONN_INFO(CAT_NETWORK, "[client] socket connected to %s:%d", ip, port);
            peer_addr  = remote;
            peer_known = true;
        }
    }

    ~NetworkEngine() { stop(); }

    void start() {
        if (!running.exchange(true))
            recv_thread = std::thread(&NetworkEngine::recv_loop, this);
    }

    void stop() {
        if (running.exchange(false)) {
            // 블로킹된 recvfrom을 깨운 뒤 소켓을 닫고 스레드를 join한다.
            // close 전 shutdown으로 recv를 즉시 unblock한다.
            if (sock >= 0) {
                shutdown(sock, SHUT_RDWR);
                close(sock);
                sock = -1;
            }
            if (recv_thread.joinable()) recv_thread.join();
        }
    }

    // Event Tap 콜백에서 직접 호출 — 큐/스레드 없이 즉시 sendto
    void send_mouse(const MousePacket& p) { send_raw(&p, sizeof(p)); }
    void send_key(const KeyPacket& p)     { send_raw(&p, sizeof(p)); }

    // peer_known 무관하게 전송 (HELLO 인증 전 사용)
    void send_raw_any(const void* data, size_t len) {
        if (server_mode) return; // host는 peer 없이 보낼 대상 없음
        send(sock, data, len, 0);
    }

    // host: HELLO 인증 성공 후 peer 주소 등록
    void set_peer(const sockaddr_in& addr) {
        std::lock_guard<std::mutex> lock(peer_mutex);
        peer_addr = addr;
        peer_known.store(true, std::memory_order_release);
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
        BCONN_INFO(CAT_NETWORK, "[host] peer registered: %s:%d", ip_str, ntohs(addr.sin_port));
    }

    // host: AUTH_ACK 전송 (peer 등록 전이므로 sendto 직접 사용)
    void send_auth_result(bool accepted, const char* host_hostname, const sockaddr_in& to) {
        AuthAckPacket pkt{};
        pkt.packet_type = PACKET_AUTH;
        pkt.accepted    = accepted ? 1 : 0;
        strncpy(pkt.host_hostname, host_hostname, sizeof(pkt.host_hostname) - 1);
        sendto(sock, &pkt, sizeof(pkt), 0, (const sockaddr*)&to, sizeof(to));
    }

    // 하위 호환
    void push_mouse(const MousePacket& p) { send_mouse(p); }
    void push_key(const KeyPacket& p)     { send_key(p); }
    void push_event(const MousePacket& p) { send_mouse(p); }

private:
    int  sock        = -1;
    bool server_mode = false;
    sockaddr_in peer_addr{};
    std::mutex  peer_mutex;
    std::atomic<bool> peer_known{false};
    std::thread recv_thread;
    std::atomic<bool> running{false};
    MouseRecvCallback   on_mouse;
    KeyRecvCallback     on_key;
    HelloRecvCallback   on_hello;
    AuthAckRecvCallback on_auth_ack;

    void send_raw(const void* data, size_t len) {
        if (!peer_known.load(std::memory_order_acquire)) return;
        if (server_mode) {
            std::lock_guard<std::mutex> lock(peer_mutex);
            sendto(sock, data, len, 0, (sockaddr*)&peer_addr, sizeof(peer_addr));
        } else {
            send(sock, data, len, 0);
        }
    }

    void recv_loop() {
        uint8_t buf[MAX_PACKET_SIZE];
        sockaddr_in sender{};
        socklen_t   sender_len = sizeof(sender);

        while (running.load(std::memory_order_relaxed)) {
            ssize_t n;
            if (server_mode) {
                sender_len = sizeof(sender);
                n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&sender, &sender_len);
            } else {
                n = recv(sock, buf, sizeof(buf), 0);
            }

            if (n > 0) {
                dispatch(buf, n, sender);
            } else if (n < 0 && errno != EINTR) {
                // 소켓이 닫혔거나 복구 불가능한 에러 — spin 방지
                break;
            }
        }
    }

    void dispatch(const uint8_t* buf, ssize_t n, const sockaddr_in& sender) {
        uint8_t type = buf[0];
        if (type == PACKET_MOUSE && n == (ssize_t)sizeof(MousePacket) && on_mouse) {
            MousePacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_mouse(p);
        } else if (type == PACKET_KEYBOARD && n == (ssize_t)sizeof(KeyPacket) && on_key) {
            KeyPacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_key(p);
        } else if (type == PACKET_HELLO && n == (ssize_t)sizeof(HelloPacket) && on_hello) {
            HelloPacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_hello(p, sender);
        } else if (type == PACKET_AUTH && n == (ssize_t)sizeof(AuthAckPacket) && on_auth_ack) {
            AuthAckPacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_auth_ack(p);
        }
    }
};
