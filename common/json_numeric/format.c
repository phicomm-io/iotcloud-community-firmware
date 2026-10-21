/*
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdlib.h>
#ifdef __NEWLIB__
/* Fixed %g layout extracted from pinned nano-vfprintf_float.c; numerical
 * digits and rounding remain its original dtoa mode2 precision6 algorithm.
 * Berkeley license for the layout source is retained in scan.c. */
#include <math.h>
#include <string.h>
#include "context.h"
#include "../json_number_checked.h"
int json_number_format(char *out, unsigned capacity, double value)
{
    struct json_num_context context = {0};
    char number[32], *p = number, *digits, *end;
    int exponent, sign, count, i;
    if (isnan(value)) { memcpy(p, "nan", 3); p += 3; }
    else if (isinf(value)) {
        if (value < 0) *p++ = '-';
        memcpy(p, "inf", 3); p += 3;
    } else {
        digits = json_num_dtoa_r(&context, value, 2, 6, &exponent, &sign, &end);
        if (!digits || context.failed) {
            json_num_cleanup(&context); errno = ENOMEM; return -1;
        }
        count = (int)(end - digits);
        if (sign) *p++ = '-';
        if (exponent <= -4 || exponent > 6) {
            unsigned magnitude;
            char reversed[4]; unsigned used = 0;
            *p++ = *digits++;
            if (count > 1) {
                *p++ = '.';
                memcpy(p, digits, (unsigned)(count - 1)); p += count - 1;
            }
            --exponent;
            *p++ = 'e'; *p++ = exponent < 0 ? '-' : '+';
            magnitude = exponent < 0 ? (unsigned)-exponent : (unsigned)exponent;
            do { reversed[used++] = (char)('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
            if (used == 1) *p++ = '0';
            while (used) *p++ = reversed[--used];
        } else if (value == 0) {
            *p++ = '0';
        } else if (exponent <= 0) {
            *p++ = '0'; *p++ = '.';
            for (i = 0; i < -exponent; ++i) *p++ = '0';
            memcpy(p, digits, (unsigned)count); p += count;
        } else {
            for (i = 0; i < exponent; ++i) *p++ = i < count ? digits[i] : '0';
            if (exponent < count) {
                *p++ = '.';
                memcpy(p, digits + exponent, (unsigned)(count - exponent)); p += count - exponent;
            }
        }
    }
    json_num_cleanup(&context);
    count = (int)(p - number);
    if ((unsigned)count >= capacity) return -1;
    memcpy(out, number, (unsigned)count); out[count] = 0;
    return count;
}
/* Fixed %f for application-owned telemetry. No generic varargs formatter.
 * Compute the final size before the first destination byte is written, so a
 * small caller buffer is a normal failure even for DBL_MAX. */
int json_number_format_fixed(char *out, unsigned capacity, double value, unsigned precision)
{
    struct json_num_context context = {0};
    char *digits = NULL, *end, *p = out;
    int exponent = 0, sign = 0, count = 0;
    unsigned integral, required, i;
    if (precision > 6) return -1;
    if (isnan(value) || isinf(value)) {
        const char *special = isnan(value) ? "nan" : value < 0 ? "-inf" : "inf";
        required = (unsigned)strlen(special);
        if (required >= capacity) return -1;
        memcpy(out, special, required + 1); return (int)required;
    }
    digits = json_num_dtoa_r(&context, value, 3, (int)precision, &exponent, &sign, &end);
    if (!digits || context.failed) {
        json_num_cleanup(&context); errno = ENOMEM; return -1;
    }
    count = (int)(end - digits);
    integral = exponent > 0 ? (unsigned)exponent : 1;
    required = (unsigned)sign + integral + (precision ? precision + 1 : 0);
    if (required >= capacity) { json_num_cleanup(&context); return -1; }
    if (sign) *p++ = '-';
    if (exponent <= 0) *p++ = '0';
    else for (i = 0; i < integral; ++i) *p++ = i < (unsigned)count ? digits[i] : '0';
    if (precision) *p++ = '.';
    for (i = 0; i < precision; ++i) {
        int index = exponent + (int)i;
        *p++ = index >= 0 && index < count ? digits[index] : '0';
    }
    *p = 0;
    json_num_cleanup(&context);
    return (int)required;
}

#endif /* __NEWLIB__ */
#ifndef __NEWLIB__
/* Host test/simulator fallback only. Embedded builds always compile the pinned
 * checked port above; glibc results are never evidence for MiCO allocation. */
#include <stdio.h>
#include <string.h>
#include "../json_number_checked.h"
int json_number_format(char *out, unsigned capacity, double value)
{
    char temporary[32];
    int n = snprintf(temporary, sizeof(temporary), "%g", value);
    if (n < 0 || (unsigned)n >= capacity || (unsigned)n >= sizeof(temporary)) return -1;
    memcpy(out, temporary, (unsigned)n + 1); return n;
}
int json_number_format_fixed(char *out, unsigned capacity, double value, unsigned precision)
{
    char temporary[340];
    int n;
    if (precision > 6) return -1;
    n = snprintf(temporary, sizeof(temporary), "%.*f", (int)precision, value);
    if (n < 0 || (unsigned)n >= capacity || (unsigned)n >= sizeof(temporary)) return -1;
    memcpy(out, temporary, (unsigned)n + 1); return n;
}
#endif
