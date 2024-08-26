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

#define SECONDS_FROM_1900_to_1970 2208988800LL /* 17 leap years */
#define TWO_TO_THE_32_OVER_ONE_MILLION 4295LL



static OSCTimeTag OSCTT_Immediately(void);
static OSCTimeTag OSCTT_Infinite(void);
static OSCTimeTag OSCTT_Now(void);

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

static OSCTimeTag OSCTT_Now(void)
{
    OSCTimeTag tt;
#ifdef _WIN32
    struct _timeb tb;
    _ftime(&tb);

    /* First get the seconds right */
    tt.seconds = (unsigned)SECONDS_FROM_1900_to_1970 +
      (unsigned)tb.time;
    /* Now get the fractional part. */
    tt.fraction = (unsigned)tb.millitm*1000;
#else
    struct timeval tv;
    struct timezone tz;
    gettimeofday(&tv, &tz);

    /* First get the seconds right */
    tt.seconds = (unsigned) SECONDS_FROM_1900_to_1970 +
      (unsigned) tv.tv_sec;
    /* Now get the fractional part. */
    tt.fraction = (unsigned) tv.tv_usec;
#endif
    tt.fraction *= (unsigned) TWO_TO_THE_32_OVER_ONE_MILLION; /* convert usec to 32-bit fraction of 1 sec */
    return tt;
}


/* get offset between two timestamps in ms */
static double OSCTT_getoffsetms(const OSCTimeTag a, const OSCTimeTag reference)
{
  const double fract2ms = 1000./4294967296.;
  double d_sec = ((double)a.seconds  -(double)reference.seconds);
  double d_msec = ((double)a.fraction-(double)reference.fraction) * fract2ms;
  return d_sec * 1000. + d_msec;
}

static OSCTimeTag OSCTT_offsetms(const OSCTimeTag org, double msec_offset)
{
    OSCTimeTag tt;
    const double fract2ms = 1000./4294967296.;
    const double ms2fract = 4294967296./1000.;
    double secs_off = floor(msec_offset*0.001);
    msec_offset -= secs_off*1000.;
    int64_t sec  = org.seconds + (int64_t)secs_off;
    int64_t fract = (int64_t)(((double)org.fraction * fract2ms + msec_offset)*ms2fract);

    sec += (fract>>32);
    fract %= 0xFFFFFFFF;

    tt.seconds = sec%0xFFFFFFFF;
    tt.fraction = fract%0xFFFFFFFF;
    return tt;
}

static OSCTimeTag OSCTT_CurrentTimePlusOffset(uint32_t offset)
{ /* offset is in microseconds */
    return OSCTT_offsetms(OSCTT_Now(), ((double)offset)*0.001);
}


/* these are globals so other packOSCs will be in lockstep */
OSCTimeTag packOSCStartTimeTag;
double packOSCLogicalStartTime;

#ifdef PD
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
#endif

#endif // _OSC_timetag_h
/* end of OSC_timetag.h */
