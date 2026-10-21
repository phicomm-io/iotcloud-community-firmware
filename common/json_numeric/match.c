#include <stdlib.h>
#ifdef __NEWLIB__
/****************************************************************

The author of this software is David M. Gay.

Copyright (C) 2000 by Lucent Technologies
All Rights Reserved

Permission to use, copy, modify, and distribute this software and
its documentation for any purpose and without fee is hereby
granted, provided that the above copyright notice appear in all
copies and that both that the copyright notice and this
permission notice and warranty disclaimer appear in supporting
documentation, and that the name of Lucent or any of its entities
not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission.

LUCENT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.
IN NO EVENT SHALL LUCENT OR ANY OF ITS ENTITIES BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.

****************************************************************/

/* Please send bug reports to
	David M. Gay
	Bell Laboratories, Room 2C-463
	600 Mountain Avenue
	Murray Hill, NJ 07974-0636
	U.S.A.
	dmg@bell-labs.com
 */

/* Modified 06-21-2006 by Jeff Johnston to work with newlib.  */

#include <_ansi.h>
#include <reent.h>
#include <string.h>
#include "mprec.h"
#include "gdtoa.h"

#ifdef INFNAN_CHECK
int
_DEFUN (match, (sp, t),
	_CONST char **sp _AND
	char *t)
{
	int c, d;
	_CONST char *s = *sp;

	while( (d = *t++) !=0) {
		if ((c = *++s) >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c != d)
			return 0;
		}
	*sp = s + 1;
	return 1;
}


#endif

#endif /* __NEWLIB__ */
