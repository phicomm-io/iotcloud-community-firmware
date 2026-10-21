#include <stdlib.h>
#ifdef __NEWLIB__
/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */


#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include "context.h"
#include "../json_number_checked.h"
/* Exact nano float token limit: MAXEXP308 + MAXFRACT39 + sign/dot/NUL3.
 * The fixed %lf frontend never accepts x, p, or parentheses; strtod's hex
 * and NaN-payload branches are therefore unreachable and compiled out. */
#define BUF 350
#define MAX_LONG_LEN ((CHAR_BIT * sizeof (long) - 1) * 4 / 13 + 2)
#define SUPPRESS 0x10
#define SIGNOK 0x80
#define NDIGITS 0x100
#define DPTOK 0x200
#define EXPOK 0x400
struct json_num_scan_data { size_t width; int flags; unsigned nread; char buf[BUF]; };
/* strtol's base10 saturation, without a reentrancy/global-errno dependency. */
static long json_num_scan_exp(const char *p)
{
    unsigned long n = 0, limit;
    int negative = 0;
    if (*p == '-' || *p == '+') negative = *p++ == '-';
    limit = (unsigned long)LONG_MAX + negative;
    while (*p >= '0' && *p <= '9') {
        unsigned d = (unsigned)(*p++ - '0');
        if (n > (limit - d) / 10) {
            n = limit;
            while (*p >= '0' && *p <= '9') ++p;
            break;
        }
        n = n * 10 + d;
    }
    return negative ? (long)(0ul - n) : (long)n;
}
static unsigned json_num_exp_length(long n)
{
    unsigned length = n < 0 ? 3 : 2; /* e, optional minus, terminating NUL */
    unsigned long magnitude = n < 0 ? 0ul - (unsigned long)n : (unsigned long)n;
    do { ++length; magnitude /= 10; } while (magnitude);
    return length;
}
static void json_num_write_exp(char *p, long n)
{
    char digits[12]; unsigned used = 0;
    unsigned long u = n < 0 ? 0ul - (unsigned long)n : (unsigned long)n;
    *p++ = 'e'; if (n < 0) *p++ = '-';
    do { digits[used++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (used) *p++ = digits[--used];
    *p = 0;
}
/* Adapted verbatim state machine from nano-vfscanf_float.c. String cursor
 * replaces FILE refill/ungetc; only the assignment required by %lf remains. */
static int json_num_scan_token(struct json_num_context *context, const char *text, double *value)
{
  struct json_num_scan_data data = {0};
  struct json_num_scan_data *pdata = &data;
  int c;
  char *p;

  /* Scan a floating point number as if by strtod.  */
  /* This code used to assume that the number of digits is reasonable.
     However, ANSI / ISO C makes no such stipulation; we have to get
     exact results even when there is an unreasonable amount of leading
     zeroes.  */
  long leading_zeroes = 0;
  long zeroes, exp_adjust;
  char *exp_start = NULL;
  unsigned width_left = 0;
  char nancount = 0;
  char infcount = 0;
#ifdef hardway
  if (pdata->width == 0 || pdata->width > BUF - 1)
#else
  /* size_t is unsigned, hence this optimisation.  */
  if (pdata->width - 1 > BUF - 2)
#endif
    {
      width_left = pdata->width - (BUF - 1);
      pdata->width = BUF - 1;
    }
  pdata->flags |= SIGNOK | NDIGITS | DPTOK | EXPOK;
  zeroes = 0;
  exp_adjust = 0;
  for (p = pdata->buf; pdata->width; )
    {
      c = (unsigned char)*text;
      /* This code mimicks the integer conversion code,
	 but is much simpler.  */
      switch (c)
	{
	case '0':
	  if (pdata->flags & NDIGITS)
	    {
	      pdata->flags &= ~SIGNOK;
	      zeroes++;
	      if (width_left)
		{
		  width_left--;
		  pdata->width++;
		}
	      goto fskip;
	    }
	/* Fall through.  */
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	  if (nancount + infcount == 0)
	    {
	      pdata->flags &= ~(SIGNOK | NDIGITS);
	      goto fok;
	    }
	  break;

	case '+':
	case '-':
	  if (pdata->flags & SIGNOK)
	    {
	      pdata->flags &= ~SIGNOK;
	      goto fok;
	    }
	  break;
	case 'n':
	case 'N':
	  if (nancount == 0 && zeroes == 0
	      && (pdata->flags & (NDIGITS | DPTOK | EXPOK)) ==
				  (NDIGITS | DPTOK | EXPOK))
	    {
	      pdata->flags &= ~(SIGNOK | DPTOK | EXPOK | NDIGITS);
	      nancount = 1;
	      goto fok;
	    }
	  if (nancount == 2)
	    {
	      nancount = 3;
	      goto fok;
	    }
	  if (infcount == 1 || infcount == 4)
	    {
	      infcount++;
	      goto fok;
	    }
	  break;
	case 'a':
	case 'A':
	  if (nancount == 1)
	    {
	      nancount = 2;
	      goto fok;
	    }
	  break;
	case 'i':
	case 'I':
	  if (infcount == 0 && zeroes == 0
	      && (pdata->flags & (NDIGITS | DPTOK | EXPOK)) ==
				  (NDIGITS | DPTOK | EXPOK))
	    {
	      pdata->flags &= ~(SIGNOK | DPTOK | EXPOK | NDIGITS);
	      infcount = 1;
	      goto fok;
	    }
	  if (infcount == 3 || infcount == 5)
	    {
	      infcount++;
	      goto fok;
	    }
	  break;
	case 'f':
	case 'F':
	  if (infcount == 2)
	    {
	      infcount = 3;
	      goto fok;
	    }
	  break;
	case 't':
	case 'T':
	  if (infcount == 6)
	    {
	      infcount = 7;
	      goto fok;
	    }
	  break;
	case 'y':
	case 'Y':
	  if (infcount == 7)
	    {
	      infcount = 8;
	      goto fok;
	    }
	  break;
	case '.':
	  if (pdata->flags & DPTOK)
	    {
	      pdata->flags &= ~(SIGNOK | DPTOK);
	      leading_zeroes = zeroes;
	      goto fok;
	    }
	  break;
	case 'e':
	case 'E':
	  /* No exponent without some digits.  */
	  if ((pdata->flags & (NDIGITS | EXPOK)) == EXPOK
	      || ((pdata->flags & EXPOK) && zeroes))
	    {
	      if (! (pdata->flags & DPTOK))
		{
		  exp_adjust = zeroes - leading_zeroes;
		  exp_start = p;
	        }
	      pdata->flags =
		(pdata->flags & ~(EXPOK | DPTOK)) | SIGNOK | NDIGITS;
	      zeroes = 0;
	      goto fok;
	    }
	  break;
	}
      break;
fok:
      *p++ = c;
fskip:
      pdata->width--;
      ++pdata->nread;
      if (!*++text)
	/* "EOF".  */
	break;
    }
  if (zeroes)
    pdata->flags &= ~NDIGITS;
  /* We may have a 'N' or possibly even [sign] 'N' 'a' as the
     start of 'NaN', only to run out of chars before it was
     complete (or having encountered a non-matching char).  So
     check here if we have an outstanding nancount, and if so
     put back the chars we did swallow and treat as a failed
     match.

     FIXME - we still don't handle NAN([0xdigits]).  */
  if (nancount - 1U < 2U)
    {
      /* "nancount && nancount < 3".  */
      /* Newlib's ungetc works even if we called __srefill in
	 the middle of a partial parse, but POSIX does not
	 guarantee that in all implementations of ungetc.  */
      while (p > pdata->buf)
	{
	  (void)*--p; /* "[-+nNaA]".  */
	  --pdata->nread;
	}
      return 0;
    }
  /* Likewise for 'inf' and 'infinity'.	 But be careful that
     'infinite' consumes only 3 characters, leaving the stream
     at the second 'i'.	 */
  if (infcount - 1U < 7U)
    {
      /* "infcount && infcount < 8".  */
      if (infcount >= 3) /* valid 'inf', but short of 'infinity'.  */
	while (infcount-- > 3)
	  {
	    (void)*--p; /* "[iInNtT]".  */
	    --pdata->nread;
	  }
      else
        {
	  while (p > pdata->buf)
	    {
	      (void)*--p; /* "[-+iInN]".  */
	      --pdata->nread;
	    }
	  return 0;
	}
    }
  /* If no digits, might be missing exponent digits
     (just give back the exponent) or might be missing
     regular digits, but had sign and/or decimal point.  */
  if (pdata->flags & NDIGITS)
    {
      if (pdata->flags & EXPOK)
	{
	  /* No digits at all.  */
	  while (p > pdata->buf)
	    {
	      (void)*--p; /* "[-+.]".  */
	      --pdata->nread;
	    }
	  return 0;
	}
      /* Just a bad exponent (e and maybe sign).  */
      c = *--p;
      --pdata->nread;
      if (c != 'e' && c != 'E')
	{
	  (void)c; /* "[-+]".  */
	  c = *--p;
	  --pdata->nread;
	}
      (void)c; /* "[eE]".  */
    }
  if ((pdata->flags & SUPPRESS) == 0)
    {
      double fp;
      long new_exp = 0;

      *p = 0;
      if ((pdata->flags & (DPTOK | EXPOK)) == EXPOK)
	{
	  exp_adjust = zeroes - leading_zeroes;
	  new_exp = -exp_adjust;
	  exp_start = p;
	}
      else if (exp_adjust)
	new_exp = (long)((unsigned long)json_num_scan_exp(exp_start + 1) - (unsigned long)exp_adjust);

      if (exp_adjust)
	{
	  /* If there might not be enough space for the new exponent,
	     truncate some trailing digits to make room.  */
	  if (exp_start >= pdata->buf + BUF - MAX_LONG_LEN)
	    exp_start = pdata->buf + BUF - MAX_LONG_LEN - 1;
          /* Native MAX_LONG_LEN misses e/sign/NUL for some normalized
           * exponents. Preserve its truncation unless the actual rewritten
           * token would cross BUF; then truncate only the additional digits. */
          unsigned exponent_bytes = json_num_exp_length(new_exp);
          if (exp_start > pdata->buf + BUF - exponent_bytes)
            exp_start = pdata->buf + BUF - exponent_bytes;
	  json_num_write_exp(exp_start, new_exp);
	}

      /* Current _strtold routine is markedly slower than
	 _strtod_r.  Only use it if we have a long double
	 result.  */
#ifdef JSON_NUMERIC_TEST_TOKEN_BOUND
      extern void json_numeric_test_token_bound(const char *, unsigned);
      json_numeric_test_token_bound(pdata->buf, BUF);
#endif
      fp = json_num_strtod_r (context, pdata->buf, NULL);

      if (context->failed) return -2;
      *value = fp;
    }
  return 1;
}
int json_number_scan(const char *text, double *value)
{
    struct json_num_context context = {0};
    double converted;
    int result;
    while (isspace((unsigned char)*text)) ++text;
    if (!*text) return -1;
    result = json_num_scan_token(&context, text, &converted);
    if (context.failed) result = -2;
    json_num_cleanup(&context);
    if (result == 1) *value = converted;
    else if (result == -2) errno = ENOMEM;
    return result;
}

#endif /* __NEWLIB__ */
