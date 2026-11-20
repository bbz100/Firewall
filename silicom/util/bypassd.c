/*
 * bypassd.c - DRAFT, REVIEW ONLY.  Do not deploy on an inline firewall yet.
 *
 * Temporary OPNsense/FreeBSD Silicom bypass supervisor.  This is NOT the
 * planned hzfirewalln service.  All Silicom operations are direct bpmod ioctls;
 * no Silicom command-line utility is launched.
 *
 * Source protocol: bbz100/Firewall/silicom/include/bp_ioctl.h and
 * silicom/util/bpctl_util.c, preserved on silicom-pre-bypassd-20261120.
 *
 * IMPORTANT: Hardware watchdog expiry-to-bypass and pair index mapping must
 * be tested on the exact card before live use.  The default is READ-ONLY.
 */

/* ===================== USER-EDITABLE PARAMETERS ===================== */
#define WATCHDOG_TIMEOUT_MS          32000  /* Silicom hardware watchdog */
#define WATCHDOG_REFRESH_SECONDS        10  /* Manual refresh; auto-reset OFF */
#define HEALTH_CHECK_SECONDS           60  /* WebGUI checks after startup */
#define HEALTH_FAILURE_LIMIT            3  /* Consecutive failed checks */
#define WEBGUI_CONNECT_TIMEOUT_MS    3000  /* Connect timeout */
#define WEBGUI_TOTAL_TIMEOUT_MS      5000  /* Total HTTPS timeout */
#define WEBGUI_ADDRESS "127.0.0.1"
#define WEBGUI_PORT 443
#define EXPECTED_DEVICE_COUNT           4  /* Four ports, two bypass pairs */
#define PAIR_A_DEVICE_INDEX             0  /* MUST verify against driver */
#define PAIR_B_DEVICE_INDEX             2  /* MUST verify against driver */
#define ENABLE_HARDWARE_CHANGES         0  /* 0 = READ-ONLY; 1 = live writes */
#define ENABLE_AUTOMATIC_POWEROFF       0  /* Requires hardware changes = 1 */
/* ==================================================================== */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/time.h>
#include <sys/param.h>
#include <net/pfvar.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "bp_ioctl.h" /* Use the exact preserved Silicom header, unmodified. */

/* Signal handler only requests exit; it never performs ioctls. */
static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int signo) { (void)signo; stop_requested = 1; }

/* Write errors and milestones to stderr for foreground review. */
static void report(const char *message) {
    time_t now = time(NULL);
    fprintf(stderr, "bypassd [%ld]: %s\n", (long)now, message);
    fflush(stderr);
}

/* Return a monotonic time in seconds, unaffected by wall-clock changes. */
static double monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Sleep briefly so SIGTERM can stop the process promptly. */
static void short_sleep(void) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
    (void)nanosleep(&ts, NULL);
}

/*
 * Issue one driver command to a PCI enumeration index.  Silicom's utility
 * uses in_param[1]=0 to select an index from in_param[0], and in_param[2]
 * for setters.  Return the driver's status, or -1 on an ioctl error.
 *
 * NOTE: status=0 is success for setters; getters encode values in status.
 */
static int bp_command(int fd, int index, unsigned long operation, int value,
                      int *status, int *data0) {
    struct bpctl_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.in_param[0] = index;
    cmd.in_param[1] = 0;
    cmd.in_param[2] = value;
    if (ioctl(fd, operation, &cmd) != 0) {
        fprintf(stderr, "bypassd: ioctl index %d failed: %s\n", index, strerror(errno));
        return -1;
    }
    if (status) *status = cmd.status;
    if (data0) *data0 = cmd.data[0];
    return 0;
}

/* A setter succeeds only if the driver returns status zero. */
static bool bp_set(int fd, int index, unsigned long operation, int value) {
    int status = -1;
    return bp_command(fd, index, operation, value, &status, NULL) == 0 && status == 0;
}

/* Read a getter whose value is returned in status (e.g. GET_BYPASS). */
static bool bp_get(int fd, int index, unsigned long operation, int *value) {
    int status = -1;
    if (bp_command(fd, index, operation, 0, &status, NULL) != 0 || status < 0)
        return false;
    *value = status;
    return true;
}

/*
 * Configure a pair controller's persistent power-loss and power-up bypass.
 * Only write a setting if the corresponding readback says it is incorrect.
 */
static bool configure_fail_safe(int fd, int index) {
    int value;
    if (!bp_get(fd, index, IOCTL_TX_MSG(GET_BYPASS_PWOFF), &value)) return false;
    if (value != 1 && !bp_set(fd, index, IOCTL_TX_MSG(SET_BYPASS_PWOFF), 1)) return false;
    if (!bp_get(fd, index, IOCTL_TX_MSG(GET_BYPASS_PWUP), &value)) return false;
    if (value != 1 && !bp_set(fd, index, IOCTL_TX_MSG(SET_BYPASS_PWUP), 1)) return false;
    if (!bp_get(fd, index, IOCTL_TX_MSG(GET_BYPASS_PWOFF), &value) || value != 1) return false;
    if (!bp_get(fd, index, IOCTL_TX_MSG(GET_BYPASS_PWUP), &value) || value != 1) return false;
    return true;
}

/* Switch both hardware pairs to the same state and verify readback. */
static bool set_both_pairs(int fd, int bypass_on) {
    const int indices[2] = { PAIR_A_DEVICE_INDEX, PAIR_B_DEVICE_INDEX };
    bool ok = true;
    for (size_t i = 0; i < 2; ++i)
        if (!bp_set(fd, indices[i], IOCTL_TX_MSG(SET_BYPASS), bypass_on)) ok = false;
    for (size_t i = 0; i < 2; ++i) {
        int actual = -1;
        if (!bp_get(fd, indices[i], IOCTL_TX_MSG(GET_BYPASS), &actual) || actual != bypass_on)
            ok = false;
    }
    return ok;
}

/*
 * Disable kernel-driver auto refresh, set watchdog expiry mode to bypass,
 * and arm the watchdog.  The exact mode code and hardware support MUST be
 * verified before enabling writes; published utility uses bypass=0.
 */
static bool arm_watchdogs(int fd) {
    const int indices[2] = { PAIR_A_DEVICE_INDEX, PAIR_B_DEVICE_INDEX };
    for (size_t i = 0; i < 2; ++i) {
        int idx = indices[i];
        if (!bp_set(fd, idx, IOCTL_TX_MSG(SET_WD_AUTORESET), 0)) return false;
        if (!bp_set(fd, idx, IOCTL_TX_MSG(SET_WD_EXP_MODE), 0)) return false;
        if (!bp_set(fd, idx, IOCTL_TX_MSG(SET_BYPASS_WD), WATCHDOG_TIMEOUT_MS)) return false;
    }
    return true;
}

/* Refresh both timers; a failed refresh is treated as a serious fault. */
static bool refresh_watchdogs(int fd) {
    const int indices[2] = { PAIR_A_DEVICE_INDEX, PAIR_B_DEVICE_INDEX };
    for (size_t i = 0; i < 2; ++i) {
        int status = -1;
        if (bp_command(fd, indices[i], IOCTL_TX_MSG(RESET_BYPASS_WD_TIMER),
                       0, &status, NULL) != 0 || status <= 0) return false;
    }
    return true;
}

/* One deadline bounds TCP, TLS and HTTP; all sockets remain nonblocking. */
static long long webgui_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static bool webgui_wait(int fd, short events, long long deadline) {
    for (;;) {
        long long now = webgui_now_ms();
        if (now < 0 || now >= deadline) return false;
        long long left = deadline - now;
        struct pollfd pfd = { .fd = fd, .events = events };
        int rc = poll(&pfd, 1, (int)(left > INT_MAX ? INT_MAX : left));
        if (rc > 0) return (pfd.revents & events) != 0;
        if (rc == 0 || errno != EINTR) return false;
    }
}
/* GET loopback HTTPS; only HTTP 200 is healthy. No ALPN (HTTP/1.x).
 * Certificate validation is intentionally disabled for this localhost
 * availability probe, not for any remote or authenticated connection.
 */
static bool webgui_healthy(void) {
    static const char request[] =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    bool ok = false;
    int fd = -1;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    long long start = webgui_now_ms();
    if (start < 0) return false;
    long long deadline = start + WEBGUI_TOTAL_TIMEOUT_MS;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) goto done;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto done;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WEBGUI_PORT);
    if (inet_pton(AF_INET, WEBGUI_ADDRESS, &addr.sin_addr) != 1) goto done;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) goto done;
        long long cd = start + WEBGUI_CONNECT_TIMEOUT_MS;
        if (cd > deadline) cd = deadline;
        if (!webgui_wait(fd, POLLOUT, cd)) goto done;
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
            goto done;
    }
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) goto done;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    ssl = SSL_new(ctx);
    if (!ssl || SSL_set_fd(ssl, fd) != 1) goto done;
    for (;;) {
        int rc = SSL_connect(ssl);
        if (rc == 1) break;
        int error = SSL_get_error(ssl, rc);
        short event = error == SSL_ERROR_WANT_READ ? POLLIN :
                      error == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
        if (!event || !webgui_wait(fd, event, deadline)) goto done;
    }
    size_t sent = 0;
    while (sent < sizeof(request) - 1) {
        int rc = SSL_write(ssl, request + sent, (int)(sizeof(request) - 1 - sent));
        if (rc > 0) { sent += (size_t)rc; continue; }
        int error = SSL_get_error(ssl, rc);
        short event = error == SSL_ERROR_WANT_READ ? POLLIN :
                      error == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
        if (!event || !webgui_wait(fd, event, deadline)) goto done;
    }
    char response[256];
    size_t used = 0;
    while (used < sizeof(response) - 1) {
        int rc = SSL_read(ssl, response + used, (int)(sizeof(response) - 1 - used));
        if (rc > 0) {
            used += (size_t)rc;
            response[used] = '\0';
            char *end = strstr(response, "\r\n");
            if (!end) continue;
            *end = '\0';
            int minor = -1, status = -1;
            ok = sscanf(response, "HTTP/1.%d %d", &minor, &status) == 2 &&
                 (minor == 0 || minor == 1) && status == 200;
            goto done;
        }
        int error = SSL_get_error(ssl, rc);
        short event = error == SSL_ERROR_WANT_READ ? POLLIN :
                      error == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
        if (!event || !webgui_wait(fd, event, deadline)) goto done;
    }
done:
    if (ssl) SSL_free(ssl);
    if (ctx) SSL_CTX_free(ctx);
    if (fd >= 0) close(fd);
    return ok;
}

/* PF enabled is a cheap, independent sanity check; not a forwarding test. */
static bool pf_healthy(void) {
    struct pf_status status;
    memset(&status, 0, sizeof(status));
    int fd = open("/dev/pf", O_RDONLY);
    if (fd < 0) return false;
    int rc = ioctl(fd, DIOCGETSTATUS, &status);
    close(fd);
    return rc == 0 && status.running != 0;
}

/* Check PF only at startup; normal PF reloads must not trip bypass. */
static bool health_round(bool startup) {
    bool web_ok = webgui_healthy();
    bool pf_ok = !startup || pf_healthy();
    if (!web_ok) report("WebGUI health check failed");
    if (!pf_ok) report("Startup PF readiness check failed");
    return web_ok && pf_ok;
}

/*
 * On a confirmed failure, command bypass FIRST, then power off the PC.
 * If bypass verification fails, still attempt power-off; power-down bypass
 * is the final fallback.  Never attempt shutdown in the review-only build.
 */
static void failure_action(int fd) {
    report("FAILURE LATCHED: attempting immediate hardware bypass");
#if ENABLE_HARDWARE_CHANGES
    if (!set_both_pairs(fd, 1)) report("WARNING: bypass command/readback failed");
#if ENABLE_AUTOMATIC_POWEROFF
    report("Requesting immediate system power-off");
    sync();
    if (reboot(RB_POWEROFF) != 0) report("ERROR: power-off syscall failed");
#else
    report("Power-off disabled by ENABLE_AUTOMATIC_POWEROFF");
#endif
#else
    (void)fd;
    report("REVIEW MODE: no hardware changes and no power-off");
#endif
}

int main(void) {
    const int indices[2] = { PAIR_A_DEVICE_INDEX, PAIR_B_DEVICE_INDEX };
    int fd = -1;
    struct bpctl_cmd count;
    double next_health, next_refresh;
    int failures = 0;
    bool active = false;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    report("starting; source is a review draft, not a production service");

    fd = open(DEVICE_NODE, O_RDWR);
    if (fd < 0) { perror("open /dev/bpmod"); return EXIT_FAILURE; }
    memset(&count, 0, sizeof(count));
    if (ioctl(fd, IOCTL_TX_MSG(GET_DEV_NUM), &count) != 0 ||
        count.out_param[0] != EXPECTED_DEVICE_COUNT) {
        report("Unexpected Silicom device count; refusing hardware control");
        close(fd);
        return EXIT_FAILURE;
    }

#if ENABLE_HARDWARE_CHANGES
    /* Safety first: do not proceed unless BOTH pairs can be made bypass-safe. */
    for (size_t i = 0; i < 2; ++i) {
        if (!configure_fail_safe(fd, indices[i])) {
            report("Unable to enforce power-up/down bypass");
            failure_action(fd); goto cleanup;
        }
    }
    if (!set_both_pairs(fd, 1)) {
        report("Unable to establish initial bypass");
        failure_action(fd); goto cleanup;
    }
#else
    /* Read-only review: print observed bypass state without changing it. */
    for (size_t i = 0; i < 2; ++i) {
        int bypass = -1;
        if (bp_get(fd, indices[i], IOCTL_TX_MSG(GET_BYPASS), &bypass))
            fprintf(stderr, "bypassd: index %d bypass=%d\n", indices[i], bypass);
    }
    report("REVIEW MODE: watchdog and bypass writes disabled");
#endif

    /* Wait for the first successful health check before entering firewall mode. */
    next_health = monotonic_seconds();
    next_refresh = next_health;
    while (!stop_requested) {
        double now = monotonic_seconds();
        if (now < 0) { report("Monotonic clock error"); failure_action(fd); break; }
        if (now >= next_health) {
            bool healthy = health_round(!active);
            next_health = monotonic_seconds() + HEALTH_CHECK_SECONDS;
            if (healthy) failures = 0;
            else ++failures;
            if (failures >= HEALTH_FAILURE_LIMIT) {
                report("Consecutive health failures exceeded limit");
                failure_action(fd); break;
            }
            if (healthy && !active) {
#if ENABLE_HARDWARE_CHANGES
                if (!arm_watchdogs(fd) || !refresh_watchdogs(fd) ||
                    !set_both_pairs(fd, 0)) {
                    report("Unable to safely enter firewall mode");
                    failure_action(fd); break;
                }
#endif
                active = true;
                next_refresh = monotonic_seconds() + WATCHDOG_REFRESH_SECONDS;
                report("Health checks passed; firewall mode requested (or simulated)");
            }
        }
        now = monotonic_seconds();
        if (active && now >= next_refresh) {
#if ENABLE_HARDWARE_CHANGES
            if (!refresh_watchdogs(fd)) {
                report("Hardware watchdog refresh failed");
                failure_action(fd); break;
            }
#endif
            next_refresh = monotonic_seconds() + WATCHDOG_REFRESH_SECONDS;
        }
        short_sleep();
    }

cleanup:
    /* Normal service stop intentionally does not force a hardware transition.
     * A stopped daemon will cease refreshing and watchdog should expire.
     * A production shutdown/restart policy must be tested separately.
     */
    close(fd);
    report("exiting");
    return EXIT_SUCCESS;
}