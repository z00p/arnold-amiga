/* Branch hints for Emu68 (perf50, 20 Oct 2026). Emu68 translates a forward
 * conditional branch as "not taken" (and a backward one over more than 128
 * bytes); a wrong guess leaves the translated unit through the dispatcher.
 * These hints make gcc put the usual path on the fall-through side. They do
 * not change what the code does. NO_HINTS: plain conditions. */
#ifndef ARN_HINT_H
#define ARN_HINT_H
#if defined(__GNUC__) && !defined(NO_HINTS)
#define ARN_LIKELY(x)   __builtin_expect(!!(x), 1)
#define ARN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define ARN_LIKELY(x)   (x)
#define ARN_UNLIKELY(x) (x)
#endif
#endif
