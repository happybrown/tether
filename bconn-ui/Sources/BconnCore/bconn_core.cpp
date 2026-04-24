/// bconn_core.cpp — 공개 API 및 전역 상태
///
/// 모듈 구조:
///   bconn_core.cpp  — 전역 상태, bconn_start/stop/get_status/get_local_ips
///   EventTap.cpp    — 호스트 전용: 디스플레이 레이아웃, 원격 모드, CGEventTap
///   ClientHandler.cpp — 클라이언트 전용: 패킷 수신, 커서 주입
///   NetworkEngine.h — UDP 송수신 엔진 (header-only)
///   protocol.h      — 패킷 구조체 및 HMAC 헬퍼
#include "include/bconn_core.h"
#include "protocol.h"
#include "NetworkEngine.h"
#include "BconnLog.h"
#include "EventTap.h"
#include "ClientHandler.h"
#include <ApplicationServices/ApplicationServices.h>
#include <objc/objc-runtime.h>
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

// ── 전역 상태 ─────────────────────────────────────────────────────────

static std::atomic<BconnStatus> g_status{BCONN_STATUS_IDLE};
static BconnStatusCallback g_cb  = nullptr;
static void*               g_ctx = nullptr;

// BconnLog.h 에서 extern 참조
BconnLogCallback g_log_cb  = nullptr;
void*            g_log_ctx = nullptr;

BconnConfig            g_cfg{};
NetworkEngine*         g_engine     = nullptr;
CFMachPortRef          g_mouse_tap  = nullptr;
CFMachPortRef          g_key_tap    = nullptr;

// g_run_loop: RunLoop 스레드가 CFRunLoopRun 진입 직전에 저장한다.
// 값이 nullptr 이 아니면 "RunLoop 준비 완료", (CFRunLoopRef)-1 은 "setup 실패" 를 의미한다.
// bconn_stop 은 cv 로 준비를 기다린 후 CFRunLoopStop 을 호출한다.
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

// host: watchdog — 이중 타임아웃
//   PEER_WARNING_S : 연결 unhealthy 경고
//   PEER_TIMEOUT_S : peer 초기화 + 원격 모드 강제 종료
static std::thread            g_watchdog_thread;
static std::atomic<bool>      g_watchdog_running{false};
static std::atomic<time_t>    g_last_ping_time{0};
static const int              PEER_WARNING_S = 15;
static const int              PEER_TIMEOUT_S = 30;

// client: 마지막 PONG 수신 시각 (Host 생존 확인)
static std::atomic<time_t>    g_last_pong_time{0};

// start/stop 재진입/경쟁 방지. set_status 의 콜백은 이 lock 을 쓰지 않는다 (lock-free).
static std::mutex             g_lifecycle_mu;

// client 화면 정보 (호스트가 client_handle_move 복귀 판단에 사용)
std::atomic<int32_t> g_client_width{32000};
std::atomic<int32_t> g_client_height{18000};

// 원격 모드 / 클라이언트 상태 (EventTap.cpp, ClientHandler.cpp 에서 extern 참조)
RemoteState g_remote;
ClientState g_client;

// ── 커서 유틸 ─────────────────────────────────────────────────────────

static void show_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("unhide"));
}

// ── 내부 헬퍼 ─────────────────────────────────────────────────────────

void bconn_set_log_callback(BconnLogCallback cb, void* ctx) {
    g_log_cb  = cb;
    g_log_ctx = ctx;
}

/// 상태를 원자적으로 업데이트하고 등록된 콜백을 호출한다.
void set_status(BconnStatus s, const char* msg) {
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

    BCONN_DEBUG(CAT_AUTH, "[클라이언트] HELLO 패킷 생성: 호스트명=%s, 타임스탬프=%lld, PSK 사용=%s",
                pkt.hostname, (long long)pkt.timestamp_s, strlen(g_psk) > 0 ? "예" : "아니오");

    if (strlen(g_psk) > 0) {
        char hn32[32] = {};
        memcpy(hn32, pkt.hostname, sizeof(hn32));
        compute_hmac(g_psk, hn32, pkt.timestamp_s, pkt.hmac);
    }
    // PSK 없으면 hmac은 zero → host에서 open mode로 수락

    g_engine->send_raw_any(&pkt, sizeof(pkt));
    BCONN_DEBUG(CAT_NETWORK, "[클라이언트] HELLO 패킷 전송됨");
}

// ── 공개 API ─────────────────────────────────────────────────────────

bool bconn_start(const BconnConfig* config, BconnStatusCallback cb, void* ctx) {
    std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mu);
    bconn_install_safety_hooks();

    BCONN_INFO(CAT_NETWORK, "bconn_start: 모드=%s, 포트=%d, PSK=%s",
               config->is_host ? "호스트" : "클라이언트",
               config->port,
               strlen(config->psk) > 0 ? "설정됨" : "없음");

    if (g_status.load() != BCONN_STATUS_IDLE) {
        BCONN_ERROR(CAT_NETWORK, "bconn_start 실패: 이미 실행 중 (상태=%d)", (int)g_status.load());
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

    BCONN_INFO(CAT_NETWORK, "bconn 시작: 호스트명=%s, 포트=%d", g_hostname, g_cfg.port);

    if (g_cfg.is_host) {
        refresh_layout(g_cfg.client_edge);

        // host: CLIENT_INFO 패킷으로 클라이언트 화면 크기 수신
        auto on_mouse = [](const MousePacket& pkt) {
            if (pkt.event_type == EVENT_CLIENT_INFO) {
                int32_t w = ((int32_t)(uint16_t)pkt.dy << 16) | (uint16_t)pkt.dx;
                g_client_width.store(w, std::memory_order_relaxed);
                BCONN_DEBUG(CAT_NETWORK, "[호스트] 클라이언트 화면 너비=%d", w);
            } else if (pkt.event_type == EVENT_CLIENT_INFO + 1) {
                int32_t h = ((int32_t)(uint16_t)pkt.dy << 16) | (uint16_t)pkt.dx;
                g_client_height.store(h, std::memory_order_relaxed);
                BCONN_DEBUG(CAT_NETWORK, "[호스트] 클라이언트 화면 높이=%d", h);
            }
        };

        // host: HELLO 수신 → 재전송 공격 방지 → HMAC 검증 → AUTH_ACK 응답
        auto on_hello = [](const HelloPacket& pkt, const sockaddr_in& sender) {
            int64_t now  = (int64_t)time(nullptr);
            int64_t diff = pkt.timestamp_s - now;
            if (diff < -30 || diff > 30) {
                BCONN_ERROR(CAT_AUTH, "[호스트] HELLO 거부됨: 타임스탬프 허용 범위 초과 (diff=%llds, 클라이언트=%s)",
                            (long long)diff, pkt.hostname);
                return;
            }

            BCONN_INFO(CAT_AUTH, "[호스트] HELLO 수신: 클라이언트=%s, diff=%llds, 인증방식=%s",
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
                BCONN_INFO(CAT_AUTH, "[호스트] 인증 성공: %s (%s)", pkt.hostname, ip_str);
                set_status(BCONN_STATUS_CONNECTED, pkt.hostname);
            } else {
                BCONN_ERROR(CAT_AUTH, "[호스트] 인증 실패: %s (%s) PSK 불일치", pkt.hostname, ip_str);
            }

            g_engine->send_auth_result(accepted, g_hostname, sender);
        };

        // host: PING 수신 → 마지막 수신 시각 갱신 + PONG 응답
        auto on_ping = [](const sockaddr_in&) {
            g_last_ping_time.store(time(nullptr), std::memory_order_release);
            BCONN_DEBUG(CAT_AUTH, "[호스트] PING 수신 → PONG 전송");
            g_engine->send_pong();
        };

        g_engine = new NetworkEngine(nullptr, g_cfg.port, on_mouse, nullptr, on_hello, nullptr, on_ping, nullptr);
        g_engine->start();

        // watchdog: 2초 주기로 점검.
        //   >= PEER_WARNING_S : "연결 불안정" 상태 표시 (1회)
        //   >= PEER_TIMEOUT_S : peer 초기화 + 원격 모드 강제 종료
        g_watchdog_running.store(true, std::memory_order_release);
        g_watchdog_thread = std::thread([] {
            bool warned = false;
            while (g_watchdog_running.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!g_watchdog_running.load(std::memory_order_acquire)) break;

                time_t last = g_last_ping_time.load(std::memory_order_acquire);
                if (last == 0) { warned = false; continue; }

                double elapsed = difftime(time(nullptr), last);
                if (elapsed >= PEER_TIMEOUT_S) {
                    BCONN_ERROR(CAT_AUTH, "[호스트] PING 타임아웃 (%.0fs) — peer 초기화", elapsed);
                    host_reset_peer("ping timeout");
                    warned = false;
                } else if (!warned && elapsed >= PEER_WARNING_S) {
                    BCONN_INFO(CAT_AUTH, "[호스트] PING 지연 (%.0fs) — 연결 불안정", elapsed);
                    set_status(BCONN_STATUS_WAITING, "연결 불안정");
                    warned = true;
                }
            }
        });

        // RunLoop 스레드에서 이벤트 탭을 설정해야 CFRunLoopGetCurrent()가 올바른 루프를 반환한다.
        // 호출자(bconn_start)는 setup 및 상태 전환까지 완료를 기다려 반환 후 즉시
        // bconn_get_status 가 의미 있는 값을 반환하도록 한다.
        g_run_loop_thread = std::thread([] {
            setup_event_taps();
            if (g_status.load() == BCONN_STATUS_ERROR) {
                {
                    std::lock_guard<std::mutex> lk(g_run_loop_mu);
                    g_run_loop.store((CFRunLoopRef)-1, std::memory_order_release);
                }
                g_run_loop_cv.notify_all();
                return;
            }

            set_status(BCONN_STATUS_WAITING, "client 접속 대기 중");
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
                BCONN_DEBUG(CAT_NETWORK, "[클라이언트] 패킷 무시됨 (인증 전): 타입=%u", pkt.event_type);
                return;
            }

            BCONN_DEBUG(CAT_EVENT, "[클라이언트] 마우스 패킷: 타입=%u, dx=%d, dy=%d",
                pkt.event_type, (int)pkt.dx, (int)pkt.dy);

            switch (pkt.event_type) {
            case EVENT_SYNC_CURSOR:
                BCONN_INFO(CAT_CURSOR, "원격 모드 전환");
                client_handle_sync(pkt);
                set_status(BCONN_STATUS_REMOTE, "원격 모드");
                break;
            case EVENT_SHOW_CURSOR:
                BCONN_INFO(CAT_CURSOR, "[클라이언트] 커서 표시 (호스트 복귀)");
                if (!g_client.cursor_hidden) {
                    id cls = (id)objc_getClass("NSCursor");
                    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("hide"));
                    g_client.cursor_hidden = true;
                }
                g_client.pos_init = false;
                set_status(BCONN_STATUS_CONNECTED, "연결됨");
                break;
            default:
                if (g_client.pos_init) {
                    client_handle_move(pkt);
                } else {
                    BCONN_DEBUG(CAT_CURSOR, "[클라이언트] 커서 이동 무시됨 (위치 미초기화): 타입=%u",
                                pkt.event_type);
                }
                break;
            }
        };

        auto on_key = [](const KeyPacket& pkt) {
            if (!g_auth_ok.load(std::memory_order_acquire)) return;
            if (g_client.pos_init) client_handle_key(pkt);
        };

        // client: AUTH_ACK 수신 → 인증 상태 업데이트
        auto on_auth_ack = [](const AuthAckPacket& pkt) {
            if (pkt.accepted) {
                g_auth_ok.store(true, std::memory_order_release);
                // 인증 직후 PONG 기준점 설정 — 아직 PONG 수신 전이라도 dead 판정 방지
                g_last_pong_time.store(time(nullptr), std::memory_order_release);
                send_client_info();
                set_status(BCONN_STATUS_CONNECTED, "연결됨");
                BCONN_INFO(CAT_AUTH, "[클라이언트] 인증 성공: 호스트=%s", pkt.host_hostname);
            } else {
                g_auth_ok.store(false, std::memory_order_release);
                set_status(BCONN_STATUS_AUTH_FAILED, "PSK 불일치");
                BCONN_ERROR(CAT_AUTH, "[클라이언트] 인증 실패 (호스트가 거부)");
            }
        };

        // PONG 수신: 호스트가 살아있음을 확인
        auto on_pong = []() {
            g_last_pong_time.store(time(nullptr), std::memory_order_release);
            BCONN_DEBUG(CAT_AUTH, "[클라이언트] PONG 수신");
        };

        g_engine = new NetworkEngine(g_cfg.host_ip, g_cfg.port,
                                     on_mouse, on_key, nullptr, on_auth_ack, nullptr, on_pong);
        g_engine->start();

        BCONN_INFO(CAT_AUTH, "[클라이언트] HELLO 전송: %s:%d (재시도 간격=%us)",
                   g_cfg.host_ip, g_cfg.port, g_reconnect_interval_s);
        send_hello();
        set_status(BCONN_STATUS_WAITING, "인증 대기 중");

        // ping 루프: 인증 전에는 HELLO 재전송, 인증 후에는 PING 전송.
        // PONG 이 pong_timeout_s 이상 오지 않으면 Host 사망으로 판정해 재인증으로 돌아간다.
        g_ping_running.store(true, std::memory_order_release);
        g_ping_thread = std::thread([] {
            const int interval_s = g_reconnect_interval_s;
            const int pong_timeout_s = std::max(30, interval_s * 3);
            while (g_ping_running.load(std::memory_order_acquire)) {
                // 100ms 단위로 쪼개 sleep → 빠른 종료 응답
                for (int i = 0; i < interval_s * 10 && g_ping_running.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!g_ping_running.load(std::memory_order_acquire)) break;

                if (g_auth_ok.load(std::memory_order_acquire)) {
                    BCONN_DEBUG(CAT_AUTH, "[클라이언트] PING 전송");
                    g_engine->send_ping();

                    time_t last = g_last_pong_time.load(std::memory_order_acquire);
                    if (last != 0 && difftime(time(nullptr), last) >= pong_timeout_s) {
                        client_reset_on_host_dead("PONG 타임아웃");
                    }
                } else {
                    BCONN_INFO(CAT_AUTH, "[클라이언트] HELLO 재전송 (미인증 또는 재연결)");
                    send_hello();
                }
            }
        });
    }

    BCONN_INFO(CAT_NETWORK, "bconn_start 완료");
    return true;
}

// ── 비정상 종료 안전 훅 ──────────────────────────────────────────────
//
// crash / atexit 경로에서 이벤트 탭을 비활성화하고 커서-포인터 연동을 복구한다.
// SIGTERM/SIGINT/SIGHUP 은 Swift DispatchSource 가 맡아 정상 bconn_stop 경로로 유도하고,
// 여기서는 catch 불가능/복구 불가능한 crash 계열만 처리한다.

static std::atomic<bool> g_safety_installed{false};

// async-signal-safe 한계 내에서만 호출. CGEvent* 계열은 공식 보장은 없으나
// 단일 Mach RPC 이므로 실무상 안전하고, 실패해도 기존보다 나쁠 것 없다.
static void safety_cleanup() {
    if (g_mouse_tap) CGEventTapEnable(g_mouse_tap, false);
    if (g_key_tap)   CGEventTapEnable(g_key_tap,   false);
    CGAssociateMouseAndMouseCursorPosition(true);

    CGDirectDisplayID main_id = CGMainDisplayID();
    CGRect b = CGDisplayBounds(main_id);
    CGWarpMouseCursorPosition(CGPointMake(b.origin.x + b.size.width * 0.5,
                                          b.origin.y + b.size.height * 0.5));
}

static void crash_signal_handler(int sig) {
    static const char msg[] = "[bconn] crash signal — restoring input\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    safety_cleanup();
    // 원래 동작 복원 후 재전달 → core dump / 디버거
    signal(sig, SIG_DFL);
    raise(sig);
}

void bconn_install_safety_hooks() {
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
    BCONN_INFO(CAT_NETWORK, "safety hook 설치 완료");
}

// host 전용: peer 연결을 초기화하고 대기 상태로 복귀.
// 원격 모드였다면 먼저 로컬 복귀시켜 Host 의 마우스/키보드 소비를 멈춘다.
static void host_reset_peer(const char* reason) {
    BCONN_INFO(CAT_AUTH, "[호스트] peer 연결 초기화: %s", reason);
    force_leave_remote(reason);
    g_engine->clear_peer();
    g_last_ping_time.store(0, std::memory_order_release);
    set_status(BCONN_STATUS_WAITING, "client 접속 대기 중");
}

// client 전용: Host 무응답 감지 시 호출. 인증/원격 상태를 해제해
// 다음 ping 루프에서 HELLO 재전송으로 자동 재연결되도록 한다.
static void client_reset_on_host_dead(const char* reason) {
    BCONN_ERROR(CAT_AUTH, "[클라이언트] 호스트 연결 끊김 감지: %s", reason);
    g_auth_ok.store(false, std::memory_order_release);
    if (g_client.pos_init) {
        g_client.pos_init = false;
        if (g_client.cursor_hidden) {
            id cls = (id)objc_getClass("NSCursor");
            ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("unhide"));
            g_client.cursor_hidden = false;
        }
    }
    g_last_pong_time.store(0, std::memory_order_release);
    set_status(BCONN_STATUS_WAITING, "호스트 응답 없음 — 재연결 중");
}

void bconn_stop() {
    std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mu);
    BCONN_INFO(CAT_NETWORK, "bconn_stop: 연결 종료 시작");

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
    show_cursor();
    g_remote.active.store(false, std::memory_order_release);
    g_remote.virtual_x = g_remote.virtual_y = 0.0;
    g_remote.entry_x = g_remote.entry_y = 0.0;
    g_remote.entry_guard = g_remote.return_guard = 0;
    g_client  = {};
    g_auth_ok.store(false, std::memory_order_release);
    g_last_pong_time.store(0, std::memory_order_release);
    set_status(BCONN_STATUS_IDLE, "중지됨");

    BCONN_INFO(CAT_NETWORK, "bconn_stop 완료");
}

BconnStatus bconn_get_status() {
    return g_status.load();
}

void bconn_force_local() {
    if (g_status.load() == BCONN_STATUS_IDLE) return;
    if (g_cfg.is_host) {
        force_leave_remote("사용자 요청");
    } else {
        client_reset_on_host_dead("사용자 요청");
    }
}

int bconn_get_local_ips(char* out, int out_size) {
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
