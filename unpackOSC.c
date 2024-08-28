/* unpackOSC is like dumpOSC but outputs a list consisting of a single symbol for the path  */
/* and a list of floats and/or symbols for the data, and adds an outlet for a time delay. */
/* This allows for the separation of the protocol and its transport. */
/* Started by Martin Peach 20060420 */
/* dumpOSC.c header follows: */
/*
Written by Matt Wright and Adrian Freed, The Center for New Music and
Audio Technologies, University of California, Berkeley.  Copyright (c)
1992,93,94,95,96,97,98,99,2000,01,02,03,04 The Regents of the University of
California (Regents).

Permission to use, copy, modify, distribute, and distribute modified versions
of this software and its documentation without fee and without a signed
licensing agreement, is hereby granted, provided that the above copyright
notice, this paragraph and the following two paragraphs appear in all copies,
modifications, and distributions.

IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS, ARISING
OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF REGENTS HAS
BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE. THE SOFTWARE AND ACCOMPANYING DOCUMENTATION, IF ANY, PROVIDED
HEREUNDER IS PROVIDED "AS IS". REGENTS HAS NO OBLIGATION TO PROVIDE
MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.


The OSC webpage is http://cnmat.cnmat.berkeley.edu/OpenSoundControl
*/

/*

    dumpOSC.c
    server that displays OpenSoundControl messages sent to it
    for debugging client udp and UNIX protocol

    by Matt Wright, 6/3/97
    modified from dumpSC.c, by Matt Wright and Adrian Freed

    version 0.2: Added "-silent" option a.k.a. "-quiet"

    version 0.3: Incorporated patches from Nicola Bernardini to make
    things Linux-friendly.  Also added ntohl() in the right places
    to support little-endian architectures.

    to-do:

    More robustness in saying exactly what's wrong with ill-formed
    messages.  (If they don't make sense, show exactly what was
    received.)

    Time-based features: print time-received for each packet

    Clean up to separate OSC parsing code from socket/select stuff

    pd: branched from http://www.cnmat.berkeley.edu/OpenSoundControl/src/dumpOSC/dumpOSC.c
    -------------
    -- added pd functions
    -- socket is made differently than original via pd mechanisms
    -- tweaks for Win32    www.zeggz.com/raf 13-April-2002
    -- the OSX changes from cnmat didnt make it here yet but this compiles
        on OSX anyway.
*/

//#define DEBUG 1
#include "packingOSC.h"
#include "OSC_timeTag.h"

static t_class *unpackOSC_class;

typedef struct _unpackOSC
{
    t_object    x_obj;
    t_outlet    *x_data_out;
    t_outlet    *x_delay_out;
    int         x_bundle_flag;/* non-zero if we are processing a bundle */
    int         x_recursion_level;/* number of times we reenter unpackOSC_list */
    int         x_abort_bundle;/* non-zero if unpackOSC_list is not well formed */
    int         x_reentry_count;

    int         x_use_pd_time;
    OSCTimeTag  x_pd_timetag;
    double      x_pd_timeref;
} t_unpackOSC;

void unpackOSC_setup(void);
static void *unpackOSC_new(void);
static void unpackOSC_free(t_unpackOSC *x);
static void unpackOSC_list(t_unpackOSC *x, t_symbol *s, int argc, t_atom *argv);
static void unpackOSC_usepdtime(t_unpackOSC *x, t_floatarg f);
static t_symbol* unpackOSC_path(t_unpackOSC *x, const char *path, size_t length);
static void unpackOSC_Smessage(t_unpackOSC *x, t_atom *data_at, int *data_atc, void *v, int n);
static void unpackOSC_PrintTypeTaggedArgs(t_unpackOSC *x, t_atom *data_at, int *data_atc, void *v, int n);
static void unpackOSC_PrintHeuristicallyTypeGuessedArgs(t_unpackOSC *x, t_atom *data_at, int *data_atc, void *v, int n, int skipComma);
static const char *unpackOSC_DataAfterAlignedString(t_unpackOSC *x, const char *string, const char *boundary);
static int unpackOSC_IsNiceString(t_unpackOSC *x, const char *string, const char *boundary);

static void *unpackOSC_new(void)
{
    t_unpackOSC *x;
    x = (t_unpackOSC *)pd_new(unpackOSC_class);
    x->x_data_out = outlet_new(&x->x_obj, &s_list);
    x->x_delay_out = outlet_new(&x->x_obj, &s_float);
    x->x_bundle_flag = 0;
    x->x_recursion_level = 0;
    x->x_abort_bundle = 0;

    unpackOSC_usepdtime(x, 1.);
    return (x);
}

static void unpackOSC_free(t_unpackOSC *x)
{
    (void)x;
}

void unpackOSC_setup(void)
{
    unpackOSC_class = class_new(gensym("unpackOSC"),
        (t_newmethod)unpackOSC_new, (t_method)unpackOSC_free,
        sizeof(t_unpackOSC), 0, 0);
    class_addlist(unpackOSC_class, (t_method)unpackOSC_list);
    class_addmethod(unpackOSC_class, (t_method)unpackOSC_usepdtime,
        gensym("usepdtime"), A_FLOAT, 0);
}
static void unpackOSC_usepdtime(t_unpackOSC *x, t_floatarg f)
{
  x->x_use_pd_time = (int)f;
  if(x->x_use_pd_time) {
    x->x_pd_timetag = OSCTT_Now();
    x->x_pd_timeref = clock_getlogicaltime();
  } else {
    x->x_pd_timetag.seconds = x->x_pd_timetag.fraction = 0;
    x->x_pd_timeref = 0;
  }

}

/* unpackOSC_list expects an OSC packet in the form of a list of floats on [0..255] */
static void unpackOSC_dolist(t_unpackOSC *x, int argc, const char *buf, t_atom out_argv[MAX_MESG])
{
    debug(">>> %s(%p, %d, %p)\n", __FUNCTION__, x, argc, buf);
    debug(">>> unpackOSC_list: %d bytes, abort=%d, reentry_count %d recursion_level %d\n",
        argc, x->x_abort_bundle, x->x_reentry_count, x->x_recursion_level);
    if(x->x_abort_bundle) return; /* if backing quietly out of the recursive stack */
    x->x_reentry_count++;
    if ((argc%4) != 0)
    {
        pd_error(x, "unpackOSC: Packet size (%d) not a multiple of 4 bytes: dropping packet", argc);
        goto unpackOSC_list_out;
    }

    if ((argc >= 8) && (strncmp(buf, "#bundle", 8) == 0))
    { /* This is a bundle message. */
        int i = 16; /* Skip "#bundle\0" and time tag */
        OSCTimeTag tt;
        double delta = 0.;
        debug("unpackOSC: bundle msg:\n");

        if (argc < 16)
        {
            pd_error(x, "unpackOSC: Bundle message too small (%d bytes) for time tag", argc);
            goto unpackOSC_list_out;
        }

        x->x_bundle_flag = 1;

        /* Print the time tag */
        debug("unpackOSC bundle timetag: [ %x.%0x\n", ntohl(*((uint32_t *)(buf+8))),
            ntohl(*((uint32_t *)(buf+12))));

        /* convert the timetag into a millisecond delay from now */
        tt.seconds = ntohl(*((uint32_t *)(buf+8)));
        tt.fraction = ntohl(*((uint32_t *)(buf+12)));
        /* pd can use a delay in milliseconds */
        if (tt.seconds == 0 && tt.fraction ==1) {
            delta = 0;
        } else {
            OSCTimeTag now;
            if (x->x_use_pd_time) {
                double deltaref = clock_gettimesince(x->x_pd_timeref);
                now = OSCTT_offsetms(x->x_pd_timetag, deltaref);
            } else {
                now = OSCTT_Now();
            }
            delta = OSCTT_getoffsetms(tt, now);
        }
        outlet_float(x->x_delay_out, delta);
        /* Note: if we wanted to actually use the time tag as a little-endian
          64-bit int, we'd have to word-swap the two 32-bit halves of it */

        while(i < argc)
        {
            int size = ntohl(*((int *) (buf + i)));
            if ((size % 4) != 0)
            {
                pd_error(x, "unpackOSC: Bad size count %d in bundle (not a multiple of 4)", size);
                goto unpackOSC_list_out;
            }
            if ((size + i + 4) > argc)
            {
                pd_error(x, "unpackOSC: Bad size count %d in bundle (only %d bytes left in entire bundle)",
                    size, argc-i-4);
                goto unpackOSC_list_out;
            }

            /* Recursively handle element of bundle */
            x->x_recursion_level++;
            debug("unpackOSC: bundle depth %d\n", x->x_recursion_level);
            if (x->x_recursion_level > MAX_BUNDLE_NESTING)
            {
                pd_error(x, "unpackOSC: bundle depth %d exceeded", MAX_BUNDLE_NESTING);
                x->x_abort_bundle = 1;/* we need to back out of the recursive stack*/
                goto unpackOSC_list_out;
            }
            debug("unpackOSC: bundle calling unpackOSC_list(x=%p, size=%d, buf[%d]=%p)\n",
              x, size, i+4, &buf[i+4]);
            unpackOSC_dolist(x, size, &buf[i+4], out_argv);
            i += 4 + size;
        }

        if (i != argc)
        {
            pd_error(x, "unpackOSC: This can't happen");
        }

        x->x_bundle_flag = 0; /* end of bundle */
        debug("unpackOSC: bundle end ] depth is %d\n", x->x_recursion_level);

    } else if ((argc == 24) && (strcmp(buf, "#time") == 0)) {
        post("unpackOSC: Time message: %s\n :).\n", buf);
        goto unpackOSC_list_out;
    } else { /* This is not a bundle message or a time message */
        int out_argc = 0; /* number of atoms to be output; atoms are stored in out_argv */
        const char*messageName = buf;
        int messageLen;
        const char*args = unpackOSC_DataAfterAlignedString(x, messageName, buf+argc);
        t_symbol *path;

        debug("unpackOSC: message name string: %s length %d\n", messageName, argc);
        if (args == 0)
        {
            pd_error(x, "unpackOSC: Bad message name string: Dropping entire message.");
            goto unpackOSC_list_out;
        }
        messageLen = args-messageName;
        /* put the OSC path into a single symbol */
        path = unpackOSC_path(x, messageName, messageLen); /* returns 0 if path failed  */
        if (path == 0)
        {
            pd_error(x, "unpackOSC: Bad message path: Dropping entire message.");
            goto unpackOSC_list_out;
        }
        debug("unpackOSC: message '%s', args=%p\n", path?path->s_name:0, args);
        debug("unpackOSC_list calling unpackOSC_Smessage: message length %d\n", argc-messageLen);

        unpackOSC_Smessage(x, out_argv, &out_argc, (void *)args, argc-messageLen);
        if (0 == x->x_bundle_flag)
            outlet_float(x->x_delay_out, 0); /* no delay for message not in a bundle */
        outlet_anything(x->x_data_out, path, out_argc, out_argv);
    }
    x->x_abort_bundle = 0;
unpackOSC_list_out:
    x->x_recursion_level = 0;
    x->x_reentry_count--;
}

static void unpackOSC_list(t_unpackOSC *x, t_symbol *s, int argc, t_atom *argv)
{
    static t_atom out_atoms[MAX_MESG]; /* symbols making up the payload */
    char raw[MAX_MESG];/* bytes making up the entire OSC message */
    int i;
    (void)s;
    if(argc>MAX_MESG) {
        pd_error(x, "unpackOSC: Packet size (%d) greater than max (%d). Change MAX_MESG and recompile if you want more.", argc, MAX_MESG);
        return;
    }
    /* copy the list to a byte buffer, checking for bytes only */
    for (i = 0; i < argc; ++i)
    {
        if (argv[i].a_type == A_FLOAT)
        {
            int j = (int)argv[i].a_w.w_float;
/*          if ((j == argv[i].a_w.w_float) && (j >= 0) && (j <= 255)) */
/* this can miss bytes between 128 and 255 because they are interpreted somewhere as negative, */
/* so change to this: */
            if ((j == argv[i].a_w.w_float) && (j >= -128) && (j <= 255))
            {
                raw[i] = (char)j;
            }
            else
            {
                pd_error(x, "unpackOSC: Data[%d] out of range (%d), dropping packet",
                         i, (int)argv[i].a_w.w_float);
                return;
            }
        }
        else
        {
            pd_error(x, "unpackOSC: Data[%d] not float, dropping packet", i);
            return;
        }
    }

    unpackOSC_dolist(x, argc, raw, out_atoms);
}

static t_symbol*unpackOSC_path(t_unpackOSC *x, const char *path, size_t len)
{
    size_t i;

    if(len<1)
    {
        pd_error(x, "unpackOSC: No Path, dropping message");
        return 0;
    }

    if (path[0] != '/')
    {
        pd_error(x, "unpackOSC: Path doesn't begin with \"/\", dropping message");
        return 0;
    }
    for (i = 1; i < len; ++i)
    {
        if (path[i] == '\0')
        { /* the end of the path: turn path into a symbol */
            return gensym(path);
        }
    }
    pd_error(x, "unpackOSC: Path too long, dropping message");
    return 0;
}
#define SMALLEST_POSITIVE_FLOAT 0.000001f

static void unpackOSC_Smessage(t_unpackOSC *x, t_atom *data_at, int *data_atc, void *v, int n)
{
    char *chars = v;

    if (n != 0)
    {
        if (chars[0] == ',')
        {
            if (chars[1] != ',')
            {
                debug("unpackOSC_Smessage calling unpackOSC_PrintTypeTaggedArgs: message length %d\n", n);
                /* This message begins with a type-tag string */
                unpackOSC_PrintTypeTaggedArgs(x, data_at, data_atc, v, n);
            }
            else
            {
                debug("unpackOSC_Smessage calling unpackOSC_PrintHeuristicallyTypeGuessedArgs: message length %d, skipComma 1\n", n);
                /* Double comma means an escaped real comma, not a type string */
                unpackOSC_PrintHeuristicallyTypeGuessedArgs(x, data_at, data_atc, v, n, 1);
            }
        }
        else
        {
            debug("unpackOSC_Smessage calling unpackOSC_PrintHeuristicallyTypeGuessedArgs: message length %d, skipComma 0\n", n);
            unpackOSC_PrintHeuristicallyTypeGuessedArgs(x, data_at, data_atc, v, n, 0);
        }
    }
}

static void unpackOSC_PrintTypeTaggedArgs(t_unpackOSC *x, t_atom *data_at, int *data_atc, void *v, int n)
{
    const char *typeTags, *thisType, *p;
    int myargc = *data_atc;
    t_atom *mya = data_at;

    typeTags = v;

    if (!unpackOSC_IsNiceString(x, typeTags, typeTags+n))
    {
        debug("unpackOSC_PrintTypeTaggedArgs not nice string\n");
        /* No null-termination, so maybe it wasn't a type tag
        string after all */
        unpackOSC_PrintHeuristicallyTypeGuessedArgs(x, data_at, data_atc, v, n, 0);
        return;
    }

    debug("unpackOSC_PrintTypeTaggedArgs calling unpackOSC_DataAfterAlignedString %p to  %p\n", typeTags, typeTags+n);
    p = unpackOSC_DataAfterAlignedString(x, typeTags, typeTags+n);
    debug("unpackOSC_PrintTypeTaggedArgs p is %p: ", p);
    if (p == NULL) return; /* malformed message */
    for (thisType = typeTags + 1; *thisType != 0; ++thisType)
    {
        switch (*thisType)
        {
            case 'b': /* blob: an int32 size count followed by that many 8-bit bytes */
            {
                int i, blob_bytes = ntohl(*((int *) p));
                debug("blob: %u bytes\n", blob_bytes);
                p += 4;
                for (i = 0; i < blob_bytes; ++i, ++p, ++myargc)
                    SETFLOAT(mya+myargc,(*(unsigned char *)p));
                while (i%4)
                {
                    ++i;
                    ++p;
                }
                break;
            }
            case 'm': /* MIDI message is the next four bytes*/
            {
                int i;
                debug("MIDI: 0x%08X\n", ntohl(*((int *) p)));
                for (i = 0; i < 4; ++i, ++p, ++myargc)
                    SETFLOAT(mya+myargc, (*(unsigned char *)p));
                break;
            }
            case 'i': case 'r': case 'c':
                debug("integer: %d\n", ntohl(*((int *) p)));
                SETFLOAT(mya+myargc,(signed)ntohl(*((int *) p)));
                myargc++;
                p += 4;
                break;
            case 'f':
            {
                intfloat32 thisif;
                thisif.i = ntohl(*((int *) p));
                debug("float: %f\n", thisif.f);
                SETFLOAT(mya+myargc, thisif.f);
                myargc++;
                p += 4;
                break;
            }
            case 'h': case 't':
                debug("[A 64-bit int]\n");
                pd_error(x, "unpackOSC: PrintTypeTaggedArgs: [A 64-bit int] not implemented");
                p += 8;
                break;
            case 'd':
                debug("[A 64-bit float]\n");
                pd_error(x, "unpackOSC: PrintTypeTaggedArgs: [A 64-bit float] not implemented");
                p += 8;
                break;
            case 's': case 'S':
                if (!unpackOSC_IsNiceString(x, p, typeTags+n))
                {
                    pd_error(x, "unpackOSC: PrintTypeTaggedArgs: Type tag said this arg is a string but it's not!\n");
                    return;
                }
                else
                {
                    debug("string: \"%s\"\n", p);
                    SETSYMBOL(mya+myargc,gensym(p));
                    myargc++;
                    p = unpackOSC_DataAfterAlignedString(x, p, typeTags+n);

                }
                break;
            case 'T':
                debug("[True]\n");
                SETFLOAT(mya+myargc,1.);
                myargc++;
                break;
            case 'F':
                debug("[False]\n");
                SETFLOAT(mya+myargc,0.);
                myargc++;
                break;
            case 'N':
                debug("[Nil]\n");
                SETFLOAT(mya+myargc,0.);
                myargc++;
                break;
            case 'I':
                debug("[Infinitum]\n");
                SETSYMBOL(mya+myargc,gensym("INF"));
                myargc++;
                break;
            default:
                debug("unpackOSC_PrintTypeTaggedArgs unknown typetag %c (%0X)\n", *thisType, *thisType);
                pd_error(x, "unpackOSC: PrintTypeTaggedArgs: [Unrecognized type tag %c]", *thisType);
                return;
         }
    }
    *data_atc = myargc;
}

static void unpackOSC_PrintHeuristicallyTypeGuessedArgs(t_unpackOSC *x, t_atom *data_at, int *data_atc, void *v, int n, int skipComma)
{
    int i;
    int *ints;
    intfloat32 thisif;
    const char *chars, *string, *nextString;
    int myargc = *data_atc;
    t_atom* mya = data_at;

    /* Go through the arguments 32 bits at a time */
    ints = v;
    chars = v;

    for (i = 0; i < n/4; )
    {
        string = &chars[i*4];
        thisif.i = ntohl(ints[i]);
        /* Reinterpret the (potentially byte-reversed) thisif as a float */

        if (thisif.i >= -1000 && thisif.i <= 1000000)
        {
            debug("%d ", thisif.i);
            SETFLOAT(mya+myargc,(t_float) (thisif.i));
            myargc++;
            i++;
        }
        else if (thisif.f >= -1000.f && thisif.f <= 1000000.f &&
            (thisif.f <=0.0f || thisif.f >= SMALLEST_POSITIVE_FLOAT))
        {
            debug("%f ",  thisif.f);
            SETFLOAT(mya+myargc,thisif.f);
            myargc++;
            i++;
        }
        else if (unpackOSC_IsNiceString(x, string, chars+n))
        {
            nextString = unpackOSC_DataAfterAlignedString(x, string, chars+n);
            debug("\"%s\" ", (i == 0 && skipComma) ? string +1 : string);
            SETSYMBOL(mya+myargc,gensym(string));
            myargc++;
            i += (nextString-string) / 4;
        }
        else
        {
            /* unhandled .. ;) */
            post("unpackOSC: PrintHeuristicallyTypeGuessedArgs: indeterminate type: 0x%x xx", ints[i]);
            i++;
        }
        *data_atc = myargc;
    }
}

#define STRING_ALIGN_PAD 4

static const char *unpackOSC_DataAfterAlignedString(t_unpackOSC *x, const char *string, const char *boundary)
{
    /* The argument is a block of data beginning with a string.  The
        string has (presumably) been padded with extra null characters
        so that the overall length is a multiple of STRING_ALIGN_PAD
        bytes.  Return a pointer to the next byte after the null
        byte(s).  The boundary argument points to the character after
        the last valid character in the buffer---if the string hasn't
        ended by there, something's wrong.

        If the data looks wrong, return 0 */

    int i;

    debug("unpackOSC_DataAfterAlignedString %p (boundary - string = %ld)\n",  string, boundary-string);
    if ((boundary - string) %4 != 0)
    {
        pd_error(x, "unpackOSC: DataAfterAlignedString: bad boundary");
        return 0;
    }

    for (i = 0; string[i] != '\0'; i++)
    {
        debug("%02X(%c) ",  string[i], string[i]);
        if (string + i >= boundary)
        {
            pd_error(x, "unpackOSC: DataAfterAlignedString: Unreasonably long string");
            return 0;
        }
    }

    /* Now string[i] is the first null character */
    debug("\nunpackOSC_DataAfterAlignedString first null character at %p\n",  &string[i]);
    i++;

    for (; (i % STRING_ALIGN_PAD) != 0; i++)
    {
        if (string + i >= boundary)
        {
            pd_error(x, "unpackOSC: DataAfterAlignedString: Unreasonably long string");
            return 0;
        }
        if (string[i] != '\0')
        {
            pd_error(x, "unpackOSC: DataAfterAlignedString: Incorrectly padded string");
            return 0;
        }
    }
    debug("unpackOSC_DataAfterAlignedString first non-null character at %p\n",  &string[i]);

    return string+i;
}

static int unpackOSC_IsNiceString(t_unpackOSC *x, const char *string, const char *boundary)
{
    /* Arguments same as DataAfterAlignedString().  Is the given "string"
       really an OSC-string?  I.e., is it
        "A sequence of non-null ASCII characters followed by a null, followed
        by 0-3 additional null characters to make the total number of bits a
        multiple of 32"? (OSC 1.0)
        Returns 1 if true, else 0. */

    int i;

    if ((boundary - string) %4 != 0)
    {
        pd_error(x, "unpackOSC: IsNiceString: bad boundary\n");
        return 0;
    }

    /* any non-zero byte will be accepted, so UTF-8 sequences will also be accepted -- not strictly OSC v1.0 */
    for (i = 0; string[i] != '\0'; i++)
        /* if ((!isprint(string[i])) || (string + i >= boundary)) return 0; */ /* only ASCII printable chars */
        /*if ((0==(string[i]&0xE0)) || (string + i >= boundary)) return 0;*/ /* onl;y ASCII space (0x20) and above */
        if (string + i >= boundary) return 0; /* anything non-zero */
    /* If we made it this far, it's a null-terminated sequence of characters
       within the given boundary.  Now we just make sure it's null padded... */

    /* Now string[i] is the first null character */
    i++;
    for (; (i % STRING_ALIGN_PAD) != 0; i++)
        if (string[i] != '\0') return 0;

    return 1;
}

/* end of unpackOSC.c */
