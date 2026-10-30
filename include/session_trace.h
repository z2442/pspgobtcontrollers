#ifndef PSPGOBT_SESSION_TRACE_H
#define PSPGOBT_SESSION_TRACE_H
#ifndef PSPGOBT_SESSION_TRACE
#define PSPGOBT_SESSION_TRACE 0
#endif
enum {
    GOBT_TRACE_BOOT = 1, GOBT_TRACE_EVENT, GOBT_TRACE_EVENT_DONE,
    GOBT_TRACE_CONNECT, GOBT_TRACE_CONNECT_DONE,
    GOBT_TRACE_DISCONNECT, GOBT_TRACE_DISCONNECT_DONE,
    GOBT_TRACE_CLEANUP, GOBT_TRACE_CLEANUP_DONE,
    GOBT_TRACE_SETUP, GOBT_TRACE_SETUP_DONE,
    GOBT_TRACE_INIT, GOBT_TRACE_INIT_DONE,
    GOBT_TRACE_READ, GOBT_TRACE_INPUT, GOBT_TRACE_TICK,
    GOBT_TRACE_POWER, GOBT_TRACE_STOP, GOBT_TRACE_PREPARE,
    GOBT_TRACE_PREPARED, GOBT_TRACE_LINK, GOBT_TRACE_MODE,
    GOBT_TRACE_REGISTER, GOBT_TRACE_REGISTER_DONE, GOBT_TRACE_INSTALL,
    GOBT_TRACE_BIND, GOBT_TRACE_PROVIDER, GOBT_TRACE_CONTEXT,
    GOBT_TRACE_EXIT, GOBT_TRACE_RESET_BLOCKED, GOBT_TRACE_CONNECT_FLAGS
};
#if PSPGOBT_SESSION_TRACE
void gobt_session_trace(unsigned kind, unsigned device, unsigned a, unsigned b, unsigned c);
void gobt_session_trace_start(unsigned firmware);
void gobt_session_trace_stop(void);
#else
static inline void gobt_session_trace(unsigned kind, unsigned device, unsigned a, unsigned b, unsigned c)
{ (void)kind; (void)device; (void)a; (void)b; (void)c; }
static inline void gobt_session_trace_start(unsigned firmware) { (void)firmware; }
static inline void gobt_session_trace_stop(void) { }
#endif
#endif
