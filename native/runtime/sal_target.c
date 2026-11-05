#define WAA_ROLE_SAL
#include "qnx_runtime.h"
#include "wifi_peer.h"
#include "wifi_runtime.h"
#include "sal_imports.h"
#include "handoff.c"
#include "control_wire.c"
#include "fifo_channel.h"
#define WAA_TCP_ERRNO() (qr_base ? qr_errno() : 0)
#include "../sal-tcp/waa_sal_tcp.c"
#include "gal_reader_guard.h"
#include "hmi_target.h"
static struct wh sal_control;
static struct fifo_channel sal_fifo;
static int sal_initialized, sal_disabled;
static unsigned sal_owner_thread;
static unsigned sal_prepared, sal_rx_seen, sal_monitor;
static uint32_t sal_rx_deadline;
/* Initialize the non-USB connection state through the stock transition methods.
 * TCP replaces USB callback registration and transport opening. */
static int sal_prepare(void) {
    void *gm = waa_tcp_galmgr();
    if (sal_prepared)
        return sal_gal_state(gm) == 1 ? 0 : -1;
    if (gal_guard_install())
        return -1;
    if (sal_gal_state(gm) != 0 || *(volatile unsigned char *)((char *)gm + SAL_GAL_STOPPING))
        return -1;
    *(volatile unsigned char *)((char *)gm + SAL_GAL_BYEBYE) = 0;
    ((void (*)(void *, unsigned, unsigned))SAL_CHANGE_STATE)(gm, 0, 1);
    sal_prepared = 1;
    return 0;
}
static int sal_drain(void *p) {
    (void)p;
    if (g_listen_fd < 0 || usb_active() || waa_tcp_owner() != OWNER_NONE || hmi_get(&hmi_abort))
        return -1;
    if (!hmi_get(&hmi_desired))
        hmi_set(&hmi_desired, HMI_CONNECTED);
    if (hmi_get(&hmi_observed) != HMI_CONNECTED || !waa_tcp_config_ready(waa_tcp_galmgr()))
        return 1;
    if (sal_prepare())
        return -1;
    for (unsigned n = 0; n < 4; n++) {
        int fd = ((fn_accept)A_ACCEPT)(g_listen_fd, 0, 0);
        if (fd < 0) {
            int error = qr_errno();
            /* EINTR leaves the backlog unknown: keep arming and drain again next tick. */
            return error == QR_EAGAIN ? 0 : error == QR_EINTR ? 1 : -1;
        }
        ((fn_i1)A_CLOSE)(fd);
    }
    return 1;
}
static int sal_adopt(void *p, int fd) {
    (void)p;
    sal_monitor = 0;
    uint32_t ip = wifi_socket_peer(fd);
    if (waa_tcp_start_fd(waa_tcp_galmgr(), fd))
        return -2;
    if (ip && sal_fifo.wire.count < WC_QUEUE)
        wc_enqueue(&sal_fifo.wire, (struct wh_message){WH_PEER_IPV4, sal_control.generation, ip});
    return 0;
}
static void sal_close(void *p, int fd) {
    (void)p;
    ((fn_i1)A_CLOSE)(fd);
}
static void sal_stop(void *p) {
    (void)p;
    hmi_set(&hmi_desired, HMI_CLOSED);
    if (waa_tcp_owner() == OWNER_TCP) {
        /* Wake blocked recv/send BEFORE stock joins the reader/writer threads. */
        waa_tcp_abort(0);
        ((fn_ucs)A_UCS)(waa_tcp_galmgr(), 12);
    }
    if (sal_prepared && waa_tcp_owner() == OWNER_NONE && !usb_active() &&
        sal_gal_state(waa_tcp_galmgr()) == 1) {
        void *gm = waa_tcp_galmgr();
        unsigned empty[2] = {0, 0};
        void *cfg = *(void **)((char *)gm + SAL_GAL_CONFIG);
        if (cfg)
            ((void (*)(void *, void *))SAL_CLEAR_CONFIG)(cfg, empty);
        ((void (*)(void *, unsigned, unsigned))SAL_CHANGE_STATE)(gm, 1, 0);
    }
    /* The stock teardown owns the active fd; never close it out from under GAL. */
    if (g_listen_fd >= 0) {
        ((fn_i1)A_CLOSE)(g_listen_fd);
        g_listen_fd = -1;
    }
}
void waa_tcp_pump(void *worker) {
    uint32_t now;
    if (sal_disabled)
        goto stock;
    if (!sal_initialized) {
        if (qr_init() || sal_resolve_imports() || qr_now(&now)) {
            sal_disabled = 1;
            goto stock;
        }
        sal_owner_thread = qr_thread();
        if (qr_mkfifo("/tsd/var/wireless-aa/run/wa2sal.fifo") ||
            qr_mkfifo("/tsd/var/wireless-aa/run/sal2wa.fifo")) {
            sal_disabled = 1;
            goto stock;
        }
        struct wh_ops ops = {fc_send, 0, sal_drain, sal_adopt, sal_close, sal_stop, &sal_fifo};
        wh_init(&sal_control, 1, &ops, 30000);
        if (fc_open(&sal_fifo, &sal_control, 1, now)) {
            sal_disabled = 1;
            goto stock;
        }
        sal_initialized = 1;
    }
    if (qr_thread() != sal_owner_thread || qr_now(&now)) {
        sal_disabled = 1;
        goto stock;
    }
    fc_pump(&sal_fifo, now); /* cancellations before admission */
    if (sal_control.state == WH_SESSION && waa_tcp_owner() == OWNER_TCP) {
        unsigned progress = __atomic_load_n(&g_tcp_rx_progress, __ATOMIC_RELAXED);
        if (!sal_monitor || progress != sal_rx_seen) {
            sal_monitor = 1;
            sal_rx_seen = progress;
            sal_rx_deadline = now + 10000u;
        }
        if (__atomic_load_n(&g_tcp_io_failed, __ATOMIC_ACQUIRE) ||
            (int32_t)(now - sal_rx_deadline) >= 0) {
            unsigned errors = __atomic_load_n(&g_tcp_io_failed, __ATOMIC_ACQUIRE);
            const char *msg = errors & TCP_BUFFER_FAILED
                                  ? "WAA_SAL: GAL buffer allocation failed; stopping safely\n"
                              : errors ? "WAA_SAL: transport I/O error; stopping\n"
                                       : "WAA_SAL: RX stalled 10s; stopping\n";
            unsigned len = 0;
            while (msg[len])
                len++;
            qr_write(1, msg, len);
            wh_cancel(&sal_control, now);
        }
    }
    if (sal_control.cleaned && !sal_control.dirty && sal_control.link && !hmi_get(&hmi_user_stop) &&
        waa_tcp_owner() == OWNER_NONE && g_session_fd < 0 && g_start_busy == START_IDLE &&
        !usb_active() && sal_gal_state(waa_tcp_galmgr()) == 0) {
        if (hmi_get(&hmi_observed) == HMI_CLOSED)
            hmi_set(&hmi_desired, HMI_IDLE);
        if (hmi_get(&hmi_observed) == HMI_IDLE) {
            sal_prepared = sal_monitor = 0;
            wh_rearm(&sal_control);
        }
    }
    if (sal_control.state == WH_WAIT && waa_tcp_owner() == OWNER_NONE && !usb_active() &&
        sal_gal_state(waa_tcp_galmgr()) == 0 && hmi_get(&hmi_known) && !hmi_get(&hmi_physical)) {
        if (g_listen_fd < 0 && waa_tcp_listen(WAA_TCP_PORT))
            goto stock;
        wh_ready(&sal_control, WAA_TCP_PORT, now);
    }
    if (hmi_get(&hmi_abort) && sal_control.used)
        wh_cancel(&sal_control, now);
    wh_tick(&sal_control, now);
    if (sal_control.state == WH_TCP_WAIT) {
        int fd = ((fn_accept)A_ACCEPT)(g_listen_fd, 0, 0);
        if (fd >= 0)
            wh_accept(&sal_control, fd, now);
        else if (!qr_transient(qr_errno()))
            wh_cancel(&sal_control, now);
    }
    if (sal_control.state == WH_SESSION && sal_gal_state(waa_tcp_galmgr()) == 9)
        hmi_set(&hmi_desired, HMI_STARTED);
    if ((sal_control.state == WH_STOPPING || sal_control.state == WH_SESSION) &&
        waa_tcp_owner() == OWNER_NONE && g_session_fd < 0 && g_start_busy == START_IDLE) {
        hmi_set(&hmi_desired, HMI_CLOSED);
        if (sal_gal_state(waa_tcp_galmgr()) == 0 && hmi_get(&hmi_observed) == HMI_CLOSED)
            wh_cleaned(&sal_control);
    }
    fc_flush(&sal_fifo);
stock:
    ((fn_v1)A_HANDLETIMEOUT)(worker);
}

#include "freestanding.c"
