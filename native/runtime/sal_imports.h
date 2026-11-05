#ifndef WAA_SAL_IMPORTS_H
#define WAA_SAL_IMPORTS_H
/* Older SAL builds do not import every socket operation used by the bridge.
 * Resolve only the profile's explicitly missing exports from loaded libraries.
 * Keep the stock Worker running if any export cannot be resolved. */
static int sal_resolve_imports(void) {
#ifdef WAA_RUNTIME_EXPORTS
    void *h = wifi_symbols();
    if (!h)
        return -1;
    int result = 0;
#define SAL_RESOLVE(macro, name)                                                                   \
    macro = (uintptr_t)wifi_symbol(h, name);                                                       \
    if (!macro)                                                                                    \
        result = -1;
    WAA_RUNTIME_EXPORTS(SAL_RESOLVE)
#undef SAL_RESOLVE
    wifi_symbols_close(h);
    return result;
#else
    return 0;
#endif
}
#endif
