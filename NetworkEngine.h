#pragma once
#include "LockFreeQueue.h"
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <functional>

#pragma pack(push, 1)
struct MousePacket {
    uint64_t timestamp;
    int16_t dx;
    int16_t dy;
    uint32_t event_type;
};
#pragma pack(pop)

#define EVENT_CLIENT_INFO 9996  // 클라이언트가 화면 정보를 host에게 전달

class NetworkEngine {
private:
    int sock;
    sockaddr_in target_addr;
    SPSCQueue<MousePacket, 2048> packet_queue;
    std::thread send_thread;
    std::thread recv_thread;
    std::atomic<bool> running{false};

    // 클라이언트로부터 패킷을 받았을 때 호출할 콜백
    std::function<void(const MousePacket&)> on_recv;

    void send_loop() {
        MousePacket packet;
        int idle_count = 0;
        while (running.load(std::memory_order_relaxed)) {
            if (packet_queue.pop(packet)) {
                sendto(sock, &packet, sizeof(packet), 0,
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
        MousePacket packet;
        // 수신을 위해 소켓에 로컬 포트 바인딩
        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_port        = htons(ntohs(target_addr.sin_port) + 100); // e.g. 9100
        local.sin_addr.s_addr = INADDR_ANY;

        int rsock = socket(AF_INET, SOCK_DGRAM, 0);
        if (rsock < 0) return;

        // SO_REUSEADDR 설정
        int yes = 1;
        setsockopt(rsock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(rsock, (sockaddr*)&local, sizeof(local)) < 0) {
            close(rsock);
            return;
        }

        // non-blocking 수신 타임아웃 설정 (종료 감지용)
        struct timeval tv{0, 200000}; // 200ms
        setsockopt(rsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (running.load(std::memory_order_relaxed)) {
            ssize_t n = recvfrom(rsock, &packet, sizeof(packet), 0, nullptr, nullptr);
            if (n == (ssize_t)sizeof(packet) && on_recv) {
                on_recv(packet);
            }
        }
        close(rsock);
    }

public:
    NetworkEngine(const char* ip, int port,
                  std::function<void(const MousePacket&)> recv_cb = nullptr)
        : on_recv(recv_cb)
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
            if (on_recv) {
                recv_thread = std::thread(&NetworkEngine::recv_loop, this);
            }
        }
    }

    void stop() {
        if (running.exchange(false)) {
            if (send_thread.joinable()) send_thread.join();
            if (recv_thread.joinable()) recv_thread.join();
        }
    }

    void push_event(const MousePacket& packet) { packet_queue.push(packet); }

    // 클라이언트가 host에 응답을 보낼 때 사용하는 포트 번호
    static int reply_port(int base_port) { return base_port + 100; }
};
