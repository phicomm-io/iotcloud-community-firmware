/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PHICOMM_JSON_NUMBER_CHECKED_H
#define PHICOMM_JSON_NUMBER_CHECKED_H
/* scanf-compatible 1=assigned, 0=match failure, -1=empty input;
 * -2=resource failure (also attributed to the active JSON allocation scope).
 * On any failure *value remains untouched. errno is only compatibility
 * metadata; the explicit return and scope attribution govern success. */
#ifdef __NEWLIB__
int json_number_scan(const char *, double *);
#endif
/* Fixed native nano %g (precision6). Returns length or -1; failure preserves
 * the entire destination. Capacity32 covers every binary64 result. */
int json_number_format(char *, unsigned, double);
/* Nine significant digits for binary32 round-trip; same destination/error
 * contract and special-value spelling as json_number_format. Capacity32 is
 * sufficient for every float result. Does not change legacy precision6. */
int json_number_format_float32(char *, unsigned, float);
/* Fixed %f, precision0..6; same transactional destination/error contract. */
int json_number_format_fixed(char *, unsigned, double, unsigned);
#endif
