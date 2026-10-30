#ifndef PSPGOBT_PROBE_LOG_H
#define PSPGOBT_PROBE_LOG_H
#ifndef PSPGOBT_DEBUG
#define PSPGOBT_DEBUG 0
#endif
#if PSPGOBT_DEBUG
void gobt_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#else
/* Keep argument evaluation and format checking in release builds. Some
 * legacy call sites pass initialization results here; a variadic macro
 * that discards its arguments would silently remove those operations. */
static inline __attribute__((format(printf, 1, 2))) void gobt_log(const char *format, ...)
{
    (void)format;
}
#endif
#endif
