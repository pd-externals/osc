/* packingOSC.h */
#ifndef _PACKINGOSC
#include "m_pd.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
# include <winsock2.h>
#else
# include <sys/types.h>
# include <netinet/in.h>
# include <ctype.h>
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

#endif // _PACKINGOSC
/* end of packingOSC.h */
