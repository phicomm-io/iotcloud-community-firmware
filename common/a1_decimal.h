/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Allocation-free A1 JSON decimal conversion. Input is a complete JSON number
 * of at most 349 bytes. Round binary64 to nearest, ties to even. Failures leave
 * the destination untouched. Formatting preserves the existing precision6 %g
 * contract; the explicit float32 entry uses nine significant digits. */
int ph_decimal_scan(const char *,double *);
int ph_decimal_format(char *,unsigned,double);
int ph_decimal_format_float32(char *,unsigned,float);
