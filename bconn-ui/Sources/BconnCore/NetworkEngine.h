#pragma once
#include "protocol.h"
#include "BconnLog.h"
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
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

    // 송신 버퍼: 빠른 마우스 이동 시 9B 패킷이 버스트로 몰려도 드롭되지 않게 충분히 확보.
    // 4KB(≈450패킷)는 버스트에서 커널 송신 큐를 넘겨 send() 드롭 → 원격 커서 끊김을 유발했다.
    // 패킷이 작아 버퍼를 키워도 실제 지연은 늘지 않는다 (커널이 즉시 NIC 로 flush).
    int sndbuf = 64 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // 수신 버퍼: 원격 중 host→client 마우스 패킷이 버스트로 몰려도 유실되지 않게 확보.
    // recv 스레드가 CGWarp/CGEventPost(동기 Mach RPC)로 잠시 블로킹되는 사이 도착한
    // 패킷이 커널 큐에 쌓이는데, 기본값이 작으면 넘쳐서 드롭된다. host 는 이 드롭으로
    // client PING(생존 신호)을 놓쳐 watchdog 오탐 복귀를 일으킬 수 있다.
    int rcvbuf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

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
    using MouseRecvCallback      = std::function<void(const MousePacket&)>;
    using KeyRecvCallback        = std::function<void(const KeyPacket&)>;
    using HelloRecvCallback      = std::function<void(const HelloPacket&, const sockaddr_in&)>;
    using AuthAckRecvCallback    = std::function<void(const AuthAckPacket&)>;
    using PingRecvCallback       = std::function<void(const sockaddr_in&)>;
    // PONG 은 host 의 현재 원격 활성 상태(1=원격, 0=로컬)를 함께 실어보낸다.
    // client 가 이 값과 자신의 커서 상태를 대조해 어긋나면 자가교정한다.
    using PongRecvCallback       = std::function<void(uint8_t host_active)>;
    using ClipboardRecvCallback  = std::function<void(const uint8_t* data, uint32_t len)>;
    using ClientInfoRecvCallback = std::function<void(const ClientInfoPacket&)>;
    using DisconnectCallback     = std::function<void()>;

    // host 모드: ip=nullptr, port=listen port
    // client 모드: ip=host ip, port=host port
    NetworkEngine(const char* ip, int port,
                  MouseRecvCallback      mouse_cb       = nullptr,
                  KeyRecvCallback        key_cb         = nullptr,
                  HelloRecvCallback      hello_cb       = nullptr,
                  AuthAckRecvCallback    auth_ack_cb    = nullptr,
                  PingRecvCallback       ping_cb        = nullptr,
                  PongRecvCallback       pong_cb        = nullptr,
                  ClipboardRecvCallback  clipboard_cb   = nullptr,
                  ClientInfoRecvCallback client_info_cb = nullptr,
                  DisconnectCallback     disconnect_cb  = nullptr)
        : on_mouse(mouse_cb), on_key(key_cb),
          on_hello(hello_cb), on_auth_ack(auth_ack_cb),
          on_ping(ping_cb), on_pong(pong_cb),
          on_clipboard(clipboard_cb),
          on_client_info(client_info_cb),
          on_disconnect(disconnect_cb),
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

    // host: peer 초기화 (타임아웃 등으로 연결 끊김 시 대기 상태로 복귀)
    void clear_peer() {
        std::lock_guard<std::mutex> lock(peer_mutex);
        peer_known.store(false, std::memory_order_release);
        peer_addr = {};
        BCONN_INFO(CAT_NETWORK, "[host] peer cleared");
    }

    // peer가 등록되어 있는지 확인 (원격 모드 진입 전 체크용)
    bool is_peer_known() const {
        return peer_known.load(std::memory_order_acquire);
    }

    // host: AUTH_ACK 전송 (peer 등록 전이므로 sendto 직접 사용)
    void send_auth_result(bool accepted, const char* host_hostname, const sockaddr_in& to) {
        AuthAckPacket pkt{};
        pkt.packet_type = PACKET_AUTH;
        pkt.accepted    = accepted ? 1 : 0;
        strncpy(pkt.host_hostname, host_hostname, sizeof(pkt.host_hostname) - 1);
        sendto(sock, &pkt, sizeof(pkt), 0, (const sockaddr*)&to, sizeof(to));
    }

    // client → host: 연결 유지 확인
    void send_ping() {
        uint8_t pkt = PACKET_PING;
        send_raw_any(&pkt, 1);
    }

    // host → client: PING 응답 (peer_known 필요).
    // host_active: 현재 원격 모드 활성 여부(1/0). client 가 커서 상태 자가교정에 사용.
    void send_pong(uint8_t host_active) {
        uint8_t pkt[2] = { PACKET_PONG, host_active };
        send_raw(pkt, sizeof(pkt));
    }

    // 클립보드 텍스트 전송 (양방향)
    // UDP fragmentation을 피하기 위해 CLIPBOARD_CHUNK_SIZE(MTU 안전)로 제한.
    // 초과 시 잘라서 전송한다 (수신 측에서 완전한 텍스트로 사용).
    // 형식: [PACKET_CLIPBOARD(1) | length(4)] + data
    void send_clipboard(const uint8_t* data, uint32_t len) {
        if (len == 0) return;
        // MTU 안전 크기로 제한 — 초과분은 잘림 (UDP에서 재조립은 비현실적)
        uint32_t send_len = std::min(len, CLIPBOARD_CHUNK_SIZE);
        std::vector<uint8_t> buf(5 + send_len);
        buf[0] = PACKET_CLIPBOARD;
        memcpy(&buf[1], &send_len, 4);
        memcpy(&buf[5], data, send_len);
        send_raw(buf.data(), buf.size());
    }

    // client → host: 화면 크기 전송 (단일 패킷)
    void send_client_info(int32_t width, int32_t height) {
        ClientInfoPacket pkt{};
        pkt.packet_type = PACKET_CLIENT_INFO;
        pkt.width       = width;
        pkt.height      = height;
        send_raw(&pkt, sizeof(pkt));
    }

    // client → host: 반대편 경계 이탈 복귀 요청 (비율 + 잔여 delta)
    void send_leave(uint8_t edge, int16_t ratio, int16_t residual_dx, int16_t residual_dy) {
        LeavePacket pkt{};
        pkt.packet_type = PACKET_LEAVE;
        pkt.edge        = edge;
        pkt.ratio       = ratio;
        pkt.residual_dx = residual_dx;
        pkt.residual_dy = residual_dy;
        send_raw_any(&pkt, sizeof(pkt));
    }

    // host: 복귀 요청 수신 콜백 등록 (setter — 생성자 인자 비대화 방지)
    void set_leave_callback(std::function<void(const LeavePacket&)> cb) { on_leave = std::move(cb); }

    // 네트워크 성능 측정 probe 수신 콜백 등록 (양방향). 인증 후 언제든 오갈 수 있다.
    void set_probe_callback(std::function<void(const ProbePacket&)> cb) { on_probe = std::move(cb); }

    // 측정 probe 전송 (요청 또는 응답). 인증 완료 상태에서만 의미가 있다.
    void send_probe(const ProbePacket& p) { send_raw(&p, sizeof(p)); }

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
    std::function<void(const LeavePacket&)> on_leave;
    std::function<void(const ProbePacket&)> on_probe;
    MouseRecvCallback      on_mouse;
    KeyRecvCallback        on_key;
    HelloRecvCallback      on_hello;
    AuthAckRecvCallback    on_auth_ack;
    PingRecvCallback       on_ping;
    PongRecvCallback       on_pong;
    ClipboardRecvCallback  on_clipboard;
    ClientInfoRecvCallback on_client_info;
    DisconnectCallback     on_disconnect;

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
        std::vector<uint8_t> buf(MAX_PACKET_SIZE);
        sockaddr_in sender{};
        socklen_t   sender_len = sizeof(sender);

        while (running.load(std::memory_order_relaxed)) {
            ssize_t n;
            if (server_mode) {
                sender_len = sizeof(sender);
                n = recvfrom(sock, buf.data(), buf.size(), 0, (sockaddr*)&sender, &sender_len);
            } else {
                n = recv(sock, buf.data(), buf.size(), 0);
            }

            if (n > 0) {
                dispatch(buf.data(), n, sender);
            } else if (n < 0) {
                if (errno == EINTR) continue;
                // Client의 connect()된 UDP 소켓에서 ECONNREFUSED는 일시적 에러
                // (Host가 아직 시작 안 됐거나 ICMP unreachable 수신).
                // 무시하고 재시도 — ping 루프가 재연결을 시도한다.
                if (!server_mode && (errno == ECONNREFUSED || errno == EHOSTUNREACH ||
                                     errno == ENETUNREACH)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                // 그 외 에러 (소켓 닫힘 등) — 루프 종료
                break;
            }
        }

        // 소켓 에러/종료로 루프 탈출 시 상위에 알림
        if (running.load(std::memory_order_relaxed) && on_disconnect) {
            on_disconnect();
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
        } else if (type == PACKET_PING && n == 1 && on_ping) {
            on_ping(sender);
        } else if (type == PACKET_PONG && (n == 1 || n == 2) && on_pong) {
            // 2바이트: [PONG, host_active]. 1바이트: 구버전 호환(active 미상 → 1로 간주).
            uint8_t host_active = (n == 2) ? buf[1] : 1;
            on_pong(host_active);
        } else if (type == PACKET_CLIPBOARD && n >= 5 && on_clipboard) {
            uint32_t len = 0;
            memcpy(&len, buf + 1, 4);
            if (len > 0 && len <= CLIPBOARD_CHUNK_SIZE && n == (ssize_t)(5 + len)) {
                on_clipboard(buf + 5, len);
            }
        } else if (type == PACKET_CLIENT_INFO && n == (ssize_t)sizeof(ClientInfoPacket) && on_client_info) {
            ClientInfoPacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_client_info(p);
        } else if (type == PACKET_LEAVE && n == (ssize_t)sizeof(LeavePacket) && on_leave) {
            LeavePacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_leave(p);
        } else if (type == PACKET_PROBE && n == (ssize_t)sizeof(ProbePacket) && on_probe) {
            ProbePacket p;
            std::memcpy(&p, buf, sizeof(p));
            on_probe(p);
        }
    }
};
