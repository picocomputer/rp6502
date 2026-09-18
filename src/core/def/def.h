/*
 * CONTRIBUTING: see keyboard.def and str.def
 */

#if defined(__INTELLISENSE__) && !defined(XBEGIN) && !defined(X)
#define XBEGIN(...)
#define XEND()
#define X(name, value)
#define XR(name, value)
#define XKEY(kc, u, s, a, sa, caps)
#define XDEAD(...)
#define XCFG(...)
#define XMON(...)
#endif
