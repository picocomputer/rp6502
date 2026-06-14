/*
 * CONTRIBUTING: see kbd.def and str.def
 */

// Editor-only no-op stubs so a lone *.def opens without red squiggles.
#if defined(__INTELLISENSE__) && !defined(BEGIN) && !defined(X)
#define BEGIN(...)
#define KEY(kc, u, s, a, sa, caps)
#define DEAD2(d, b, r)
#define DEAD3(d1, d2, b, r)
#define END()
#define X(name, value)
#define XR(name, value)
#endif
