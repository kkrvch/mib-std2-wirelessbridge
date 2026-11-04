/* Protocol field definitions: see THIRD_PARTY_NOTICES.md.
 * Bounded wire encoder/decoder; no generated protobuf code or runtime. */
#include "aa_bootstrap.h"
struct writer {
    unsigned char *p;
    unsigned int used, cap, failed;
};
static void byte(struct writer *w, unsigned int value) {
    if (w->used >= w->cap) {
        w->failed = 1;
        return;
    }
    w->p[w->used++] = (unsigned char)value;
}
static void varint(struct writer *w, unsigned int value) {
    while (value >= 128) {
        byte(w, (value & 127) | 128);
        value >>= 7;
    }
    byte(w, value);
}
static void string_field(struct writer *w, unsigned int field, struct aa_bytes s) {
    unsigned int i;
    if ((!s.data && s.size) || s.size > 65535u) {
        w->failed = 1;
        return;
    }
    varint(w, (field << 3) | 2);
    varint(w, s.size);
    for (i = 0; i < s.size && !w->failed; ++i)
        byte(w, s.data[i]);
}
static unsigned int finish(struct writer *w, unsigned int id) {
    unsigned int payload;
    if (w->failed || w->used < 4 || w->used - 4 > 65535u)
        return 0;
    payload = w->used - 4;
    w->p[0] = (unsigned char)(payload >> 8);
    w->p[1] = (unsigned char)payload;
    w->p[2] = (unsigned char)(id >> 8);
    w->p[3] = (unsigned char)id;
    return w->used;
}
unsigned int aa_start_frame(unsigned char *out, unsigned int capacity, struct aa_bytes ip,
                            unsigned int port) {
    struct writer w = {out, 4, capacity, 0};
    if (!out || capacity < 4 || !ip.size || !port || port > 65535u)
        return 0;
    string_field(&w, 1, ip);
    byte(&w, 0x10);
    varint(&w, port);
    return finish(&w, 1);
}
unsigned int aa_wifi_frame(unsigned char *out, unsigned int capacity, const struct aa_wifi *wifi) {
    struct writer w = {out, 4, capacity, 0};
    if (!out || capacity < 4 || !wifi || !wifi->ssid.size || wifi->ssid.size > 32 ||
        wifi->ap_type > 1)
        return 0;
    switch (wifi->security) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 8:
    case 12:
    case 20:
    case 24:
    case 28:
        break;
    default:
        return 0;
    }
    string_field(&w, 1, wifi->ssid);
    string_field(&w, 2, wifi->key);
    string_field(&w, 3, wifi->bssid);
    byte(&w, 0x20);
    varint(&w, wifi->security);
    byte(&w, 0x28);
    varint(&w, wifi->ap_type);
    return finish(&w, 3);
}
unsigned int aa_raw_frame(unsigned char *out, unsigned int capacity, unsigned int id,
                          const unsigned char *payload, unsigned int size) {
    struct writer w = {out, 4, capacity, 0};
    unsigned int i;
    if (!out || capacity < 4 || id > 65535u || (!payload && size))
        return 0;
    for (i = 0; i < size && !w.failed; ++i)
        byte(&w, payload[i]);
    return finish(&w, id);
}
void aa_rx_reset(struct aa_rx *rx) {
    rx->header_used = rx->payload_used = rx->length = rx->id = rx->failed = 0;
}
int aa_rx_feed(struct aa_rx *rx, const unsigned char *data, unsigned int size, aa_on_frame callback,
               void *context) {
    unsigned int i = 0;
    if (!rx)
        return -1;
    if (rx->failed || !callback || (!data && size)) {
        rx->failed = 1;
        return -1;
    }
    while (i < size) {
        if (rx->header_used < 4) {
            rx->header[rx->header_used++] = data[i++];
            if (rx->header_used < 4)
                continue;
            rx->length = ((unsigned int)rx->header[0] << 8) | rx->header[1];
            rx->id = ((unsigned int)rx->header[2] << 8) | rx->header[3];
            if (rx->length > AA_RX_LIMIT) {
                rx->failed = 1;
                return -1;
            }
        } else {
            rx->payload[rx->payload_used++] = data[i++];
        }
        if (rx->payload_used == rx->length) {
            if (callback(context, rx->id, rx->payload, rx->length)) {
                rx->failed = 1;
                return -1;
            }
            rx->header_used = rx->payload_used = 0;
        }
    }
    return 0;
}
