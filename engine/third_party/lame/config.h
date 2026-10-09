/* config.h for SUBstation's build of LAME's encoder (libmp3lame, nothing else):
   what LAME's configure finds on the platforms SUBstation builds on (64-bit,
   C99, IEEE 754 floats; SSE on x86-64). Not part of LAME's distribution. */
#ifndef SUB_LAME_CONFIG_H
#define SUB_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define PROTOTYPES 1
#define PACKAGE "lame"

/* A faster log, precise enough; quantization with IEEE 754 bit tricks. */
#define USE_FAST_LOG 1
#define TAKEHIRO_IEEE754_HACK 1

/* SSE quantization (vector/xmm_quantize_sub.c) on x86-64. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)
#define HAVE_XMMINTRIN_H 1
#endif

typedef float ieee754_float32_t;
typedef double ieee754_float64_t;
typedef long double ieee854_float80_t;

#endif
