#pragma once
#include "protocol.h"
#include "TetherLog.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
#include <utility>
#include <mutex>
#include <vector>
#include <algorithm>
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
        // std::function 콜백은 값으로 받아 move 로 멤버에 넘겨 복사를 한 번 아낀다.
        : on_mouse(std::move(mouse_cb)), on_key(std::move(key_cb)),
          on_hello(std::move(hello_cb)), on_auth_ack(std::move(auth_ack_cb)),
          on_ping(std::move(ping_cb)), on_pong(std::move(pong_cb)),
          on_clipboard(std::move(clipboard_cb)),
          on_client_info(std::move(client_info_cb)),
          on_disconnect(std::move(disconnect_cb)),
          server_mode(ip == nullptr)
    {
        sock = make_udp_sock();
        if (sock < 0) { TETHER_ERROR(CAT_NETWORK, "socket() failed errno=%d", errno); return; }

        if (server_mode) {
            // host: 지정 포트에 바인드
            int yes = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in local{};
            local.sin_family      = AF_INET;
            local.sin_port        = htons(port);
            local.sin_addr.s_addr = INADDR_ANY;
            if (bind(sock, (sockaddr*)&local, sizeof(local)) < 0)
                TETHER_ERROR(CAT_NETWORK, "bind() failed port=%d errno=%d", port, errno);
            else
                TETHER_INFO(CAT_NETWORK, "[host] listening on UDP port %d", port);
        } else {
            // client: host 주소로 connect (이후 send/recv 모두 이 소켓)
            sockaddr_in remote{};
            remote.sin_family      = AF_INET;
            remote.sin_port        = htons(port);
            remote.sin_addr.s_addr = inet_addr(ip);
            if (connect(sock, (sockaddr*)&remote, sizeof(remote)) < 0)
                TETHER_ERROR(CAT_NETWORK, "connect() failed host=%s port=%d errno=%d", ip, port, errno);
            else
                TETHER_INFO(CAT_NETWORK, "[client] socket connected to %s:%d", ip, port);
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
        TETHER_INFO(CAT_NETWORK, "[host] peer registered: %s:%d", ip_str, ntohs(addr.sin_port));
    }

    // host: peer 초기화 (타임아웃 등으로 연결 끊김 시 대기 상태로 복귀)
    void clear_peer() {
        std::lock_guard<std::mutex> lock(peer_mutex);
        peer_known.store(false, std::memory_order_release);
        peer_addr = {};
        TETHER_INFO(CAT_NETWORK, "[host] peer cleared");
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

    // 클립보드 페이로드 전송 (양방향, 텍스트/이미지 공용)
    // MTU 초과 데이터를 CLIPBOARD_CHUNK_SIZE 단위로 쪼개 여러 청크로 전송한다.
    // 각 청크 헤더로 수신 측이 순서·유실을 감지해 재조립한다.
    // 형식: [PACKET_CLIPBOARD(1) | msg_id(2) | chunk_index(2) | total_chunks(2)
    //        | total_len(4) | payload_len(2)] + payload
    void send_clipboard(const uint8_t* data, uint32_t len) {
        if (len == 0) return;
        // 잘린 페이로드는 텍스트든 이미지든 쓸모없으므로 상한 초과 시 전송을 포기한다.
        if (len > CLIPBOARD_MAX_BYTES) {
            TETHER_INFO(CAT_NETWORK, "clipboard payload too large (%u bytes) — skipped", len);
            return;
        }
        uint16_t total_chunks = (uint16_t)((len + CLIPBOARD_CHUNK_SIZE - 1) / CLIPBOARD_CHUNK_SIZE);
        uint16_t msg_id = next_clip_msg_id_++;

        std::vector<uint8_t> buf(CLIPBOARD_HEADER_SIZE + CLIPBOARD_CHUNK_SIZE);
        for (uint16_t idx = 0; idx < total_chunks; ++idx) {
            uint32_t offset = (uint32_t)idx * CLIPBOARD_CHUNK_SIZE;
            uint16_t payload_len = (uint16_t)std::min(CLIPBOARD_CHUNK_SIZE, len - offset);
            buf[0] = PACKET_CLIPBOARD;
            memcpy(&buf[1], &msg_id, 2);
            memcpy(&buf[3], &idx, 2);
            memcpy(&buf[5], &total_chunks, 2);
            memcpy(&buf[7], &len, 4);
            memcpy(&buf[11], &payload_len, 2);
            memcpy(&buf[CLIPBOARD_HEADER_SIZE], data + offset, payload_len);
            send_raw(buf.data(), CLIPBOARD_HEADER_SIZE + payload_len);
            // 큰 이미지를 한 번에 쏟아내면 수신 소켓 버퍼(256KB)가 넘쳐 청크가 유실되고
            // 재조립 전체가 실패한다. 16청크(약 22KB)마다 1ms 쉬어 수신 측에 여유를 준다.
            // (수신 스레드가 시스템 부하로 잠시 밀려도 버퍼의 1/10 수준이라 안전 마진이 크다.)
            // 8MB 기준 총 지연 약 370ms — 호출부가 백그라운드 스레드라 UI 영향 없음.
            if ((idx & 15) == 15) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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

    // ── 클립보드 청크 송신/재조립 상태 ─────────────────────────────
    uint16_t next_clip_msg_id_ = 0;

    // 진행 중인 재조립 버퍼. 청크가 순서 없이 도착할 수 있으므로
    // 인덱스별 수신 여부를 함께 추적해 유실을 감지한다.
    struct ClipReassembly {
        uint16_t msg_id       = 0;
        uint16_t total_chunks = 0;
        uint32_t total_len    = 0;
        uint16_t received     = 0;      // 수신한 (중복 제외) 청크 수
        std::vector<uint8_t> data;      // total_len 크기로 미리 할당
        std::vector<bool>    got;       // 청크별 수신 여부
    };
    ClipReassembly clip_reasm_;

    // 클립보드 청크 하나를 처리한다. 마지막 조각까지 모여 검증을 통과하면 콜백 호출.
    void handle_clipboard_chunk(const uint8_t* buf, ssize_t n) {
        if (n < (ssize_t)CLIPBOARD_HEADER_SIZE) return;
        uint16_t msg_id, chunk_index, total_chunks, payload_len;
        uint32_t total_len;
        memcpy(&msg_id,       buf + 1,  2);
        memcpy(&chunk_index,  buf + 3,  2);
        memcpy(&total_chunks, buf + 5,  2);
        memcpy(&total_len,    buf + 7,  4);
        memcpy(&payload_len,  buf + 11, 2);

        // 헤더 유효성 검증 (손상/악의적 패킷 방어)
        if (total_chunks == 0 || total_len == 0 || total_len > CLIPBOARD_MAX_BYTES) return;
        if (chunk_index >= total_chunks) return;
        if (payload_len == 0 || payload_len > CLIPBOARD_CHUNK_SIZE) return;
        if (n != (ssize_t)(CLIPBOARD_HEADER_SIZE + payload_len)) return;

        // 새 메시지 도착 → 재조립 버퍼 초기화 (이전 미완성 메시지는 폐기)
        if (clip_reasm_.msg_id != msg_id || clip_reasm_.total_chunks != total_chunks
            || clip_reasm_.data.empty()) {
            clip_reasm_.msg_id       = msg_id;
            clip_reasm_.total_chunks = total_chunks;
            clip_reasm_.total_len    = total_len;
            clip_reasm_.received     = 0;
            clip_reasm_.data.assign(total_len, 0);
            clip_reasm_.got.assign(total_chunks, false);
        }

        uint32_t offset = (uint32_t)chunk_index * CLIPBOARD_CHUNK_SIZE;
        if (offset + payload_len > clip_reasm_.total_len) return;  // 길이 불일치 방어
        if (clip_reasm_.got[chunk_index]) return;                  // 중복 청크 무시

        memcpy(&clip_reasm_.data[offset], buf + CLIPBOARD_HEADER_SIZE, payload_len);
        clip_reasm_.got[chunk_index] = true;
        clip_reasm_.received++;

        // 모든 청크 수신 완료 → 무결성 검증 후 반영
        if (clip_reasm_.received == clip_reasm_.total_chunks) {
            // 중간 유실이 없었는지 총량으로 재확인 (중복/공백 방어의 최종 관문)
            if (clip_reasm_.data.size() == clip_reasm_.total_len && on_clipboard) {
                on_clipboard(clip_reasm_.data.data(), clip_reasm_.total_len);
            }
            clip_reasm_ = ClipReassembly{};  // 완료/폐기 후 상태 리셋
        }
    }

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
        if (type == PACKET_MOUSE && on_mouse &&
            (n == (ssize_t)sizeof(MousePacket) || n == (ssize_t)sizeof(MousePacket) - 1)) {
            // 구버전(button 필드 없는 9바이트)도 수용 — button=0 이면 기존 동작(center)과 동일.
            MousePacket p{};
            std::memcpy(&p, buf, n);
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
        } else if (type == PACKET_CLIPBOARD && n >= (ssize_t)CLIPBOARD_HEADER_SIZE && on_clipboard) {
            handle_clipboard_chunk(buf, n);
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
