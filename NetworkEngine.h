#pragma once
#include "LockFreeQueue.h"
#include "protocol.h"
#include <thread>
#include <atomic>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

// 가변 크기 패킷을 담는 공용 버퍼
struct RawPacket {
    uint8_t data[MAX_PACKET_SIZE];
    uint8_t size;

    static RawPacket from_mouse(const MousePacket& p) {
        RawPacket r;
        r.size = sizeof(MousePacket);
        std::memcpy(r.data, &p, sizeof(MousePacket));
        return r;
    }

    static RawPacket from_key(const KeyPacket& p) {
        RawPacket r;
        r.size = sizeof(KeyPacket);
        std::memcpy(r.data, &p, sizeof(KeyPacket));
        return r;
    }
};

class NetworkEngine {
public:
    using MouseRecvCallback = std::function<void(const MousePacket&)>;
    using KeyRecvCallback   = std::function<void(const KeyPacket&)>;

    NetworkEngine(const char* ip, int port,
                  MouseRecvCallback mouse_cb = nullptr,
                  KeyRecvCallback   key_cb   = nullptr)
        : on_mouse(mouse_cb), on_key(key_cb)
    {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) { std::cerr << "소켓 생성 실패" << std::endl; return; }

        int sndbuf = 65536;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        target_addr.sin_family      = AF_INET;
        target_addr.sin_port        = htons(port);
        target_addr.sin_addr.s_addr = inet_addr(ip);
    }

    ~NetworkEngine() {
        stop();
        if (sock >= 0) close(sock);
    }

    void start() {
        if (!running.exchange(true)) {
            send_thread = std::thread(&NetworkEngine::send_loop, this);
            if (on_mouse || on_key)
                recv_thread = std::thread(&NetworkEngine::recv_loop, this);
        }
    }

    void stop() {
        if (running.exchange(false)) {
            if (send_thread.joinable()) send_thread.join();
            if (recv_thread.joinable()) recv_thread.join();
        }
    }

    void push_mouse(const MousePacket& p) { packet_queue.push(RawPacket::from_mouse(p)); }
    void push_key(const KeyPacket& p)     { packet_queue.push(RawPacket::from_key(p)); }

    // 하위 호환: 기존 push_event 유지
    void push_event(const MousePacket& p) { push_mouse(p); }

private:
    int sock = -1;
    sockaddr_in target_addr{};
    SPSCQueue<RawPacket, 2048> packet_queue;
    std::thread send_thread;
    std::thread recv_thread;
    std::atomic<bool> running{false};
    MouseRecvCallback on_mouse;
    KeyRecvCallback   on_key;

    void send_loop() {
        RawPacket pkt;
        int idle_count = 0;
        while (running.load(std::memory_order_relaxed)) {
            if (packet_queue.pop(pkt)) {
                sendto(sock, pkt.data, pkt.size, 0,
                       (sockaddr*)&target_addr, sizeof(target_addr));
                idle_count = 0;
            } else {
                if (++idle_count > 100) {
                    struct timespec ts{0, 100000};
                    nanosleep(&ts, nullptr);
                } else {
                    std::this_thread::yield();
                }
            }
        }
    }

    void recv_loop() {
        int rsock = socket(AF_INET, SOCK_DGRAM, 0);
        if (rsock < 0) return;

        int yes = 1;
        setsockopt(rsock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_port        = htons(ntohs(target_addr.sin_port) + 100);
        local.sin_addr.s_addr = INADDR_ANY;

        if (bind(rsock, (sockaddr*)&local, sizeof(local)) < 0) {
            close(rsock);
            return;
        }

        struct timeval tv{0, 200000};
        setsockopt(rsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t buf[MAX_PACKET_SIZE];
        while (running.load(std::memory_order_relaxed)) {
            ssize_t n = recvfrom(rsock, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n < 1) continue;

            dispatch(buf, n);
        }
        close(rsock);
    }

    void dispatch(const uint8_t* buf, ssize_t n) {
        uint8_t type = buf[0];
        if (type == PACKET_MOUSE && n == (ssize_t)sizeof(MousePacket) && on_mouse) {
            MousePacket p;
            std::memcpy(&p, buf, sizeof(MousePacket));
            on_mouse(p);
        } else if (type == PACKET_KEYBOARD && n == (ssize_t)sizeof(KeyPacket) && on_key) {
            KeyPacket p;
            std::memcpy(&p, buf, sizeof(KeyPacket));
            on_key(p);
        }
    }
};
