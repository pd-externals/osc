/* packingOSC.h */
#ifndef _PACKINGOSC
#include "m_pd.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
# include <winsock2.h>
#else
# include <netinet/in.h>
#endif /* _WIN32 */

/* Declarations */
#define MAX_MESG 65536 /* same as MAX_UDP_PACKET */
/* The maximum depth of bundles within bundles within bundles within...
   This is the size of a static array.  If you exceed this limit you'll
   get an error message. */
#define MAX_BUNDLE_NESTING 32

typedef union
{
    int     i;
    float   f;
} intfloat32;


#undef debug
#if DEBUG
# define debug printf
#else
static void debug(const char*fmt, ...) {(void)fmt;}
#endif


#endif // _PACKINGOSC
/* end of packingOSC.h */
