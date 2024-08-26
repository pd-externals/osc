/* OSC_timeTag.h: handle OSC timetags
   © Martin Peach

 * based on:
 OSC_timeTag.h: library for manipulating OSC time tags
 Matt Wright, 5/29/97

 Time tags in OSC have the same format as in NTP: 64 bit fixed point, with the
 top 32 bits giving number of seconds sinve midnight 1/1/1900 and the bottom
 32 bits giving fractional parts of a second.  We represent this by an 8-byte
 unsigned long if possible, or else a struct.

 NB: On many architectures with 8-byte ints, it's illegal (like maybe a bus error)
 to dereference a pointer to an 8 byte int that's not 8-byte aligned.
*/



#ifndef _OSC_timetag_h
#define _OSC_timetag_h

#ifdef _WIN32
# include <sys/timeb.h>
#else
# include <sys/time.h>
#endif /* _WIN32 */

#include <math.h>
#include <stdint.h>
#ifdef HAVE_S_STUFF_H
#include "s_stuff.h"
#endif
typedef struct _OSCTimeTag
{
    uint32_t seconds;
    uint32_t fraction;
} OSCTimeTag;

static OSCTimeTag OSCTT_Immediately(void);
static OSCTimeTag OSCTT_Infinite(void);


static OSCTimeTag OSCTT_CurrentTimePlusOffset(uint32_t offset);
static OSCTimeTag OSCTT_CurrentPdTimePlusOffset(uint32_t offset);


/* The next bit is modified from OSC-timetag.c. */
/*

 OSC_timeTag.c: library for manipulating OSC time tags
 Matt Wright, 5/29/97

 Version 0.2 (9/11/98): cleaned up so no explicit type names in the .c file.

*/

static OSCTimeTag OSCTT_Immediately(void)
{
    OSCTimeTag tt;
    tt.fraction = 1;
    tt.seconds = 0;
    return tt;
}

static OSCTimeTag OSCTT_Infinite(void)
{
    OSCTimeTag tt;
    tt.fraction = 0xffffffffL;
    tt.seconds = 0xffffffffL;
    return tt;
}

#define SECONDS_FROM_1900_to_1970 2208988800LL /* 17 leap years */
#define TWO_TO_THE_32_OVER_ONE_MILLION 4295LL

static OSCTimeTag OSCTT_CurrentTimePlusOffset(uint32_t offset)
{ /* offset is in microseconds */
    OSCTimeTag tt;
    static unsigned int onemillion = 1000000;
#ifdef _WIN32
    static unsigned int onethousand = 1000;
    struct _timeb tb;

    _ftime(&tb);

    /* First get the seconds right */
    tt.seconds = (unsigned)SECONDS_FROM_1900_to_1970 +
        (unsigned)tb.time+
        (unsigned)offset/onemillion;
    /* Now get the fractional part. */
    tt.fraction = (unsigned)tb.millitm*onethousand + (unsigned)(offset%onemillion); /* in usec */
#else
    struct timeval tv;
    struct timezone tz;

    gettimeofday(&tv, &tz);

    /* First get the seconds right */
    tt.seconds = (unsigned) SECONDS_FROM_1900_to_1970 +
        (unsigned) tv.tv_sec +
        (unsigned) offset/onemillion;
    /* Now get the fractional part. */
    tt.fraction = (unsigned) tv.tv_usec + (unsigned)(offset%onemillion); /* in usec */
#endif
    if (tt.fraction > onemillion)
    {
        tt.fraction -= onemillion;
        tt.seconds++;
    }
    tt.fraction *= (unsigned) TWO_TO_THE_32_OVER_ONE_MILLION; /* convert usec to 32-bit fraction of 1 sec */
    return tt;
}


/* these are globals so other packOSCs will be in lockstep */
OSCTimeTag packOSCStartTimeTag;
double packOSCLogicalStartTime;

/* Use the global time that was set once by the first packOSC instance, to avoid OS/Pd clock jitter  */
static OSCTimeTag OSCTT_CurrentPdTimePlusOffset(uint32_t offset)
{ /* offset is in microseconds */
    OSCTimeTag tt;
    static unsigned int onemillion = 1000000;
    static unsigned int onethousand = 1000;

    /* milliseconds since 'our' epoch */
    double delta_ms = clock_gettimesince(packOSCLogicalStartTime);


    /* First get the seconds right */
    tt.seconds = floor(delta_ms/onethousand) +
        packOSCStartTimeTag.seconds +
        (unsigned) offset/onemillion;
    /* Now get the fractional part. */
    tt.fraction = (unsigned) (delta_ms*onethousand - tt.seconds*onemillion) +
        packOSCStartTimeTag.fraction +
        (unsigned)(offset%onemillion); /* in usec */

    if (tt.fraction > onemillion)
    {
        tt.fraction -= onemillion;
        tt.seconds++;
    }
    tt.fraction *= (unsigned) TWO_TO_THE_32_OVER_ONE_MILLION; /* convert usec to 32-bit fraction of 1 sec */
    if(sys_verbose)
        logpost(0, 3, "delta_ms %lf timetag: %ldsec %ld\n",
		delta_ms, (long int)tt.seconds, (long int)tt.fraction);
    return tt;
}

#endif // _OSC_timetag_h
/* end of OSC_timetag.h */
