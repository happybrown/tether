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

class NetworkEngine {
public:
    using RecvCallback = std::function<void(const MousePacket&)>;

    NetworkEngine(const char* ip, int port, RecvCallback recv_cb = nullptr)
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
            if (on_recv)
                recv_thread = std::thread(&NetworkEngine::recv_loop, this);
        }
    }

    void stop() {
        if (running.exchange(false)) {
            if (send_thread.joinable()) send_thread.join();
            if (recv_thread.joinable()) recv_thread.join();
        }
    }

    void push_event(const MousePacket& packet) { packet_queue.push(packet); }

private:
    int sock = -1;
    sockaddr_in target_addr{};
    SPSCQueue<MousePacket, 2048> packet_queue;
    std::thread send_thread;
    std::thread recv_thread;
    std::atomic<bool> running{false};
    RecvCallback on_recv;

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
        int rsock = socket(AF_INET, SOCK_DGRAM, 0);
        if (rsock < 0) return;

        int yes = 1;
        setsockopt(rsock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        // host는 base_port+100 에서 client의 응답을 수신
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

        MousePacket packet;
        while (running.load(std::memory_order_relaxed)) {
            ssize_t n = recvfrom(rsock, &packet, sizeof(packet), 0, nullptr, nullptr);
            if (n == (ssize_t)sizeof(packet))
                on_recv(packet);
        }
        close(rsock);
    }
};
