#pragma once
#include "protocol.h"
#include <thread>
#include <atomic>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

// ── 소켓 유틸 ─────────────────────────────────────────────────────────

static inline int make_udp_sock() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    // 커널 버퍼 최소화: 버퍼가 크면 오래된 패킷이 쌓여 지연 증가
    int buf = 4096;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

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
    using MouseRecvCallback = std::function<void(const MousePacket&)>;
    using KeyRecvCallback   = std::function<void(const KeyPacket&)>;

    // host 모드: ip=nullptr, port=listen port
    // client 모드: ip=host ip, port=host port
    NetworkEngine(const char* ip, int port,
                  MouseRecvCallback mouse_cb = nullptr,
                  KeyRecvCallback   key_cb   = nullptr)
        : on_mouse(mouse_cb), on_key(key_cb), server_mode(ip == nullptr)
    {
        sock = make_udp_sock();
        if (sock < 0) { std::cerr << "소켓 생성 실패\n"; return; }

        if (server_mode) {
            // host: 지정 포트에 바인드, 첫 패킷 수신 후 client 주소 기록
            int yes = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in local{};
            local.sin_family      = AF_INET;
            local.sin_port        = htons(port);
            local.sin_addr.s_addr = INADDR_ANY;
            if (bind(sock, (sockaddr*)&local, sizeof(local)) < 0)
                std::cerr << "bind 실패\n";
        } else {
            // client: host 주소로 connect (이후 send/recv 모두 이 소켓)
            sockaddr_in remote{};
            remote.sin_family      = AF_INET;
            remote.sin_port        = htons(port);
            remote.sin_addr.s_addr = inet_addr(ip);
            if (connect(sock, (sockaddr*)&remote, sizeof(remote)) < 0)
                std::cerr << "connect 실패\n";
            peer_addr  = remote;
            peer_known = true;
        }
    }

    ~NetworkEngine() {
        stop();
        if (sock >= 0) close(sock);
    }

    void start() {
        if (!running.exchange(true))
            recv_thread = std::thread(&NetworkEngine::recv_loop, this);
    }

    void stop() {
        if (running.exchange(false)) {
            // 빈 패킷으로 블로킹된 recvfrom 깨우기
            if (sock >= 0) shutdown(sock, SHUT_RDWR);
            if (recv_thread.joinable()) recv_thread.join();
        }
    }

    // Event Tap 콜백에서 직접 호출 — 큐/스레드 없이 즉시 sendto
    void send_mouse(const MousePacket& p) { send_raw(&p, sizeof(p)); }
    void send_key(const KeyPacket& p)     { send_raw(&p, sizeof(p)); }

    // 하위 호환
    void push_mouse(const MousePacket& p) { send_mouse(p); }
    void push_key(const KeyPacket& p)     { send_key(p); }
    void push_event(const MousePacket& p) { send_mouse(p); }

private:
    int  sock        = -1;
    bool server_mode = false;
    sockaddr_in peer_addr{};
    std::atomic<bool> peer_known{false};
    std::thread recv_thread;
    std::atomic<bool> running{false};
    MouseRecvCallback on_mouse;
    KeyRecvCallback   on_key;

    void send_raw(const void* data, size_t len) {
        if (!peer_known.load(std::memory_order_acquire)) return;
        if (server_mode)
            sendto(sock, data, len, 0, (sockaddr*)&peer_addr, sizeof(peer_addr));
        else
            send(sock, data, len, 0);
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
                if (n > 0 && !peer_known.load(std::memory_order_relaxed)) {
                    peer_addr  = sender;
                    peer_known.store(true, std::memory_order_release);
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sender.sin_addr, ip_str, sizeof(ip_str));
                    std::cout << "[client 접속: " << ip_str << "]\n";
                }
            } else {
                n = recv(sock, buf, sizeof(buf), 0);
            }

            if (n > 0) dispatch(buf, n);
        }
    }

    void dispatch(const uint8_t* buf, ssize_t n) {
        uint8_t type = buf[0];
        if (type == PACKET_MOUSE && n == (ssize_t)sizeof(MousePacket) && on_mouse) {
            MousePacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_mouse(p);
        } else if (type == PACKET_KEYBOARD && n == (ssize_t)sizeof(KeyPacket) && on_key) {
            KeyPacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_key(p);
        }
    }
};
