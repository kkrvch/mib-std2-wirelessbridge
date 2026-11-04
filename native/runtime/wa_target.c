#define WAA_ROLE_WA
#define WAA_MODE 1
#include "qnx_runtime.h"
#include "handoff.c"
#include "control_wire.c"
#include "wifi_peer.h"
#include "wifi_runtime.h"
static void wa_peer_message(struct wh_message);
#define WAA_PEER_MESSAGE wa_peer_message
#include "fifo_channel.h"
#include "peer_selection.h"
static void wa_rx_cb(void *, uint32_t, uint32_t, const void *);
static void wa_flow_cb(void *, uint32_t, uint32_t, const void *);
static void wa_error_cb(void *, uint32_t, uint32_t, const void *);
static void wa_uuid_cb(void *, uint32_t, uint32_t, const void *);
#define WAA_UUID_CALLBACK wa_uuid_cb
#define WAA_SPP_RX_CALLBACK wa_rx_cb
#define WAA_SPP_FLOW_CALLBACK wa_flow_cb
#define WAA_SPP_ERROR_CALLBACK wa_error_cb
#include "../waa-ext/waa_ext.c"
#include "../waa-spp/waa_spp.c"
#include "../aa-bootstrap/aa_bootstrap.c"
#include "../waa-spp/aa_boot_sm.c"
#include "../waa-ext/lifecycle/rx_capture.c"
#include "../waa-spp/lifecycle/spp_lifecycle.c"
#include "../waa-spp/lifecycle/lifecycle_queue.c"
#include "freestanding.c"
/* Only enablement is packaged. Peer selection and AP data are live. */
struct wa_settings {
    uint32_t magic;
};
struct wa_settings waa_settings __attribute__((section(".data"))) = {0};
#include "ap_runtime.h"
static struct aa_wifi_cfg wa_wifi;
static struct waa_peer_selection wa_selected;
static struct wh wa_control;
static struct fifo_channel wa_fifo;
static struct spp_lcq wa_events;
static struct aa_boot wa_boot;
static void *wa_handler;
static unsigned wa_tid, wa_initialized, wa_app_init, wa_uuid_sent, wa_uuid_ok, wa_instance;
static unsigned wa_next_channel, wa_spp_disconnect_pending;
static unsigned wa_uuid_pending, wa_uuid_target, wa_uuid_known;
static uint32_t wa_uuid_probe_after, wa_uuid_probe_delay;
static uint32_t wa_retry_after, wa_ap_check_after;
static unsigned wa_ap_waiting;
static uint32_t wa_ap_probe_after, wa_ap_probe_delay;
static unsigned wa_depth, wa_fault, wa_control_started, wa_phone_seen;
static uint32_t wa_now, wa_startup_deadline, wa_uuid_deadline;
static uint32_t wa_wifi_ip, wa_wifi_generation, wa_wifi_probe_after, wa_wifi_down;
static uint8_t wa_wifi_mac[6];
static unsigned wa_wifi_bound;
static uint32_t wa_wifi_probe_delay;
static void wa_peer_message(struct wh_message m) {
    if (!wa_control.used || m.generation != wa_control.generation)
        return;
    wa_wifi_ip = m.value;
    wa_wifi_generation = m.generation;
    wa_wifi_bound = 0;
    wa_wifi_down = 0;
    wa_wifi_probe_after = wa_now;
    wa_wifi_probe_delay = 1000u;
}
static int wa_peer(const struct spp_lc_event *ev) {
    return waa_settings.magic == 0x57414131 &&
           waa_peer_select(&wa_selected, ev->addr, ev->kind == SPP_LC_CONNECTED, ev->function,
                           ev->status, ev->instance);
}
static int wa_same_thread(void) { return wa_initialized && qr_thread() == wa_tid; }
static int wa_guard(void) {
    if (wa_same_thread())
        return 1;
    __atomic_store_n(&wa_fault, 1, __ATOMIC_SEQ_CST);
    return 0;
}
/* The stock authorization indication has a six-byte network-order address
 * and a four-byte network-order service mask. Reject only SPP (bit 0); keep
 * stock authorization for calls, audio, contacts and messages. */
void waa_wa_authorise(void *h, uint32_t op, uint32_t count, const void *node) {
    if (h == wa_handler && wa_same_thread() && waa_settings.magic == 0x57414131 && op == 0x0402u &&
        count == 2 && node && PA_SIZE(node, 0) == 6 && PA_SIZE(node, 1) == 4 && PA_PTR(node, 0) &&
        PA_PTR(node, 1)) {
        const uint8_t *service = PA_PTR(node, 1);
        if (!service[0] && !service[1] && !service[2] && service[3] == 1 &&
            (!ap_enabled() || !wa_uuid_ok || wa_uuid_pending)) {
            uint8_t address[6], accept = 0;
            uint32_t function = 1;
            /* Type 12 serializes the native little-endian Bluetooth address. */
            for (unsigned i = 0; i < 6; i++)
                address[i] = PA_PTR(node, 0)[5 - i];
            struct hif_param params[3] = {{address, 6, 0}, {&function, 4, 0}, {&accept, 1, 0}};
            BuildAndQueueMessage(HL_OF(h), 0x0402u, 3u, params, 0);
            Log(HL_OF(h), 1, "WAA_E2E: AA unavailable; SPP authorization refused\n");
            return;
        }
    }
    ((void (*)(void *, uint32_t, uint32_t, const void *))WA_AUTHORISE_STOCK)(h, op, count, node);
}
void waa_wa_register_authorise(void *hl, uint32_t op, void *callback) {
    RegisterHandler(hl, op, op == 0x0402u ? (void *)waa_wa_authorise : callback);
}
static void wa_disconnect_spp(void) {
    if (!wa_phone_seen || wa_spp_disconnect_pending)
        return;
    uint32_t function = 1;
    uint8_t instance = (uint8_t)wa_instance;
    struct hif_param params[2] = {{&function, 4, 0}, {&instance, 1, 0}};
    wa_spp_disconnect_pending = 1;
    BuildAndQueueMessage(HL_OF(wa_handler), 0x0309u, 2u, params, 0);
    Log(HL_OF(wa_handler), 1, "WAA_E2E: AA service changing; disconnecting SPP only\n");
}
static void wa_rx_cb(void *h, uint32_t op, uint32_t count, const void *node) {
    if (wa_guard())
        waa_spp_rx_cb(h, op, count, node);
}
static void wa_flow_cb(void *h, uint32_t op, uint32_t count, const void *node) {
    if (wa_guard())
        waa_spp_flow_cb(h, op, count, node);
}
static void wa_error_cb(void *h, uint32_t op, uint32_t count, const void *node) {
    if (wa_guard())
        waa_spp_err_cb(h, op, count, node);
}
static void wa_uuid_cb(void *h, uint32_t op, uint32_t count, const void *node) {
    if (!wa_guard())
        return;
    waa_uuid_result_cb(h, op, count, node);
    if (!wa_uuid_pending)
        return;
    if (count != 2 || !node || PA_SIZE(node, 0) != 1 || PA_SIZE(node, 1) != 1 || !PA_PTR(node, 0) ||
        !PA_PTR(node, 1) || *PA_PTR(node, 0) != 0) {
        __atomic_store_n(&wa_fault, 1, __ATOMIC_SEQ_CST);
        return;
    }
    unsigned status = *PA_PTR(node, 1);
    wa_uuid_pending = 0;
    if (!status) {
        wa_uuid_known = 1;
        wa_uuid_ok = wa_uuid_target;
        wa_uuid_probe_delay = 4000u;
        Log(HL_OF(wa_handler), 1,
            wa_uuid_ok ? "WAA_E2E: AA Bluetooth service enabled\n"
                       : "WAA_E2E: AA Bluetooth service withdrawn\n");
    } else if (status == 7) {
        /* The radio may still be draining a just-disconnected SPP channel. */
        wa_uuid_probe_after = wa_now + 2000u;
    } else
        __atomic_store_n(&wa_fault, 1, __ATOMIC_SEQ_CST);
}
static void wa_service_policy(void) {
    if (!wa_app_init || wa_uuid_pending || wa_control.state != WH_READY_STATE ||
        (int32_t)(wa_now - wa_uuid_probe_after) < 0)
        return;
    unsigned enabled = ap_enabled();
    if (!wa_uuid_probe_delay)
        wa_uuid_probe_delay = 1000u;
    wa_uuid_probe_after = wa_now + wa_uuid_probe_delay;
    if (wa_uuid_probe_delay < 4000u)
        wa_uuid_probe_delay *= 2u;
    if (wa_uuid_known && enabled == wa_uuid_ok)
        return;
    /* UUID changes require a disconnected SPP instance. Other Bluetooth
     * services keep their stock service records and authorization. */
    if (wa_phone_seen) {
        wa_disconnect_spp();
        return;
    }
    wa_uuid_sent = wa_uuid_pending = 1;
    wa_uuid_target = enabled;
    wa_uuid_deadline = wa_now + 15000u;
    if (waa_ext_send_uuid_set(HL_OF(wa_handler), 0, UUID_16, SPP_1101, enabled ? UUID_128 : UUID_16,
                              enabled ? WAA_UUID128 : SPP_1101))
        __atomic_store_n(&wa_fault, 1, __ATOMIC_SEQ_CST);
}
static int wa_ap_ready(void) {
    if ((int32_t)(wa_now - wa_ap_probe_after) < 0)
        return 0;
    if (!wa_ap_probe_delay)
        wa_ap_probe_delay = 1000u;
    wa_ap_probe_after = wa_now + wa_ap_probe_delay;
    if (ap_refresh(&wa_wifi)) {
        if (wa_ap_probe_delay < 4000u)
            wa_ap_probe_delay *= 2u;
        if (!wa_ap_waiting)
            Log(HL_OF(wa_handler), 1, "WAA_E2E: waiting for a usable access point\n");
        wa_ap_waiting = 1;
        return 0;
    }
    if (wa_ap_waiting)
        Log(HL_OF(wa_handler), 1, "WAA_E2E: access point ready; continuing connection\n");
    wa_ap_waiting = 0;
    wa_ap_probe_delay = 1000u;
    return 1;
}
static int wa_begin(void *p, uint16_t port) {
    (void)p;
    if (!wa_handler || !wa_uuid_ok || !wa_phone_seen || waa_settings.magic != 0x57414131)
        return -1;
    if (ap_refresh(&wa_wifi)) {
        Log(HL_OF(wa_handler), 1, "WAA_E2E: AP configuration unavailable; bootstrap refused\n");
        return -1;
    }
    if (aa_boot_init(&wa_boot, HL_OF(wa_handler), wa_instance, &wa_wifi, 15000, 30000))
        return -1;
    Log(HL_OF(wa_handler), 1, "WAA_E2E: bootstrap start instance=%u port=%u\n", wa_instance,
        (unsigned)port);
    return aa_boot_start(&wa_boot, wa_now, port) == AA_BOOT_START_SENT ? 0 : -1;
}
static void wa_stop(void *p) {
    (void)p;
    wa_retry_after = wa_now + 2000u;
    /* Keep an actually connected bootstrap channel for Wi-Fi-only loss. A
     * radio DISCONNECT clears phone_seen independently and closes the shim. */
    if (wa_phone_seen)
        wa_next_channel = 1;
    if (!wa_next_channel) {
        wa_phone_seen = 0;
        wa_control.phone = 0;
    }
}
static void wa_reset_ap_probe(void) {
    wa_ap_waiting = 0;
    wa_ap_probe_after = wa_now;
    wa_ap_probe_delay = 1000u;
}
static void wa_reset_radio(void) {
    wa_app_init = wa_uuid_sent = wa_uuid_ok = wa_phone_seen = wa_next_channel = 0;
    wa_spp_disconnect_pending = wa_uuid_pending = wa_uuid_target = wa_uuid_known = 0;
    wa_uuid_probe_after = wa_now;
    wa_uuid_probe_delay = 1000u;
    wa_reset_ap_probe();
    spp_lcq_reset(&wa_events);
    wa_selected = (struct waa_peer_selection){0};
}
/* Both stock HifHandler destructors call Log before freeing HifLib. Replacing
 * those two calls makes the process-lifetime owner pump release its borrowed
 * pointer before the adapter is destroyed. QNX VM reproduced a use-after-free
 * here when ordinary Bluetooth shutdown removed the adapter. */
void waa_wa_destroy_log(void *h, unsigned level, const char *message) {
    Log(h, level, message); /* preserve the exact stock destructor log */
    if (h != wa_handler)
        return;
    Log(HL_OF(h), 1, "WAA_E2E: adapter detached; releasing borrowed HIF\n");
    wa_handler = 0;
    wa_reset_radio();
    if (wa_control_started && wa_control.used) {
        if (!qr_now(&wa_now)) {
            wh_cancel(&wa_control, wa_now);
            fc_pump(&wa_fifo, wa_now);
        }
    }
}
void waa_wa_register(void *h) {
    wa_handler = h;
    /* Stock RegisterCallbacks is called by the entry trampoline exactly once. */
    if (!wa_initialized) {
        if (qr_init())
            return;
        wa_tid = qr_thread();
        if (qr_now(&wa_now))
            return;
        wa_startup_deadline = wa_now + 90000;
        wa_initialized = 1;
    }
    wa_reset_radio();
    waa_ext_register(h);
    waa_spp_register(h);
    Log(HL_OF(h), 1, "WAA_E2E: registered owner=%u; awaiting owner pump and config\n", wa_tid);
}
__attribute__((naked)) void waa_wa_entry(void) {
    __asm__ volatile("push {r0-r3,r12,lr}\nldr r12,=" WAA_STRING(
        WA_REGISTER_CALLBACKS) "\nblx r12\nldr r0,[sp]\nbl "
                               "waa_wa_register\npop {r0-r3,r12,lr}\nbx lr\n");
}
typedef uint16_t *(*wa_parse)(uint16_t *, void *, const uint8_t *, uint32_t, uint32_t);
uint16_t *waa_wa_rx(uint16_t *out, void *hl, const uint8_t *buf, uint32_t len, uint32_t flag) {
    struct spp_lc_event event;
    int captured = -1;
    unsigned prefix = 0;
    int owned = wa_handler && wa_same_thread() && HL_OF(wa_handler) == hl;
    if (owned) {
        wa_depth++;
        if (wa_depth != 1)
            __atomic_store_n(&wa_fault, 1, __ATOMIC_SEQ_CST);
        if (!flag && wa_depth == 1) {
            prefix = ((uint8_t *)hl)[WA_ASIP_PREFIX] ? 3 : 0;
            captured = waa_lifecycle_frame(buf, len, prefix != 0, &event);
            if (wa_wifi_bound && wa_control.state == WH_SESSION &&
                wa_wifi_generation == wa_control.generation &&
                wifi_station_down(buf, len, prefix != 0, wa_wifi_mac))
                wa_wifi_down = wa_control.generation;
            if (buf && len >= prefix + 4 && buf[prefix] == 1 && buf[prefix + 1] == 4)
                wa_app_init = 1;
        }
    } else if (wa_initialized)
        __atomic_store_n(&wa_fault, 1, __ATOMIC_SEQ_CST);
    uint16_t *result = ((wa_parse)WA_PARSE)(out, hl, buf, len, flag);
    if (owned && wa_handler && HL_OF(wa_handler) == hl && captured == SPP_LC_OK) {
        int peer = wa_peer(&event);
        /* Unrelated HFP/A2DP/PBAP events must not exhaust our eight SPP slots. */
        if (peer && event.function == SPP_LC_FUNCTION_SPP && event.instance <= 1 &&
            (event.kind == SPP_LC_DISCONNECTED || event.status == 0 || event.status == 0x80))
            spp_lcq_push(&wa_events, &event);
    }
    if (owned)
        wa_depth--;
    return result;
}
void waa_wa_pump(void) {
    if (!wa_initialized || !wa_guard() || wa_depth)
        return;
    if (qr_now(&wa_now))
        return;
    if (!wa_handler) {
        /* FIFO heartbeat/cancellation belongs to the process, not the radio object.
         * Before the first attempt, keep READY for a later normal radio restart. */
        if (wa_control_started) {
            fc_pump(&wa_fifo, wa_now);
            wh_tick(&wa_control, wa_now);
        }
        return;
    }
    if (!wa_control_started) {
        if (waa_settings.magic != 0x57414131)
            return;
        struct wh_ops ops = {fc_send, wa_begin, 0, 0, 0, wa_stop, &wa_fifo};
        wh_init(&wa_control, 0, &ops, 30000);
        if (fc_open(&wa_fifo, &wa_control, 0, wa_now)) {
            if ((int32_t)(wa_now - wa_startup_deadline) >= 0)
                __atomic_store_n(&wa_fault, 1, __ATOMIC_SEQ_CST);
            return;
        }
        wa_control_started = 1;
    }
    if (__atomic_load_n(&wa_fault, __ATOMIC_SEQ_CST) || wa_events.poisoned) {
        wh_cancel(&wa_control, wa_now);
        fc_pump(&wa_fifo, wa_now);
        return;
    }
    fc_pump(&wa_fifo, wa_now);
    /* A stopped AP can leave wlan1 UP/RUNNING. Cancel an in-flight attempt
     * or session when NetMgr disables AP, even without a station-down event. */
    if ((wa_phone_seen ||
         (wa_control.used && wa_control.state >= WH_ARMING && wa_control.state <= WH_SESSION)) &&
        (int32_t)(wa_now - wa_ap_check_after) >= 0) {
        wa_ap_check_after = wa_now + 1000u;
        if (!ap_enabled()) {
            wa_disconnect_spp();
            if (wa_control.used && wa_control.state >= WH_ARMING &&
                wa_control.state <= WH_SESSION) {
                Log(HL_OF(wa_handler), 1, "WAA_E2E: AP disabled or mode unavailable; stopping\n");
                wh_cancel(&wa_control, wa_now);
                fc_flush(&wa_fifo);
            }
        }
    }
    if (wa_wifi_down && wa_control.state == WH_SESSION && wa_wifi_down == wa_control.generation) {
        wa_wifi_down = 0;
        Log(HL_OF(wa_handler), 1, "WAA_E2E: active Wi-Fi peer departed; stopping\n");
        wh_cancel(&wa_control, wa_now);
        fc_flush(&wa_fifo);
    }
    if (wa_control.state == WH_SESSION && wa_wifi_ip && !wa_wifi_bound &&
        wa_wifi_generation == wa_control.generation &&
        (int32_t)(wa_now - wa_wifi_probe_after) >= 0) {
        wa_wifi_probe_after = wa_now + wa_wifi_probe_delay;
        if (wa_wifi_probe_delay < 8000u)
            wa_wifi_probe_delay *= 2u;
        if (!wifi_resolve_peer(wa_wifi_ip, wa_wifi_mac)) {
            wa_wifi_bound = 1;
            Log(HL_OF(wa_handler), 1, "WAA_E2E: active Wi-Fi peer bound\n");
        }
    }
    wa_service_policy();
    if (wa_uuid_pending && (int32_t)(wa_now - wa_uuid_deadline) >= 0) {
        __atomic_store_n(&wa_fault, 1, __ATOMIC_SEQ_CST);
        wh_cancel(&wa_control, wa_now);
        return;
    }
    struct spp_lc_event ev;
    /* A successful inbound connect may precede the UUID CFM on the same HIF.
     * Keep it queued until confirmation; do not silently discard a live channel. */
    while ((wa_uuid_known || (wa_uuid_sent && !wa_uuid_pending)) &&
           spp_lcq_pop(&wa_events, &ev) == 0) {
        if (ev.kind == SPP_LC_CONNECTED && spp_lc_connect_filter(&ev, wa_selected.address) == 0 &&
            !wa_phone_seen) {
            /* A new radio CONNECT, serialized after DISCONNECT, is the channel
             * boundary. Old callbacks while DRAINING were discarded by the SPP layer. */
            waa_spp_mark_drained(HL_OF(wa_handler));
            wa_instance = ev.instance;
            wa_spp_disconnect_pending = 0;
            if (!waa_spp_open(HL_OF(wa_handler), wa_instance)) {
                wa_phone_seen = 1;
                aa_boot_rechannel(&wa_boot);
                if (wa_control.used) {
                    wa_next_channel = 1;
                    /* SPP is a bootstrap channel, not the accepted TCP session's
                     * liveness signal. Reconnecting it during TLS must not cancel GAL.
                     * Keep it for rebootstrap after SAL confirms a real TCP loss. */
                }
            }
        } else if (ev.kind == SPP_LC_DISCONNECTED && ev.function == SPP_LC_FUNCTION_SPP &&
                   wa_phone_seen && ev.instance == wa_instance) {
            if (waa_peer_equal(ev.addr, wa_selected.address)) {
                waa_spp_close(HL_OF(wa_handler), wa_instance);
                wa_phone_seen = wa_next_channel = 0;
                wa_spp_disconnect_pending = 0;
                wa_reset_ap_probe();
                wa_control.phone = 0;
                /* SPP is bootstrap only: TCP can remain healthy without Bluetooth.
                 * After READY, used=0: there is no live generation to cancel. A late
                 * disconnect in the retry cooldown must not strand WA in STOPPING. */
                if (wa_control.used && !wa_control.accepted)
                    wh_cancel(&wa_control, wa_now);
            }
        }
    }
    /* SAL prepares HMI/GAL on WH_START. Check AP first, keeping heartbeats
     * and Bluetooth dispatch alive without starting a new generation. */
    if (wa_control.state == WH_READY_STATE && wa_phone_seen && wa_uuid_ok && !wa_uuid_pending &&
        !wa_spp_disconnect_pending && (int32_t)(wa_now - wa_retry_after) >= 0 && wa_ap_ready()) {
        if (wa_next_channel) {
            /* No new generation has transmitted yet. Discard the preceding bootstrap
             * byte queue before sending a fresh WifiStartRequest on this channel. */
            if (waa_spp_open(HL_OF(wa_handler), wa_instance))
                return;
            wa_next_channel = 0;
        }
        wh_phone(&wa_control, wa_now);
    }
    wh_tick(&wa_control, wa_now);
    if (wa_control.state == WH_BOOT) {
        int state = aa_boot_poll(&wa_boot, wa_now);
        if (state == AA_BOOT_WAIT_TCP)
            wh_boot_done(&wa_control, wa_now);
        else if (state == AA_BOOT_ERROR || state == AA_BOOT_TIMEOUT)
            wh_cancel(&wa_control, wa_now);
    }
    if (wa_control.state == WH_SESSION && aa_boot_state(&wa_boot) == AA_BOOT_WAIT_TCP)
        aa_boot_tcp_accepted(&wa_boot);
    /* The phone keeps the SPP channel after bootstrap; drain it and answer keepalives. */
    if (wa_phone_seen && wa_control.state != WH_BOOT)
        aa_boot_serve(&wa_boot);
    if (wa_phone_seen)
        waa_spp_tick(HL_OF(wa_handler), wa_instance);
    fc_flush(&wa_fifo);
    if (wa_control.state == WH_READY_STATE && !wa_phone_seen && !wa_events.count)
        wa_selected.selected = 0;
}
/* Adapter-thread wait wrapper. Stock ipc::wait may return NULL on timeout,
 * but its caller requires a real Transfer. Handle timeouts here without fabricating
 * events. The separate main-thread wait remains unchanged. */
void *waa_wa_wait(unsigned timeout) {
    typedef void *(*wait_fn)(unsigned);
    if (timeout != 0xffffffffu)
        return ((wait_fn)WA_WAIT)(timeout);
    for (;;) {
        waa_wa_pump();
        void *event = ((wait_fn)WA_WAIT)(50);
        if (event)
            return event;
    }
}
