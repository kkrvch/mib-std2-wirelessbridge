/* Profile-validated HMI ABI. Calls run exclusively on SalCommunication's Worker.
 * GAL communicates through atomic desired/observed phases; IDLE reset is
 * acknowledged by this owner only after paired removal.
 * The permanent base references below keep these immutable objects alive for
 * the process, including successive sessions. DSI may take/release additional references.
 */
#ifndef WAA_HMI_TARGET_H
#define WAA_HMI_TARGET_H
#define HMI_WAA_ID 0x57414131u
enum { HMI_IDLE, HMI_CONNECTED, HMI_STARTED, HMI_CLOSED };
struct hmi_sp {
    void *p;
    int *refs;
};
struct hmi_device {
    unsigned id;
    struct hmi_sp name;
    unsigned connection;
    struct hmi_sp address;
    unsigned methods;
};
struct hmi_vector {
    struct hmi_sp *begin, *end, *capacity;
};
static unsigned hmi_user_stop;
static unsigned hmi_desired, hmi_observed, hmi_abort, hmi_known, hmi_physical;
static unsigned hmi_published, hmi_strings_initialized;
static void *hmi_listener;
static unsigned hmi_name, hmi_address;
static int hmi_name_refs = 1, hmi_address_refs = 1, hmi_device_refs = 1, hmi_vector_refs = 1,
           hmi_empty_refs = 1;
static struct hmi_device hmi_device = {
    HMI_WAA_ID, {&hmi_name, &hmi_name_refs}, 2, {&hmi_address, &hmi_address_refs}, 8};
static struct hmi_sp hmi_item = {&hmi_device, &hmi_device_refs};
static struct hmi_vector hmi_vector = {&hmi_item, &hmi_item + 1, &hmi_item + 1}, hmi_empty;
static struct hmi_sp hmi_devices = {&hmi_vector, &hmi_vector_refs},
                     hmi_no_devices = {&hmi_empty, &hmi_empty_refs};
_Static_assert(sizeof(struct hmi_device) == SAL_HMI_DEVICE_SIZE,
               "DSI Device size differs from profile");
static unsigned hmi_get(unsigned *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void hmi_set(unsigned *p, unsigned v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void hmi_publish(void *listener, struct hmi_sp *list, unsigned valid) {
    ((void (*)(void *, void *, unsigned))SAL_HMI_DISCOVERED)(listener, list, valid);
}
static void hmi_state(void *object, unsigned state) {
    /* Native SAL enums: connected=3, started=4, disconnected=1, GAL=4.
     * Stock conversion emits DSI state 1/2/0 and method 8 respectively. */
    ((void (*)(void *, unsigned, unsigned, unsigned))SAL_HMI_STATE)(object, HMI_WAA_ID, state, 4);
}
void waa_hmi_discovered(void *listener, struct hmi_sp *list, unsigned valid) {
    struct hmi_vector *v = list ? list->p : 0;
    unsigned present = !v || v->begin != v->end;
    hmi_listener = listener;
    hmi_set(&hmi_physical, present);
    hmi_set(&hmi_known, valid == 1);
    if (hmi_published && !present)
        return; /* keep our sole device until paired removal */
    if (hmi_published && present)
        hmi_set(&hmi_abort, 1);
    hmi_publish(listener, list, valid);
}
void waa_hmi_connect(void *object, unsigned id, unsigned method) {
    if (id == HMI_WAA_ID) {
        if (method != 8)
            hmi_set(&hmi_abort, 1);
        return;
    }
    if (hmi_published)
        hmi_set(&hmi_abort, 1);
    ((void (*)(void *, unsigned, unsigned))SAL_HMI_CONNECT)(object, id, method);
}
void waa_hmi_disconnect(void *object, unsigned id) {
    if (id == HMI_WAA_ID) {
        /* HMI may echo our own disconnection while removing the device. */
        if (hmi_get(&hmi_desired) != HMI_CLOSED) {
            hmi_set(&hmi_user_stop, 1);
            hmi_set(&hmi_abort, 1);
        }
        return;
    }
    ((void (*)(void *, unsigned))SAL_HMI_DISCONNECT)(object, id);
}
void waa_hmi_worker(void *worker) {
    /* runWorker/defaultEventDispatcher owns a loop and does not return each
     * 500 ms. This hook belongs to the virtual handleTimeout slot instead. */
    ((void (*)(void *))A_HANDLETIMEOUT)(worker);
    unsigned desired = hmi_get(&hmi_desired), observed = hmi_get(&hmi_observed);
    if (desired == HMI_IDLE) {
        if (observed == HMI_CLOSED && !hmi_published && !hmi_get(&hmi_physical) &&
            hmi_get(&hmi_known)) {
            hmi_set(&hmi_abort, 0);
            hmi_set(&hmi_observed, HMI_IDLE);
        }
        return;
    }
    if (observed == HMI_CLOSED)
        return;
    if (desired == HMI_CLOSED && !hmi_published) {
        hmi_set(&hmi_observed, HMI_CLOSED);
        return;
    }
    void *object = *(void **)((char *)worker + SAL_HMI_OBJECT);
    if (!object || *(unsigned *)object != SAL_HMI_VTABLE ||
        (char *)object + SAL_HMI_LISTENER != hmi_listener) {
        hmi_set(&hmi_abort, 1);
        return;
    }
    if (desired == HMI_CLOSED) {
        if (hmi_published) {
            hmi_state(object, 1);
            if (!hmi_get(&hmi_physical))
                hmi_publish(hmi_listener, &hmi_no_devices, 1);
            hmi_published = 0;
        }
        hmi_set(&hmi_observed, HMI_CLOSED);
        return;
    }
    if (hmi_get(&hmi_abort) || !hmi_get(&hmi_known) || hmi_get(&hmi_physical)) {
        hmi_set(&hmi_abort, 1);
        return;
    }
    if (observed == HMI_IDLE) {
        if (!hmi_strings_initialized) {
            char allocator = 0;
            ((void (*)(void *, const char *, void *))SAL_STRING_CTOR)(&hmi_name, "Wireless AA",
                                                                      &allocator);
            ((void (*)(void *, const char *, void *))SAL_STRING_CTOR)(&hmi_address, "WIRELESS-AA",
                                                                      &allocator);
            hmi_strings_initialized = 1;
        }
        hmi_published = 1;
        hmi_publish(hmi_listener, &hmi_devices, 1);
        hmi_state(object, 3);
        hmi_set(&hmi_observed, HMI_CONNECTED);
    }
    if (desired == HMI_STARTED && hmi_get(&hmi_observed) == HMI_CONNECTED) {
        hmi_state(object, 4);
        hmi_set(&hmi_observed, HMI_STARTED);
    }
}
#endif
