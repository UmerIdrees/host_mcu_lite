#ifndef HML_CONFIG_H
#define HML_CONFIG_H

/*
 * Top-level build-time config defaults (may be overridden by CMake cache). 
 * Core code must not depend on a particular port; only include/ are public.
 */

#ifndef HML_DEFAULT_MTU
#define HML_DEFAULT_MTU 1500u
#endif

#ifndef HML_DEFAULT_MAX_FRAME
#define HML_DEFAULT_MAX_FRAME (1u<<20)
#endif

#ifndef HML_DEFAULT_FRAG_PAYLOAD
#define HML_DEFAULT_FRAG_PAYLOAD 1428u
#endif

/* Written p99 EOF-latency budget for the T0 loopback CI path, in microseconds.
 * Derived from measured loopback runs on a loaded VM; see docs/latency.md.
 * It is a smoke-test threshold, NOT an MCU or dual-machine product target.
 * Override at build time with -DHML_LATENCY_BUDGET_US=<n> (0 = no budget). */
#ifndef HML_LATENCY_BUDGET_US
#define HML_LATENCY_BUDGET_US 40000u
#endif

#endif /* HML_CONFIG_H */