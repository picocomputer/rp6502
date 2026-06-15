/*
 * CONTRIBUTING: see kbd.def and str.def
 */

// Editor-only no-op stubs so a lone *.def opens without red squiggles.
#if defined(__INTELLISENSE__) && !defined(XBEGIN) && !defined(X)
#define XBEGIN(...)
#define XEND()
#define X(name, value)
#define XR(name, value)
#define XKEY(kc, u, s, a, sa, caps)
#define XDEAD(...)
#endif
