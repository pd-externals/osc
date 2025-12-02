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
#include <stdint.h>

typedef struct _OSCTimeTag
{
    uint32_t seconds;
    uint32_t fraction;
} OSCTimeTag;

OSCTimeTag OSCTT_Immediately(void);
OSCTimeTag OSCTT_Infinite(void);
OSCTimeTag OSCTT_Now(void);

OSCTimeTag OSCTT_Immediately(void);
OSCTimeTag OSCTT_Infinite(void);
OSCTimeTag OSCTT_Now(void);


/* get offset between two timestamps in ms */
double OSCTT_getoffsetms(const OSCTimeTag a, const OSCTimeTag reference);
OSCTimeTag OSCTT_offsetms(const OSCTimeTag org, double msec_offset);


/* debugging */
void OSCTT_post(const OSCTimeTag tt);

#endif // _OSC_timetag_h
/* end of OSC_timetag.h */
