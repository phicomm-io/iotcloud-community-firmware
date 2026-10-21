/* Vendored algorithm provenance (not a libc replacement):
 * newlib-cygwin revision94e3a561d053ca1b00f16d81d4ade2884ea45b54,
 * declared base of GNU Arm Embedded5.4-2016q2 (toolchain also has backports).
 * Compatibility is tested against its actual nano binary, not inferred from
 * a byte-identical rebuild. Original source SHA256 values:
 * stdlib/mprec.c f33c8bff4e6bff19c86d0188447b378b0a8d1e963a5020bcfda0037835e302ac
 * stdlib/mprec.h e723a342751935c2ca667a7c1e66695a96e01c1943d8a31de523305219a559c6
 * stdlib/dtoa.c bd21eeb94d462f23aa7ce59c525a2d7d903aa43ef6fd31e9386cceea201fe906
 * stdlib/strtod.c 8dbd367d226be0551420eee2fb6b40cf7b230a37593e331bcf8cbb2e37c93bd8
 * stdlib/gdtoa.h 2d9c9a1377a57877b38a242b2fd6ca24ef69d2ef73da78727b1c14efa9ec7163
 * stdlib/gd_qnan.h fefca1ad05794e0397feeaccca899d0762effaa25d20d67cc119914416a51f97
 * stdlib/gdtoa-hexnan.c cc49256dc21753839945584928371f9a1381a69edb8da25675324d5477561bcb
 * stdio/nano-vfscanf_float.c 269e20a18609d82cfbf1e2ef075707a2453b3ea27e1fe0e08b2570ae4ab329a5
 * stdio/nano-vfprintf_float.c 944e518efe06064fa0d72e2a937d21bb51daa5a2f41de8ddff767bcdaf8b591e
 * Transformations: private helper/table namespace; explicit lifetime context;
 * checked NULL-return propagation; checked allocation arithmetic; fixed C-locale
 * frontends %lf/%g precision6/%f precision0..6. Build requires nano headers,
 * binary64, native32-bit Bigint limbs, -Os, Cortex-M4 Thumb; A1/M1 hard-float
 * fpv4-sp-d16, TC1 soft-float. _REENT_ONLY removes public strtod wrappers.
 * NO_HEX_FP and No_Hex_NaN compile out only branches unreachable through the
 * copied scanf token machine (no x, p, parentheses). No global libc hooks.
 */
/* Private checked lifetime for the pinned newlib numeric algorithms. */
#ifndef JSON_NUMERIC_CONTEXT_H
#define JSON_NUMERIC_CONTEXT_H
#include <reent.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#ifndef _REENT_SMALL
#error "JSON private contexts require newlib-nano headers (--specs=nano.specs)"
#endif
struct json_num_allocation;
struct json_num_context {
    int _errno;
    struct _mprec *_mp;
    struct json_num_allocation *allocations;
    size_t bytes;
    unsigned blocks;
    int failed;
};
/* These are rejection limits, not a claimed sufficient arena size. Every
 * allocation remains checked; bounded frontends and freelist reuse normally
 * stay well below them. Account headers and simultaneously retained objects. */
#define JSON_NUMERIC_SCRATCH_LIMIT 16384u
#define JSON_NUMERIC_BLOCK_LIMIT 64u
void *json_num_calloc(struct json_num_context *, size_t, size_t);
int json_num_init_mp(struct json_num_context *);
void json_num_cleanup(struct json_num_context *);
void json_num_resource_error(struct json_num_context *);
double json_num_strtod_r(struct json_num_context *, const char *, char **);
char *json_num_dtoa_r(struct json_num_context *, double, int, int, int *, int *, char **);
#define _calloc_r json_num_calloc
/* GCC statement expressions preserve control flow even in an unbraced if or
 * nested assignment. The return exits only this checked helper, never unwinds
 * across frames. All storage remains owned by the conversion lifetime. */
#define JSON_NUM_TRY(expression) __extension__ ({ \
    __typeof__(expression) json_num_result = (expression); \
    if (!json_num_result) return JSON_NUM_FAILURE; \
    json_num_result; \
})
#endif
