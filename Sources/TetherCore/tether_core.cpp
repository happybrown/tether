/// tether_core.cpp — 공개 API 및 전역 상태
///
/// 모듈 구조:
///   tether_core.cpp  — 전역 상태, tether_start/stop/get_status/get_local_ips
///   EventTap.cpp    — 호스트 전용: 디스플레이 레이아웃, 원격 모드, CGEventTap
///   ClientHandler.cpp — 클라이언트 전용: 패킷 수신, 커서 주입
///   NetworkEngine.h — UDP 송수신 엔진 (header-only)
///   protocol.h      — 패킷 구조체 및 HMAC 헬퍼
#include "include/tether_core.h"
#include "protocol.h"
#include "NetworkEngine.h"
#include "TetherLog.h"
#include "EventTap.h"
#include "ClientHandler.h"
#include "CursorUtil.h"
#include <ApplicationServices/ApplicationServices.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <csignal>
#include <cstdlib>
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>
#include <vector>
#include <cmath>

// ── 전역 상태 ─────────────────────────────────────────────────────────

static std::atomic<TetherStatus> g_status{TETHER_STATUS_IDLE};
static TetherStatusCallback g_cb  = nullptr;
static void*               g_ctx = nullptr;

// TetherLog.h 에서 extern 참조
TetherLogCallback g_log_cb  = nullptr;
void*            g_log_ctx = nullptr;

TetherConfig            g_cfg{};
NetworkEngine*         g_engine     = nullptr;
CFMachPortRef          g_mouse_tap  = nullptr;
CFMachPortRef          g_key_tap    = nullptr;

// g_run_loop: RunLoop 스레드가 CFRunLoopRun 진입 직전에 저장한다.
// 값이 nullptr 이 아니면 "RunLoop 준비 완료", (CFRunLoopRef)-1 은 "setup 실패" 를 의미한다.
// tether_stop 은 cv 로 준비를 기다린 후 CFRunLoopStop 을 호출한다.
std::atomic<CFRunLoopRef>      g_run_loop{nullptr};
static std::mutex              g_run_loop_mu;
static std::condition_variable g_run_loop_cv;
static std::thread             g_run_loop_thread;

// 인증 / 연결 유지 관련 전역
static char              g_psk[128]     = {};
static char              g_hostname[32] = {};
std::atomic<bool>        g_auth_ok{false};

// client: PING 루프 (인증 후 주기적 PING 전송)
static std::thread       g_ping_thread;
static std::atomic<bool> g_ping_running{false};
static uint16_t          g_reconnect_interval_s = 5;

// host: 자체 복귀 예측 사용 여부. EventTap 콜백(다른 스레드)에서 읽으므로 atomic.
// 기본 false. 끄면 client LeavePacket 안전망으로만 복귀한다(지연 있으나 오탐 없음).
std::atomic<bool>        g_predict_leave{false};

// ── 네트워크 성능(RTT) 측정 상태 ──────────────────────────────────────
// probe 요청을 보낸 seq 별 송신 시각을 기록해두고, 응답(is_ack=1) 수신 시 왕복을 잰다.
// 측정 스레드와 recv 스레드(probe 콜백)가 공유하므로 mutex 로 보호한다.
static std::mutex                 g_probe_mu;
static std::atomic<bool>          g_probe_running{false};
static std::vector<double>        g_probe_rtts;        // 수집된 RTT(ms)
static std::vector<int64_t>       g_probe_sent_us;     // seq index → 송신 시각(us), 0=미수신
static std::condition_variable    g_probe_cv;

// steady_clock 을 마이크로초로.
static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// probe 수신 처리 (host/client 공용, recv 스레드에서 호출).
//   요청(is_ack=0): 그대로 is_ack=1 로 되돌린다(echo).
//   응답(is_ack=1): 측정 중이면 seq 로 송신 시각을 찾아 왕복 시간을 기록한다.
static void handle_probe(const ProbePacket& p) {
    if (p.is_ack == 0) {
        ProbePacket ack = p;
        ack.is_ack = 1;
        if (g_engine) g_engine->send_probe(ack);
        return;
    }
    if (!g_probe_running.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_probe_mu);
    if (p.seq >= g_probe_sent_us.size()) return;
    int64_t sent = g_probe_sent_us[p.seq];
    if (sent == 0) return;                 // 이미 처리했거나 무효
    g_probe_sent_us[p.seq] = 0;            // 중복 응답 방지
    g_probe_rtts.push_back((now_us() - sent) / 1000.0);
    g_probe_cv.notify_all();
}

// host: watchdog — 이중 타임아웃
//   PEER_WARNING_S       : 연결 unhealthy 경고
//   PEER_TIMEOUT_S       : peer 초기화 + 원격 모드 강제 종료
//   g_remote_fast_timeout_s: 원격 모드 활성 시 빠른 복귀 (입력 먹통 방지)
//     PING 주기의 2배(최소 10초)로 잡아 PING 1회 유실만으로 복귀하지 않게 한다.
//     클라이언트 pong_timeout_remote_s 와 동일한 마진 — 양측 비대칭을 없앤다.
static std::thread            g_watchdog_thread;
static std::atomic<bool>      g_watchdog_running{false};
static std::atomic<time_t>    g_last_ping_time{0};
static const int              PEER_WARNING_S        = 8;
static const int              PEER_TIMEOUT_S        = 15;
static int                    g_remote_fast_timeout_s = 10;

// client: 마지막 PONG 수신 시각 (Host 생존 확인)
static std::atomic<time_t>    g_last_pong_time{0};

// start/stop 재진입/경쟁 방지. set_status 의 콜백은 이 lock 을 쓰지 않는다 (lock-free).
static std::mutex             g_lifecycle_mu;

// 클립보드 수신 콜백 (Swift 등록)
static TetherClipboardCallback g_clipboard_cb  = nullptr;
static void*                  g_clipboard_ctx = nullptr;

// client 화면 정보 (호스트가 client_handle_move 복귀 판단에 사용)
std::atomic<int32_t> g_client_width{32000};
std::atomic<int32_t> g_client_height{18000};

// host: client 화면 정렬 오프셋 (-1.0 ~ +1.0, client 화면 크기 대비 비율).
// EventTap.cpp 의 진입/복귀 비율 변환에서 참조한다. 자세한 의미는 tether_core.h 참고.
std::atomic<double> g_client_offset{0.0};

// host: ClientInfoPacket 수신 여부. g_client_width/height 의 기본값(sentinel)과
// 실제 수신값을 구분해, 설정 UI 가 "크기 미상" 상태를 표시할 수 있게 한다.
static std::atomic<bool> g_client_size_known{false};

// 원격 모드 / 클라이언트 상태 (EventTap.cpp, ClientHandler.cpp 에서 extern 참조)
RemoteState g_remote;
ClientState g_client;

// client: 디스플레이 변경 대기 플래그. Swift(메인 스레드)가 tether_displays_changed 로
// 세우고, ping 스레드가 감지해 client_apply_display_change 를 실행한다. client 의 커서/화면
// 상태는 recv·ping 스레드에서만 만지도록 일원화해 메인 스레드와의 경합을 피한다.
static std::atomic<bool> g_client_displays_dirty{false};



// ── 내부 헬퍼 ─────────────────────────────────────────────────────────

void tether_set_log_callback(TetherLogCallback cb, void* ctx) {
    g_log_cb  = cb;
    g_log_ctx = ctx;
}

void tether_set_clipboard_callback(TetherClipboardCallback cb, void* ctx) {
    g_clipboard_cb  = cb;
    g_clipboard_ctx = ctx;
}

void tether_set_force_stop_callback(TetherForceStopCallback cb, void* ctx) {
    set_force_stop_cb([cb, ctx]() {
        if (cb) cb(ctx);
    });
}

// 공개 헤더와 내부 프로토콜의 클립보드 상한이 어긋나면 컴파일 단계에서 잡는다.
static_assert(CLIPBOARD_MAX_BYTES == TETHER_CLIPBOARD_MAX_BYTES,
              "clipboard max size mismatch between protocol.h and tether_core.h");

void tether_send_clipboard(const uint8_t* data, uint32_t len) {
    // 큰 이미지 전송(페이싱 포함 수백 ms)을 위해 Swift 가 백그라운드 스레드에서 호출한다.
    // 전송 중 tether_stop 이 g_engine 을 파괴하지 않도록 생명주기 lock 으로 보호한다.
    std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mu);
    if (!g_engine) return;
    // 연결 확인은 모드별로 다르다. g_auth_ok 는 client 전용 상태(host 로부터 AUTH_ACK
    // 수신 여부)라, 이것만 검사하면 host 에선 항상 false → host→client 클립보드가
    // 전송되지 않는다. host 는 peer 등록(client 인증 성공) 여부로 판정해야 한다.
    bool ready = g_cfg.is_host ? g_engine->is_peer_known()
                               : g_auth_ok.load(std::memory_order_acquire);
    if (!ready) return;
    g_engine->send_clipboard(data, len);
}

/// 상태를 원자적으로 업데이트하고 등록된 콜백을 호출한다.
void set_status(TetherStatus s, const char* msg) {
    g_status.store(s);
    if (g_cb) g_cb(s, msg, g_ctx);
}

// 전방 선언
static void host_reset_peer(const char* reason);
static void client_reset_on_host_dead(const char* reason);

/// HELLO 패킷을 생성해 host로 전송한다.
/// PSK가 설정된 경우 HMAC-SHA1을 계산해 삽입한다.
static void send_hello() {
    HelloPacket pkt{};
    pkt.packet_type = PACKET_HELLO;
    strncpy(pkt.hostname, g_hostname, sizeof(pkt.hostname) - 1);
    pkt.timestamp_s = (int64_t)time(nullptr);

    TETHER_DEBUG(CAT_AUTH, "[클라이언트] HELLO 패킷 생성: 호스트명=%s, 타임스탬프=%lld, PSK 사용=%s",
                pkt.hostname, (long long)pkt.timestamp_s, strlen(g_psk) > 0 ? "예" : "아니오");

    if (strlen(g_psk) > 0) {
        char hn32[32] = {};
        memcpy(hn32, pkt.hostname, sizeof(hn32));
        compute_hmac(g_psk, hn32, pkt.timestamp_s, pkt.hmac);
    }
    // PSK 없으면 hmac은 zero → host에서 open mode로 수락

    g_engine->send_raw_any(&pkt, sizeof(pkt));
    TETHER_DEBUG(CAT_NETWORK, "[클라이언트] HELLO 패킷 전송됨");
}

// ── 공개 API ─────────────────────────────────────────────────────────

bool tether_start(const TetherConfig* config, TetherStatusCallback cb, void* ctx) {
    std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mu);
    tether_install_safety_hooks();

    TETHER_INFO(CAT_NETWORK, "tether_start: 모드=%s, 포트=%d, PSK=%s",
               config->is_host ? "호스트" : "클라이언트",
               config->port,
               strlen(config->psk) > 0 ? "설정됨" : "없음");

    if (g_status.load() != TETHER_STATUS_IDLE) {
        TETHER_ERROR(CAT_NETWORK, "tether_start 실패: 이미 실행 중 (상태=%d)", (int)g_status.load());
        return false;
    }

    g_cfg = *config;
    g_cb  = cb;
    g_ctx = ctx;

    strncpy(g_psk, g_cfg.psk, sizeof(g_psk) - 1);
    g_psk[sizeof(g_psk) - 1] = '\0';
    gethostname(g_hostname, sizeof(g_hostname));
    g_hostname[sizeof(g_hostname) - 1] = '\0';
    g_auth_ok.store(false, std::memory_order_release);
    g_reconnect_interval_s = (g_cfg.reconnect_interval_s > 0) ? g_cfg.reconnect_interval_s : 5;
    g_predict_leave.store(g_cfg.predict_leave, std::memory_order_relaxed);
    g_client_offset.store(std::clamp(g_cfg.client_offset, -1.0, 1.0), std::memory_order_relaxed);
    g_client_size_known.store(false, std::memory_order_release);
    // 원격 모드 빠른 복귀 타임아웃: PING 주기의 2배(최소 10초). PING 1회 유실로 복귀 방지.
    g_remote_fast_timeout_s = std::max(10, g_reconnect_interval_s * 2);
    // 시작 직후 경계에 커서가 있어도 즉시 원격 진입하지 않도록 보호 시각을 현재로 설정한다.
    g_remote.return_time = std::chrono::steady_clock::now();

    TETHER_INFO(CAT_NETWORK, "tether 시작: 호스트명=%s, 포트=%d", g_hostname, g_cfg.port);

    if (g_cfg.is_host) {
        // 기준 화면은 레이아웃 계산 전에 반영해야 첫 refresh 부터 적용된다.
        set_trigger_display_uuid(g_cfg.trigger_display_uuid);
        refresh_layout(g_cfg.client_edge);

        // host: ClientInfoPacket으로 클라이언트 화면 크기 수신
        auto on_client_info = [](const ClientInfoPacket& pkt) {
            g_client_width.store(pkt.width, std::memory_order_relaxed);
            g_client_height.store(pkt.height, std::memory_order_relaxed);
            g_client_size_known.store(true, std::memory_order_release);
            TETHER_DEBUG(CAT_NETWORK, "[호스트] 클라이언트 화면 크기=%dx%d", pkt.width, pkt.height);
        };

        auto on_mouse = [](const MousePacket&) {
            // host는 client로부터 마우스 패킷을 수신하지 않음 (복귀는 LeavePacket 전용)
        };

        // host: 클라이언트 복귀 요청 수신 (host 자체 예측이 주 경로, 이건 안전망)
        auto on_leave = [](const LeavePacket& pkt) {
            double ratio = static_cast<double>(pkt.ratio) / 32767.0;
            TETHER_INFO(CAT_CURSOR, "[호스트] 클라이언트 복귀 요청 수신(안전망): 비율=%.3f", ratio);
            request_leave_remote(ratio);
        };

        // host: HELLO 수신 → 재전송 공격 방지 → HMAC 검증 → AUTH_ACK 응답
        auto on_hello = [](const HelloPacket& pkt, const sockaddr_in& sender) {
            int64_t now  = (int64_t)time(nullptr);
            int64_t diff = pkt.timestamp_s - now;
            if (diff < -30 || diff > 30) {
                TETHER_ERROR(CAT_AUTH, "[호스트] HELLO 거부됨: 타임스탬프 허용 범위 초과 (diff=%llds, 클라이언트=%s)",
                            (long long)diff, pkt.hostname);
                return;
            }

            TETHER_INFO(CAT_AUTH, "[호스트] HELLO 수신: 클라이언트=%s, diff=%llds, 인증방식=%s",
                       pkt.hostname, (long long)diff, strlen(g_psk) > 0 ? "HMAC" : "Open");

            bool accepted = false;
            if (strlen(g_psk) == 0) {
                accepted = (pkt.hostname[0] != '\0');
            } else {
                char hn32[32] = {};
                memcpy(hn32, pkt.hostname, sizeof(hn32));
                uint8_t expected[20];
                compute_hmac(g_psk, hn32, pkt.timestamp_s, expected);
                accepted = (memcmp(expected, pkt.hmac, 20) == 0);
            }

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sender.sin_addr, ip_str, sizeof(ip_str));

            if (accepted) {
                g_engine->set_peer(sender);
                TETHER_INFO(CAT_AUTH, "[호스트] 인증 성공: %s (%s)", pkt.hostname, ip_str);
                set_status(TETHER_STATUS_CONNECTED, pkt.hostname);
            } else {
                TETHER_ERROR(CAT_AUTH, "[호스트] 인증 실패: %s (%s) PSK 불일치", pkt.hostname, ip_str);
            }

            g_engine->send_auth_result(accepted, g_hostname, sender);
        };

        // host: PING 수신 → 마지막 수신 시각 갱신 + PONG 응답
        auto on_ping = [](const sockaddr_in&) {
            g_last_ping_time.store(time(nullptr), std::memory_order_release);
            // 현재 원격 활성 상태를 PONG 에 실어 client 가 커서 상태를 자가교정하게 한다.
            uint8_t active = g_remote.active.load(std::memory_order_acquire) ? 1 : 0;
            TETHER_DEBUG(CAT_AUTH, "[호스트] PING 수신 → PONG 전송 (active=%u)", active);
            g_engine->send_pong(active);
        };

        auto on_clipboard_host = [](const uint8_t* data, uint32_t len) {
            if (g_clipboard_cb) g_clipboard_cb(data, len, g_clipboard_ctx);
        };

        // 소켓 에러 시 즉시 입력 복원
        auto on_disconnect_host = []() {
            TETHER_ERROR(CAT_NETWORK, "[호스트] 소켓 연결 끊김 감지 — 입력 복원");
            force_leave_remote("소켓 에러");
        };

        g_engine = new NetworkEngine(nullptr, g_cfg.port,
                                     on_mouse, nullptr, on_hello, nullptr,
                                     on_ping, nullptr, on_clipboard_host,
                                     on_client_info, on_disconnect_host);
        g_engine->set_leave_callback(on_leave);
        g_engine->set_probe_callback(handle_probe);
        g_engine->start();

        // watchdog: 원격 모드 비활성 시 2초 주기, 활성 시 1초 주기로 점검.
        //   >= PEER_WARNING_S       : "연결 불안정" 상태 표시 (1회)
        //   >= PEER_TIMEOUT_S       : peer 초기화 + 원격 모드 강제 종료
        //   >= REMOTE_FAST_TIMEOUT_S: 원격 모드 중이면 즉시 로컬 복귀 (입력 먹통 방지)
        g_watchdog_running.store(true, std::memory_order_release);
        g_watchdog_thread = std::thread([] {
            bool warned = false;
            while (g_watchdog_running.load(std::memory_order_acquire)) {
                // 원격 모드 활성 시 더 빠르게 점검
                bool in_remote = g_remote.active.load(std::memory_order_acquire);
                int sleep_ms = in_remote ? 1000 : 2000;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                if (!g_watchdog_running.load(std::memory_order_acquire)) break;

                time_t last = g_last_ping_time.load(std::memory_order_acquire);
                if (last == 0) { warned = false; continue; }

                double elapsed = difftime(time(nullptr), last);

                // 원격 모드 중 빠른 복귀: 짧은 타임아웃으로 먹통 최소화
                if (in_remote && elapsed >= g_remote_fast_timeout_s) {
                    TETHER_ERROR(CAT_AUTH, "[호스트] 원격 모드 중 PING 무응답 (%.0fs) — 즉시 로컬 복귀", elapsed);
                    force_leave_remote("원격 모드 중 연결 끊김");
                    if (elapsed >= PEER_TIMEOUT_S) {
                        host_reset_peer("ping timeout (remote)");
                        warned = false;
                    } else if (!warned) {
                        set_status(TETHER_STATUS_WAITING, "연결 불안정");
                        warned = true;
                    }
                    continue;
                }

                if (elapsed >= PEER_TIMEOUT_S) {
                    TETHER_ERROR(CAT_AUTH, "[호스트] PING 타임아웃 (%.0fs) — peer 초기화", elapsed);
                    host_reset_peer("ping timeout");
                    warned = false;
                } else if (!warned && elapsed >= PEER_WARNING_S) {
                    TETHER_INFO(CAT_AUTH, "[호스트] PING 지연 (%.0fs) — 연결 불안정", elapsed);
                    set_status(TETHER_STATUS_WAITING, "연결 불안정");
                    warned = true;
                }
            }
        });

        // RunLoop 스레드에서 이벤트 탭을 설정해야 CFRunLoopGetCurrent()가 올바른 루프를 반환한다.
        // 호출자(tether_start)는 setup 및 상태 전환까지 완료를 기다려 반환 후 즉시
        // tether_get_status 가 의미 있는 값을 반환하도록 한다.
        g_run_loop_thread = std::thread([] {
            setup_event_taps();
            if (g_status.load() == TETHER_STATUS_ERROR) {
                {
                    std::lock_guard<std::mutex> lk(g_run_loop_mu);
                    g_run_loop.store((CFRunLoopRef)-1, std::memory_order_release);
                }
                g_run_loop_cv.notify_all();
                return;
            }

            set_status(TETHER_STATUS_WAITING, "client 접속 대기 중");
            {
                std::lock_guard<std::mutex> lk(g_run_loop_mu);
                g_run_loop.store(CFRunLoopGetCurrent(), std::memory_order_release);
            }
            g_run_loop_cv.notify_all();

            CFRunLoopRun();

            std::lock_guard<std::mutex> lk(g_run_loop_mu);
            g_run_loop.store(nullptr, std::memory_order_release);
        });

        // run_loop 스레드가 setup 완료 후 상태 전환 + g_run_loop 저장까지 마치기를 기다린다.
        {
            std::unique_lock<std::mutex> lk(g_run_loop_mu);
            g_run_loop_cv.wait_for(lk, std::chrono::seconds(2), [] {
                return g_run_loop.load(std::memory_order_acquire) != nullptr;
            });
        }

    } else {
        // client 모드
        g_client.screen = get_screen_bounds();

        auto on_mouse = [](const MousePacket& pkt) {
            // 인증 완료 전에는 host 패킷 무시
            if (!g_auth_ok.load(std::memory_order_acquire)) {
                TETHER_DEBUG(CAT_NETWORK, "[클라이언트] 패킷 무시됨 (인증 전): 타입=%u", pkt.event_type);
                return;
            }

            TETHER_DEBUG(CAT_EVENT, "[클라이언트] 마우스 패킷: 타입=%u, dx=%d, dy=%d",
                pkt.event_type, (int)pkt.dx, (int)pkt.dy);

            switch (pkt.event_type) {
            case EVENT_SYNC_CURSOR:
                TETHER_INFO(CAT_CURSOR, "원격 모드 전환");
                client_handle_sync(pkt);
                // 원격 모드 진입 시점을 기준으로 PONG 타임아웃을 리셋.
                // 진입 전 마지막 PONG이 오래전이더라도 즉시 타임아웃 판정하지 않도록.
                g_last_pong_time.store(time(nullptr), std::memory_order_release);
                set_status(TETHER_STATUS_REMOTE, "원격 모드");
                break;
            case EVENT_SHOW_CURSOR:
                // 호스트가 로컬로 복귀 → 클라이언트 커서를 화면 밖으로 밀어 감춘다.
                // (메뉴바 앱에서 CGDisplayHideCursor 가 안 먹으므로 offscreen warp 사용)
                client_hide_cursor_offscreen();
                g_client.cursor_hidden = true;
                g_client.pos_init = false;
                set_status(TETHER_STATUS_CONNECTED, "연결됨");
                break;
            default:
                if (g_client.pos_init) {
                    client_handle_move(pkt);
                } else {
                    TETHER_DEBUG(CAT_CURSOR, "[클라이언트] 커서 이동 무시됨 (위치 미초기화): 타입=%u",
                                pkt.event_type);
                }
                break;
            }
        };

        auto on_key = [](const KeyPacket& pkt) {
            if (!g_auth_ok.load(std::memory_order_acquire)) return;
            // modifier 해제는 복귀 신호이므로 pos_init(원격 활성) 가드와 무관하게 처리한다.
            // SHOW_CURSOR(pos_init=false)와 RESET_MODIFIERS 는 UDP 라 순서가 뒤집힐 수 있어,
            // 이 가드를 두면 SHOW_CURSOR 가 먼저 와 pos_init=false 가 됐을 때 리셋이 무시된다.
            if (pkt.event_type == EVENT_RESET_MODIFIERS) { client_handle_key(pkt); return; }
            if (g_client.pos_init) client_handle_key(pkt);
        };

        // client: AUTH_ACK 수신 → 인증 상태 업데이트
        auto on_auth_ack = [](const AuthAckPacket& pkt) {
            if (pkt.accepted) {
                g_auth_ok.store(true, std::memory_order_release);
                // 인증 직후 PONG 기준점 설정 — 아직 PONG 수신 전이라도 dead 판정 방지
                g_last_pong_time.store(time(nullptr), std::memory_order_release);
                // 재연결 시 화면 정보가 바뀌었을 수 있으므로 갱신 후 전송
                g_client.screen = get_screen_bounds();
                send_client_info();
                set_status(TETHER_STATUS_CONNECTED, "연결됨");
                TETHER_INFO(CAT_AUTH, "[클라이언트] 인증 성공: 호스트=%s", pkt.host_hostname);
            } else {
                g_auth_ok.store(false, std::memory_order_release);
                set_status(TETHER_STATUS_AUTH_FAILED, "PSK 불일치");
                TETHER_ERROR(CAT_AUTH, "[클라이언트] 인증 실패 (호스트가 거부)");
            }
        };

        // PONG 수신: 호스트가 살아있음을 확인. + 커서 상태 자가교정.
        // host_active 와 client 의 원격 상태(pos_init)가 어긋나면 제어 신호(SHOW/SYNC)
        // 유실로 커서 상태가 틀어진 것이므로 바로잡는다. recv 스레드 단일 실행이라
        // client_handle_* 와 동일 스레드 — 커서 조작에 별도 동기화 불필요.
        auto on_pong = [](uint8_t host_active) {
            g_last_pong_time.store(time(nullptr), std::memory_order_release);
            if (!g_auth_ok.load(std::memory_order_acquire)) return;

            // host 로컬(0) 인데 client 는 원격(pos_init=true): SHOW_CURSOR 유실 →
            // 양측에 커서가 동시 표시된 상태. client 커서를 숨기고 로컬로 되돌린다.
            if (host_active == 0 && g_client.pos_init) {
                TETHER_INFO(CAT_CURSOR, "[클라이언트] 상태 불일치 교정: host 로컬인데 원격 유지 → 커서 숨김");
                g_client.pos_init = false;
                client_hide_cursor_offscreen();
                g_client.cursor_hidden = true;
                set_status(TETHER_STATUS_CONNECTED, "연결됨");
            }
            // host 원격(1) 인데 client 는 로컬(pos_init=false): SYNC_CURSOR 유실.
            // 위치 비율 정보가 없어 여기서 커서를 복원하지 않는다. host 가 다음 이동/복귀
            // 패킷으로 재동기화하며, 그때까지 잠깐 양측 커서가 안 보일 수 있다(무해).
            else if (host_active == 1 && !g_client.pos_init) {
                TETHER_DEBUG(CAT_CURSOR, "[클라이언트] 상태 불일치 감지: host 원격인데 로컬 (SYNC 대기)");
            }
        };

        auto on_clipboard_client = [](const uint8_t* data, uint32_t len) {
            if (!g_auth_ok.load(std::memory_order_acquire)) return;
            if (g_clipboard_cb) g_clipboard_cb(data, len, g_clipboard_ctx);
        };

        // 소켓 에러 시 즉시 입력 복원
        auto on_disconnect_client = []() {
            TETHER_ERROR(CAT_NETWORK, "[클라이언트] 소켓 연결 끊김 감지 — 입력 복원");
            client_reset_on_host_dead("소켓 에러");
        };

        g_engine = new NetworkEngine(g_cfg.host_ip, g_cfg.port,
                                     on_mouse, on_key, nullptr, on_auth_ack,
                                     nullptr, on_pong, on_clipboard_client,
                                     nullptr, on_disconnect_client);
        g_engine->set_probe_callback(handle_probe);
        g_engine->start();

        TETHER_INFO(CAT_AUTH, "[클라이언트] HELLO 전송: %s:%d (재시도 간격=%us)",
                   g_cfg.host_ip, g_cfg.port, g_reconnect_interval_s);
        send_hello();
        set_status(TETHER_STATUS_WAITING, "인증 대기 중");

        // ping 루프: 인증 전에는 HELLO 재전송, 인증 후에는 PING 전송.
        // PONG 이 pong_timeout_s 이상 오지 않으면 Host 사망으로 판정해 재인증으로 돌아간다.
        // 원격 모드 활성 시에는 더 짧은 타임아웃을 적용해 먹통 시간을 최소화한다.
        g_ping_running.store(true, std::memory_order_release);
        g_ping_thread = std::thread([] {
            const int interval_s = g_reconnect_interval_s;
            const int pong_timeout_s = std::max(15, interval_s * 3);
            // 원격 모드 활성 시 빠른 감지 (입력 먹통 방지)
            const int pong_timeout_remote_s = std::max(10, interval_s * 2);
            // 원격 모드 중에는 PING 을 1초 주기로 촘촘히 보낸다.
            // host 는 client PING 도착으로만 연결 생존을 판단하는데, 원격 중 데이터는
            // host→client 단방향이라 역방향(PING)이 성기면 유실 하나에 host watchdog 이
            // 오탐 복귀한다. 1초 heartbeat 이면 유실 하나가 나도 다음 것이 곧 도착한다.
            const int REMOTE_PING_INTERVAL_MS = 1000;

            // 100ms 틱 루프. 원격 여부에 따라 PING 전송 주기를 동적으로 정한다.
            int elapsed_ms = 0;
            while (g_ping_running.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!g_ping_running.load(std::memory_order_acquire)) break;
                elapsed_ms += 100;

                // 디스플레이 변경 처리: 메인 스레드가 세운 플래그를 이 스레드에서 소비해
                // 화면 재계산/재전송/복원을 한다 (client 커서·화면 상태를 단일 스레드로 일원화).
                if (g_client_displays_dirty.exchange(false, std::memory_order_acq_rel) &&
                    g_auth_ok.load(std::memory_order_acquire)) {
                    client_apply_display_change();
                }

                bool in_remote = g_client.pos_init;
                int send_interval_ms = in_remote ? REMOTE_PING_INTERVAL_MS : interval_s * 1000;
                if (elapsed_ms < send_interval_ms) continue;
                elapsed_ms = 0;

                if (g_auth_ok.load(std::memory_order_acquire)) {
                    TETHER_DEBUG(CAT_AUTH, "[클라이언트] PING 전송");
                    g_engine->send_ping();

                    time_t last = g_last_pong_time.load(std::memory_order_acquire);
                    if (last != 0) {
                        double elapsed = difftime(time(nullptr), last);
                        // 원격 모드 중이면 짧은 타임아웃 적용
                        int timeout = in_remote ? pong_timeout_remote_s : pong_timeout_s;
                        if (elapsed >= timeout) {
                            client_reset_on_host_dead(
                                in_remote ? "PONG 타임아웃 (원격 모드)" : "PONG 타임아웃");
                        }
                    }
                } else {
                    TETHER_INFO(CAT_AUTH, "[클라이언트] HELLO 재전송 (미인증 또는 재연결)");
                    send_hello();
                }
            }
        });
    }

    TETHER_INFO(CAT_NETWORK, "tether_start 완료");
    return true;
}

// ── 비정상 종료 안전 훅 ──────────────────────────────────────────────
//
// crash / atexit 경로에서 이벤트 탭을 비활성화하고 커서-포인터 연동을 복구한다.
// SIGTERM/SIGINT/SIGHUP 은 Swift DispatchSource 가 맡아 정상 tether_stop 경로로 유도하고,
// 여기서는 catch 불가능/복구 불가능한 crash 계열만 처리한다.

static std::atomic<bool> g_safety_installed{false};

// async-signal-safe 한계 내에서만 호출. CGEvent* 계열은 공식 보장은 없으나
// 단일 Mach RPC 이므로 실무상 안전하고, 실패해도 기존보다 나쁠 것 없다.
std::atomic<bool> g_taps_active{false};

static void safety_cleanup() {
    // teardown 완료 후에는 이미 정리됐으므로 중복 실행 방지
    if (!g_taps_active.exchange(false, std::memory_order_acq_rel)) return;

    if (g_mouse_tap) CGEventTapEnable(g_mouse_tap, false);
    if (g_key_tap)   CGEventTapEnable(g_key_tap,   false);
    CGAssociateMouseAndMouseCursorPosition(true);

    CGDirectDisplayID main_id = CGMainDisplayID();
    CGRect b = CGDisplayBounds(main_id);
    CGWarpMouseCursorPosition(CGPointMake(b.origin.x + b.size.width * 0.5,
                                          b.origin.y + b.size.height * 0.5));
}

static void crash_signal_handler(int sig) {
    static const char msg[] = "[tether] crash signal — restoring input\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    safety_cleanup();
    // 원래 동작 복원 후 재전달 → core dump / 디버거
    signal(sig, SIG_DFL);
    raise(sig);
}

void tether_install_safety_hooks() {
    bool expected = false;
    if (!g_safety_installed.compare_exchange_strong(expected, true)) return;

    atexit(safety_cleanup);

    struct sigaction sa{};
    sa.sa_handler = crash_signal_handler;
    sa.sa_flags   = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    // crash 계열만 처리 (SIGKILL/SIGSTOP 은 catch 불가).
    // SIGTERM/SIGINT/SIGHUP 은 Swift 측 DispatchSource 가 맡는다.
    const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGPIPE };
    for (int s : sigs) {
        struct sigaction old{};
        sigaction(s, nullptr, &old);
        // 디버거 등이 이미 설치한 핸들러는 덮어쓰지 않음.
        if (old.sa_handler == SIG_DFL || old.sa_handler == SIG_IGN)
            sigaction(s, &sa, nullptr);
    }
    TETHER_INFO(CAT_NETWORK, "safety hook 설치 완료");
}

// host 전용: peer 연결을 초기화하고 대기 상태로 복귀.
// 원격 모드였다면 먼저 로컬 복귀시켜 Host 의 마우스/키보드 소비를 멈춘다.
static void host_reset_peer(const char* reason) {
    TETHER_INFO(CAT_AUTH, "[호스트] peer 연결 초기화: %s", reason);
    force_leave_remote(reason);
    g_engine->clear_peer();
    g_last_ping_time.store(0, std::memory_order_release);
    set_status(TETHER_STATUS_WAITING, "client 접속 대기 중");
}

// client 전용: Host 무응답 감지 시 호출. 인증/원격 상태를 해제해
// 다음 ping 루프에서 HELLO 재전송으로 자동 재연결되도록 한다.
// 즉시 입력을 복원하여 사용자가 로컬 Mac을 정상 사용할 수 있게 한다.
static void client_reset_on_host_dead(const char* reason) {
    TETHER_ERROR(CAT_AUTH, "[클라이언트] 호스트 연결 끊김 감지: %s", reason);
    g_auth_ok.store(false, std::memory_order_release);

    // 즉시 입력 복원 — 원격 모드 중이었다면 커서를 화면 안으로 되돌리고 위치를 초기화
    if (g_client.pos_init) {
        g_client.pos_init = false;
        if (g_client.cursor_hidden) {
            client_show_cursor_onscreen();
            g_client.cursor_hidden = false;
        }
    }
    CGAssociateMouseAndMouseCursorPosition(true);

    g_last_pong_time.store(0, std::memory_order_release);
    set_status(TETHER_STATUS_WAITING, "호스트 응답 없음 — 재연결 중");
}

void tether_stop() {
    std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mu);
    TETHER_INFO(CAT_NETWORK, "tether_stop: 연결 종료 시작");

    // join 대기 중 set_status → Swift 콜백 → 메인 스레드 데드락 방지
    g_cb  = nullptr;
    g_ctx = nullptr;

    g_ping_running.store(false, std::memory_order_release);
    if (g_ping_thread.joinable()) g_ping_thread.join();

    g_watchdog_running.store(false, std::memory_order_release);
    if (g_watchdog_thread.joinable()) g_watchdog_thread.join();

    // RunLoop 스레드가 아직 CFRunLoopRun 에 진입하기 전이면 Stop 이 무시되어 join 이 영구 블록된다.
    // 준비 완료 또는 setup 실패 (reinterpret -1) 를 최대 2초 기다린 뒤 Stop 을 호출한다.
    {
        std::unique_lock<std::mutex> lk(g_run_loop_mu);
        g_run_loop_cv.wait_for(lk, std::chrono::seconds(2), [] {
            return g_run_loop.load(std::memory_order_acquire) != nullptr;
        });
        CFRunLoopRef rl = g_run_loop.load(std::memory_order_acquire);
        if (rl && rl != (CFRunLoopRef)-1) CFRunLoopStop(rl);
    }
    if (g_run_loop_thread.joinable()) g_run_loop_thread.join();
    g_run_loop.store(nullptr, std::memory_order_release);

    teardown_event_taps();

    if (g_engine) {
        delete g_engine;   // 소멸자에서 stop() → recv_thread join → socket close
        g_engine = nullptr;
    }

    CGAssociateMouseAndMouseCursorPosition(true);
    // Client 원격 중 종료 시, offscreen 으로 밀어둔 커서를 화면 안으로 되돌린다.
    if (g_client.cursor_hidden) {
        client_show_cursor_onscreen();
        g_client.cursor_hidden = false;
    }
    g_remote.active.store(false, std::memory_order_release);
    g_remote.entry_x = g_remote.entry_y = 0.0;
    g_remote.return_x = g_remote.return_y = 0.0;
    g_remote.entry_display = CGRectZero;
    g_remote.entry_guard = 0;
    g_remote.return_time = {};
    g_client  = {};
    g_auth_ok.store(false, std::memory_order_release);
    g_last_pong_time.store(0, std::memory_order_release);
    g_client_displays_dirty.store(false, std::memory_order_release);
    set_status(TETHER_STATUS_IDLE, "중지됨");

    TETHER_INFO(CAT_NETWORK, "tether_stop 완료");
}

TetherStatus tether_get_status() {
    return g_status.load();
}

void tether_force_local() {
    if (g_status.load() == TETHER_STATUS_IDLE) return;
    if (g_cfg.is_host) {
        force_leave_remote("사용자 요청");
    } else {
        client_reset_on_host_dead("사용자 요청");
    }
}

void tether_set_client_edge(TetherEdge edge) {
    // 실행 전 설정은 tether_start 의 config.client_edge 로 전달되므로 여기선 무시한다.
    // 이 API 는 "실행 중" host 의 경계를 재시작 없이 실시간 갱신하는 용도다.
    if (g_status.load() == TETHER_STATUS_IDLE || !g_cfg.is_host) return;
    apply_client_edge(edge);
}

void tether_set_predict_leave(bool enabled) {
    // 실행 여부와 무관하게 플래그를 갱신한다. 실행 전 설정은 tether_start 가 config 로
    // 덮어쓰지만, 실행 중 토글은 재시작 없이 다음 이동 이벤트부터 즉시 반영된다.
    g_predict_leave.store(enabled, std::memory_order_relaxed);
    g_cfg.predict_leave = enabled;  // 이후 재시작 시에도 유지
}

void tether_set_trigger_display(const char* uuid) {
    // 실행 전에도 값을 보관해 다음 start 에 반영되게 한다 (client 모드에선 의미 없음).
    strncpy(g_cfg.trigger_display_uuid, uuid ? uuid : "",
            sizeof(g_cfg.trigger_display_uuid) - 1);
    g_cfg.trigger_display_uuid[sizeof(g_cfg.trigger_display_uuid) - 1] = '\0';
    if (g_status.load() == TETHER_STATUS_IDLE || !g_cfg.is_host) {
        set_trigger_display_uuid(g_cfg.trigger_display_uuid);
        return;
    }
    apply_trigger_display(g_cfg.trigger_display_uuid);
}

int tether_get_displays(TetherDisplayInfo* out, int max) {
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, ids, &count);
    if (!out) return (int)count;
    if (max <= 0) return 0;

    int n = 0;
    for (uint32_t i = 0; i < count && n < max; ++i) {
        CGRect b = CGDisplayBounds(ids[i]);
        TetherDisplayInfo& d = out[n];
        d.uuid[0] = '\0';
        if (CFUUIDRef u = CGDisplayCreateUUIDFromDisplayID(ids[i])) {
            if (CFStringRef s = CFUUIDCreateString(kCFAllocatorDefault, u)) {
                CFStringGetCString(s, d.uuid, sizeof(d.uuid), kCFStringEncodingUTF8);
                CFRelease(s);
            }
            CFRelease(u);
        }
        d.x = b.origin.x;      d.y = b.origin.y;
        d.width = b.size.width; d.height = b.size.height;
        d.is_main    = CGDisplayIsMain(ids[i]);
        d.is_builtin = CGDisplayIsBuiltin(ids[i]);
        ++n;
    }
    return n;
}

bool tether_get_client_size(int32_t* width, int32_t* height) {
    if (!g_client_size_known.load(std::memory_order_acquire)) return false;
    if (width)  *width  = g_client_width.load(std::memory_order_relaxed);
    if (height) *height = g_client_height.load(std::memory_order_relaxed);
    return true;
}

void tether_set_client_offset(double offset) {
    // predict_leave 와 같은 방식의 실시간 갱신. 값 하나만 바꾸므로 RunLoop 위임이 필요 없고,
    // 다음 진입/복귀 비율 계산부터 즉시 반영된다.
    double clamped = std::clamp(offset, -1.0, 1.0);
    g_client_offset.store(clamped, std::memory_order_relaxed);
    g_cfg.client_offset = clamped;  // 이후 재시작 시에도 유지
}

void tether_displays_changed() {
    if (g_status.load() == TETHER_STATUS_IDLE) return;

    if (g_cfg.is_host) {
        // host: 현재 경계로 레이아웃을 재계산한다. apply_client_edge 가 RunLoop 스레드에서
        // 원격 모드 복귀 + refresh_layout 을 수행하므로 그대로 재사용한다.
        // (host 는 CGDisplayRegisterReconfigurationCallback 도 있어 이중 안전망이다.)
        TETHER_INFO(CAT_CURSOR, "[호스트] 디스플레이 변경 통지 — 레이아웃 재계산");
        apply_client_edge(g_cfg.client_edge);
    } else {
        // client: 화면 재계산은 커서/화면 상태를 만지므로 recv·ping 스레드로 위임한다.
        // 플래그만 세우고 ping 스레드가 다음 틱(≤100ms)에 소비한다.
        g_client_displays_dirty.store(true, std::memory_order_release);
    }
}

int tether_get_local_ips(char* out, int out_size) {
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) return 0;

    int count = 0;
    int pos   = 0;
    for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strncmp(ifa->ifa_name, "lo", 2) == 0) continue;

        char ip[64] = {};
        getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                    ip, sizeof(ip), nullptr, 0, NI_NUMERICHOST);

        int len = (int)strlen(ip);
        if (pos + len + 2 > out_size) break;
        memcpy(out + pos, ip, len);
        out[pos + len] = '\n';
        pos += len + 1;
        ++count;
    }
    if (pos < out_size) out[pos] = '\0';
    freeifaddrs(ifap);
    return count;
}

bool tether_run_netperf(int count, int interval_ms, TetherNetPerfCallback cb, void* ctx) {
    if (!cb) return false;
    // 동시 측정 방지 — 진행 중이면 시작하지 않는다.
    bool expected = false;
    if (!g_probe_running.compare_exchange_strong(expected, true)) return false;

    if (count < 1)   count = 1;
    if (count > 1000) count = 1000;               // 폭주 방지 상한
    if (interval_ms < 5) interval_ms = 5;

    std::thread([count, interval_ms, cb, ctx]() {
        // 미연결이면 즉시 빈 결과 반환.
        // 연결 판정은 모드별로 다르다:
        //   host  : peer 등록(client 인증 성공)만으로 probe 를 되받을 수 있다.
        //           g_auth_ok 는 client 전용 상태라 host 에선 항상 false → 여기서 쓰면 안 된다.
        //   client: host 로부터 AUTH_ACK 를 받아(g_auth_ok) 실제 연결이 확인된 상태여야 한다.
        bool connected = g_engine && g_engine->is_peer_known() &&
                         (g_cfg.is_host || g_auth_ok.load(std::memory_order_acquire));
        if (!connected) {
            TetherNetPerfResult r{};
            cb(&r, ctx);
            g_probe_running.store(false, std::memory_order_release);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_probe_mu);
            g_probe_rtts.clear();
            g_probe_sent_us.assign(count, 0);
        }

        // count 회 probe 를 interval 간격으로 전송.
        for (int i = 0; i < count; ++i) {
            ProbePacket p{};
            p.packet_type = PACKET_PROBE;
            p.is_ack      = 0;
            p.seq         = (uint16_t)i;
            int64_t t     = now_us();
            p.t_send_us   = t;
            {
                std::lock_guard<std::mutex> lk(g_probe_mu);
                g_probe_sent_us[i] = t;
            }
            g_engine->send_probe(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }

        // 마지막 응답을 위해 잠깐 더 대기 (지연이 큰 응답 수집). 최대 1초.
        {
            std::unique_lock<std::mutex> lk(g_probe_mu);
            g_probe_cv.wait_for(lk, std::chrono::milliseconds(1000), [count]() {
                return (int)g_probe_rtts.size() >= count;
            });
        }

        // 통계 계산.
        TetherNetPerfResult r{};
        r.sent = count;
        std::vector<double> rtts;
        {
            std::lock_guard<std::mutex> lk(g_probe_mu);
            rtts = g_probe_rtts;
        }
        r.received = (int)rtts.size();
        r.loss_pct = 100.0 * (count - r.received) / count;
        if (!rtts.empty()) {
            double sum = 0, mn = rtts[0], mx = rtts[0];
            for (double v : rtts) { sum += v; mn = std::min(mn, v); mx = std::max(mx, v); }
            r.avg_ms = sum / rtts.size();
            r.min_ms = mn;
            r.max_ms = mx;
            double var = 0;
            for (double v : rtts) var += (v - r.avg_ms) * (v - r.avg_ms);
            r.stddev_ms = std::sqrt(var / rtts.size());
        }

        TETHER_INFO(CAT_NETWORK,
            "[성능측정] 전송=%d 수신=%d 손실=%.0f%% RTT min/avg/max=%.1f/%.1f/%.1fms 지터=%.1fms",
            r.sent, r.received, r.loss_pct, r.min_ms, r.avg_ms, r.max_ms, r.stddev_ms);

        cb(&r, ctx);
        g_probe_running.store(false, std::memory_order_release);
    }).detach();

    return true;
}
