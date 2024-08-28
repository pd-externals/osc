/* packOSC is like sendOSC but outputs a list of floats which are the bytes making up the OSC packet. */
/* This allows for the separation of the protocol and its transport. */
/* Started by Martin Peach 20060403 */
/* 20060425 version independent of libOSC */
/* 20070620 added packOSC_path and packOSC_anything methods by zmoelnig */
/* 20160713 added 'm'  typetag for MIDI messages */
/* packOSC.c makes extensive use of code from OSC-client.c and sendOSC.c */
/* as well as some from OSC-timetag.c. These files have the following header: */
/*
Written by Matt Wright, The Center for New Music and Audio Technologies,
University of California, Berkeley.  Copyright (c) 1996,97,98,99,2000,01,02,03
The Regents of the University of California (Regents).

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

//define DEBUG

#define SC_BUFFER_SIZE 64000

#include "packingOSC.h"
#include "OSC_timeTag.h"

/* This is from OSC-client.h :*/
/*

   OSC-client.h: library for constructing OpenSoundControl messages.
   Derived from SynthControl.h
   Author: Matt Wright
   Version 0.1: 6/13/97
   Version 0.2: 7/21/2000: Support for type-tagged messages


   General notes:

   This library abstracts away the data format for the OpenSoundControl
   protocol.  Users of this library can construct OpenSoundControl packets
   with a function call interface instead of knowing how to lay out the bits.

   All issues of memory allocation are deferred to the user of this library.
   There are two data structures that the user must allocate.  The first
   is the actual buffer that the message will be written into.  This buffer
   can be any size, but if it's too small there's a possibility that it
   will become overfull.  The other data structure is called an OSCbuf,
   and it holds all the state used by the library as it's constructing
   a buffer.

   All procedures that have the possibility of an error condition return int,
   with 0 indicating no error and nonzero indicating an error.  The variable
   OSC_errorMessage will be set to point to a string containing an error
   message explaining what the problem is.

*/


/* Return the time tag 0x0000000000000001, indicating to the receiving device
   that it should process the message immediately. */
/* these are globals so other packOSCs will be in lockstep */
int packOSCs;
/* packOSCLogicalStartTime is Pd's count of DSP ticks. Use clock_gettimesince() to measure intervals in milliseconds from here */

/* Don't ever manipulate the data in the OSCbuf struct directly.  (It's
   declared here in the header file only so your program will be able to
   declare variables of type OSCbuf and have the right amount of memory
   be allocated.) */

typedef struct OSCbuf_struct
{
    char        *buffer; /* The buffer to hold the OSC packet */
    size_t      size; /* Size of the buffer */
    char        *bufptr; /* Current position as we fill the buffer */
    int         state; /* State of partially-constructed message */
    uint32_t    *thisMsgSize; /* Pointer to count field before */
                /* currently-being-written message */
    uint32_t    *prevCounts[MAX_BUNDLE_NESTING]; /* Pointers to count */
                /* field before each currently open bundle */
    int         bundleDepth; /* How many sub-sub-bundles are we in now? */
    char        *typeStringPtr; /* This pointer advances through the type */
                /* tag string as you add arguments. */
    int         gettingFirstUntypedArg; /* nonzero if this message doesn't have */
                /*  a type tag and we're waiting for the 1st arg */
} OSCbuf;

typedef struct
{
    enum {INT_osc, FLOAT_osc, STRING_osc, BLOB_osc, NOTYPE_osc} type;
    union
    {
        int   i;
        float f;
        const char  *s;
    } datum;
} typedArg;

/* Here are the possible values of the state field: */

#define EMPTY 0 /* Nothing written to packet yet */
#define ONE_MSG_ARGS 1 /* Packet has a single message; gathering arguments */
#define NEED_COUNT 2 /* Just opened a bundle; must write message name or */
                     /* open another bundle */
#define GET_ARGS 3 /* Getting arguments to a message.  If we see a message */
                     /* name or a bundle open/close then the current message */
                     /* will end. */
#define DONE 4 /* All open bundles have been closed, so can't write */
                     /* anything else */


static int OSC_strlen(const char *s);
static int OSC_padString(char *dest, const char *str);
static int OSC_padStringWithAnExtraStupidComma(char *dest, const char *str);
static int OSC_WriteStringPadding(char *dest, int i);
static int OSC_WriteBlobPadding(char *dest, int i);

static int CheckTypeTag(void *x, const OSCbuf *buf, char expectedType);

/* Initialize the given OSCbuf.  The user of this module must pass in the
   block of memory that this OSCbuf will use for a buffer, and the number of
   bytes in that block.  (It's the user's job to allocate the memory because
   you do it differently in different systems.) */
static void OSC_initBuffer(OSCbuf *buf, size_t size, char *byteArray);

/* Reset the given OSCbuf.  Do this after you send out the contents of
   the buffer and want to start writing new data into it. */
static void OSC_resetBuffer(OSCbuf *buf);

/* Is the buffer empty?  (I.e., would it be stupid to send the buffer
   contents to the synth?) */
static int OSC_isBufferEmpty(const OSCbuf *buf);

/* How much space is left in the buffer? */
static size_t OSC_freeSpaceInBuffer(const OSCbuf *buf);

/* Does the buffer contain a valid OSC packet?  (Returns nonzero if yes.) */
static int OSC_isBufferDone(const OSCbuf *buf);

/* When you're ready to send out the buffer (i.e., when OSC_isBufferDone()
   returns true), call these two procedures to get the OSC packet that's been
   assembled and its size in bytes.  (And then call OSC_resetBuffer() if you
   want to re-use this OSCbuf for the next packet.)  */
static char *OSC_getPacket(const OSCbuf *buf);
static int OSC_packetSize(const OSCbuf *buf);
static int OSC_CheckOverflow(void *x, const OSCbuf *buf, size_t bytesNeeded);

/* Here's the basic model for building up OSC messages in an OSCbuf:
    - Make sure the OSCbuf has been initialized with OSC_initBuffer().
    - To open a bundle, call OSC_openBundle().  You can then write
      messages or open new bundles within the bundle you opened.
      Call OSC_closeBundle() to close the bundle.  Note that a packet
      does not have to have a bundle; it can instead consist of just a
      single message.
    - For each message you want to send:
      - Call OSC_writeAddress() with the name of your message.  (In
        addition to writing your message name into the buffer, this
        procedure will also leave space for the size count of this message.)
      - Alternately, call OSC_writeAddressAndTypes() with the name of
        your message and with a type string listing the types of all the
        arguments you will be putting in this message.
      - Now write each of the arguments into the buffer, by calling one of:
        OSC_writeFloatArg()
        OSC_writeIntArg()
        OSC_writeStringArg()
        OSC_writeNullArg()
      - Now your message is complete; you can send out the buffer or you can
        add another message to it.
*/

static int OSC_openBundle(void *x, OSCbuf *buf, OSCTimeTag tt);
static int OSC_closeBundle(void *x, OSCbuf *buf);
static int OSC_writeAddress(void *x, OSCbuf *buf, char *name);
static int OSC_writeAddressAndTypes(void *x, OSCbuf *buf, char *name, char *types);
static int OSC_writeFloatArg(void *x, OSCbuf *buf, float arg);
static int OSC_writeIntArg(void *x, OSCbuf *buf, uint32_t arg);
static int OSC_writeBlobArg(void *x, OSCbuf *buf, typedArg *arg, size_t nArgs);
static int OSC_writeStringArg(void *x, OSCbuf *buf, const char *arg);
static int OSC_writeNullArg(void *x, OSCbuf *buf, char type);

/* How many bytes will be needed in the OSC format to hold the given
   string?  The length of the string, plus the null char, plus any padding
   needed for 4-byte alignment. */
static int OSC_effectiveStringLength(const char *string);

static t_class *packOSC_class;

typedef struct _packOSC
{
    t_object    x_obj;
    int         x_typetags; /* typetag flag */
    int         x_timeTagOffset;
    int         x_bundle; /* bundle open flag */
    OSCbuf      x_oscbuf[1]; /* OSCbuffer */
    t_outlet    *x_bdpthout; /* bundle-depth floatoutlet */
    t_outlet    *x_listout; /* OSC packet list ouput */
    size_t      x_buflength; /* number of elements in x_bufferForOSCbuf and x_bufferForOSClist */
    char        *x_bufferForOSCbuf; /*[SC_BUFFER_SIZE];*/
    t_atom      *x_bufferForOSClist; /*[SC_BUFFER_SIZE];*/
    const char  *x_prefix;
    int         x_reentry_count;
    int         x_use_pd_time;
    OSCTimeTag  x_pd_timetag;
    double      x_pd_timeref;
} t_packOSC;

static void *packOSC_new(void);
static void packOSC_path(t_packOSC *x, t_symbol*s);
static void packOSC_openbundle(t_packOSC *x);
static void packOSC_closebundle(t_packOSC *x);
static void packOSC_settypetags(t_packOSC *x, t_floatarg f);
static void packOSC_setbufsize(t_packOSC *x, t_floatarg f);
static void packOSC_usepdtime(t_packOSC *x, t_floatarg f);
static void packOSC_setTimeTagOffset(t_packOSC *x, t_floatarg f);
static void packOSC_sendtyped(t_packOSC *x, t_symbol *s, int argc, t_atom *argv);
static void packOSC_send_type_forced(t_packOSC *x, t_symbol *s, int argc, t_atom *argv);
static void packOSC_send(t_packOSC *x, t_symbol *s, int argc, t_atom *argv);
static void packOSC_anything(t_packOSC *x, t_symbol *s, int argc, t_atom *argv);
static void packOSC_free(t_packOSC *x);
void packOSC_setup(void);
static typedArg packOSC_parseatom(t_atom *a, t_packOSC *x);
static typedArg packOSC_packMIDI(t_atom *a, t_packOSC *x);
static typedArg packOSC_forceatom(t_atom *a, char ctype, t_packOSC *x);
static typedArg packOSC_blob(t_atom *a, t_packOSC *x);
static int packOSC_writetypedmessage(t_packOSC *x, OSCbuf *buf, char *messageName, int numArgs, typedArg *args, char *typeStr);
static int packOSC_writemessage(t_packOSC *x, OSCbuf *buf, char *messageName, int numArgs, typedArg *args);
static void packOSC_sendbuffer(t_packOSC *x);

static void *packOSC_new(void)
{
    t_packOSC *x = (t_packOSC *)pd_new(packOSC_class);
    x->x_typetags = 1; /* set typetags to 1 by default */
    x->x_bundle = 0; /* bundle is closed */
    x->x_buflength = SC_BUFFER_SIZE;
    x->x_bufferForOSCbuf = (char *)getbytes(sizeof(char)*x->x_buflength);
    if(x->x_bufferForOSCbuf == NULL)
    {
        pd_error(x, "packOSC: unable to allocate %lu bytes for x_bufferForOSCbuf", (long)(sizeof(char)*x->x_buflength));
        goto fail;
    }
    x->x_bufferForOSClist = (t_atom *)getbytes(sizeof(t_atom)*x->x_buflength);
    if(x->x_bufferForOSClist == NULL)
    {
        pd_error(x, "packOSC: unable to allocate %lu bytes for x_bufferForOSClist", (long)(sizeof(t_atom)*x->x_buflength));
        goto fail;
    }
    OSC_initBuffer(x->x_oscbuf, x->x_buflength, x->x_bufferForOSCbuf);
    x->x_listout = outlet_new(&x->x_obj, &s_list);
    x->x_bdpthout = outlet_new(&x->x_obj, &s_float);
    x->x_timeTagOffset = -1; /* immediately */
    x->x_reentry_count = 0;

    packOSC_usepdtime(x, 1.);
    return (x);
fail:
    if(x->x_bufferForOSCbuf != NULL) freebytes(x->x_bufferForOSCbuf, (long)(sizeof(char)*x->x_buflength));
    if(x->x_bufferForOSClist != NULL) freebytes(x->x_bufferForOSClist, (long)(sizeof(char)*x->x_buflength));
    return NULL;
}

static void packOSC_path(t_packOSC *x, t_symbol*s)
{
/* Set a default prefix to the OSC path */
    if(s == gensym(""))
    {
        x->x_prefix = 0;
        return;
    }
    if ((*s->s_name) != '/')
    {
        pd_error(x, "packOSC: bad path: '%s'", s->s_name);
        return;
    }
    x->x_prefix = s->s_name;
}

static void packOSC_openbundle(t_packOSC *x)
{
    int result;
    t_float bundledepth=(t_float)x->x_oscbuf->bundleDepth;
    OSCTimeTag tt = OSCTT_Immediately();

    if (x->x_timeTagOffset == -1) {
      /* immediately */
    } else {
      double delta = x->x_timeTagOffset*0.001;
      if (x->x_use_pd_time) {
          delta += clock_gettimesince(x->x_pd_timeref);
          tt = OSCTT_offsetms(x->x_pd_timetag, delta);
      } else {
          tt = OSCTT_offsetms(OSCTT_Now(), delta);
      }
    }
    result = OSC_openBundle(x, x->x_oscbuf, tt);
    if (result != 0)
    { /* reset the buffer */
        OSC_initBuffer(x->x_oscbuf, x->x_buflength, x->x_bufferForOSCbuf);
        x->x_bundle = 0;
    }
    else x->x_bundle = 1;
    outlet_float(x->x_bdpthout, bundledepth);
}

static void packOSC_closebundle(t_packOSC *x)
{
    t_float bundledepth=(t_float)x->x_oscbuf->bundleDepth;
    if (OSC_closeBundle(x, x->x_oscbuf))
    {
        pd_error(x, "packOSC: Problem closing bundle.");
        return;
    }
    outlet_float(x->x_bdpthout, bundledepth);
    /* in bundle mode we send when bundle is closed */
    if((!OSC_isBufferEmpty(x->x_oscbuf)) && OSC_isBufferDone(x->x_oscbuf))
    {
        x->x_bundle = 0; /* call this before _sendbuffer() to be ready for recursive calls */
        packOSC_sendbuffer(x);
        return;
    }
}

static void packOSC_settypetags(t_packOSC *x, t_floatarg f)
{
    x->x_typetags = (f != 0)?1:0;
    logpost(x, 3, "packOSC: setting typetags %d", x->x_typetags);
}

static void packOSC_setbufsize(t_packOSC *x, t_floatarg f)
{
    if (x->x_bufferForOSCbuf != NULL) freebytes((void *)x->x_bufferForOSCbuf, sizeof(char)*x->x_buflength);
    if (x->x_bufferForOSClist != NULL) freebytes((void *)x->x_bufferForOSClist, sizeof(t_atom)*x->x_buflength);
    logpost(x, 3, "packOSC: bufsize arg is %f (%lu)", f, (long)f);
    x->x_buflength = (long)f;
    x->x_bufferForOSCbuf = (char *)getbytes(sizeof(char)*x->x_buflength);
    if(x->x_bufferForOSCbuf == NULL)
        pd_error(x, "packOSC unable to allocate %lu bytes for x_bufferForOSCbuf", (long)(sizeof(char)*x->x_buflength));
    x->x_bufferForOSClist = (t_atom *)getbytes(sizeof(t_atom)*x->x_buflength);
    if(x->x_bufferForOSClist == NULL)
        pd_error(x, "packOSC unable to allocate %lu bytes for x_bufferForOSClist", (long)(sizeof(t_atom)*x->x_buflength));
    OSC_initBuffer(x->x_oscbuf, x->x_buflength, x->x_bufferForOSCbuf);
    logpost(x, 3, "packOSC: bufsize is now %ld", (long unsigned int)x->x_buflength);
}

static void packOSC_usepdtime(t_packOSC *x, t_floatarg f)
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

static void packOSC_setTimeTagOffset(t_packOSC *x, t_floatarg f)
{
    x->x_timeTagOffset = (int)f;
}
/* this is the real and only sending routine now, for both typed and */
/* undtyped mode. */

static void packOSC_sendtyped(t_packOSC *x, t_symbol *s, int argc, t_atom *argv)
{
    char            messageName[MAXPDSTRING];
    unsigned int    nTypeTags = 0, typeStrTotalSize = 0;
    unsigned int    argsSize = sizeof(typedArg)*argc;
    char*           typeStr = NULL; /* might not be used */
    typedArg*       args = (typedArg*)getbytes(argsSize);
    unsigned int    i, nTagsWithData, nArgs, blobCount;
    unsigned int    m, tagIndex, typedArgIndex, argvIndex;
    char            c;
    (void)s;

#ifdef DEBUG
    printf("*** packOSC_sendtyped bundle %d reentry %d\n", x->x_bundle, x->x_reentry_count);
#endif
    x->x_reentry_count++;
    if (args == NULL)
    {
        pd_error(x, "packOSC: unable to allocate %lu bytes for args", (long)argsSize);
        return;
    }
    messageName[0] = '\0'; /* empty */
    if(x->x_prefix) /* if there is a prefix, prefix it to the path */
    {
        size_t len = strlen(x->x_prefix);
        if(len >= MAXPDSTRING)
        len = MAXPDSTRING-1;

        strncpy(messageName, x->x_prefix, MAXPDSTRING);
        atom_string(&argv[0], messageName+len, (unsigned)(MAXPDSTRING-len));
    }
    else
        atom_string(&argv[0], messageName, MAXPDSTRING); /* the OSC address string */

    if (x->x_typetags & 2)
    { /* second arg is typestring */
        /* we need to find out how long the type string is before we copy it*/
        nTypeTags = (unsigned int)strlen(atom_getsymbol(&argv[1])->s_name);
        typeStrTotalSize = nTypeTags + 2;
        typeStr = (char*)getzbytes(typeStrTotalSize);
        if (typeStr == NULL)
        {
            pd_error(x, "packOSC: unable to allocate %u bytes for typeStr", nTypeTags);
            return;
        }
        typeStr[0] = ',';
        atom_string(&argv[1], &typeStr[1], typeStrTotalSize);
#ifdef DEBUG
        printf("packOSC_sendtyped typeStr: %s, nTypeTags %u\n", typeStr, nTypeTags);
#endif
        nArgs = argc-2;
        for (m = nTagsWithData = blobCount = 0; m < nTypeTags; ++m)
        {
#ifdef DEBUG
            printf("packOSC_sendtyped typeStr[%d] %c\n", m+1, typeStr[m+1]);
#endif
            if ((c = typeStr[m+1]) == 0) break;
            if (!(c == 'T' || c == 'F' || c == 'N' || c == 'I'))
            {
                ++nTagsWithData; /* anything other than these tags have at least one data byte */
                if (c == 'm') nTagsWithData += 3; // MIDI tag should have four data bytes
/*
    OSC-blob
    An int32 size count, followed by that many 8-bit bytes of arbitrary binary data,
    followed by 0-3 additional zero bytes to make the total number of bits a multiple of 32.
*/
                if (c == 'b') blobCount++; /* b probably has more than one byte, so set a flag */
            }
        }
        if (((blobCount == 0)&&(nTagsWithData != nArgs)) || ((blobCount != 0)&&(nTagsWithData > nArgs)))
        {
            pd_error(x, "packOSC: Tags count %d doesn't match argument count %d", nTagsWithData, nArgs);
            goto cleanup;
        }
        if (blobCount > 1)
        {
            pd_error(x, "packOSC: Only one blob per packet at the moment...");
            goto cleanup;
        }
        for (tagIndex = typedArgIndex = 0, argvIndex = 2; tagIndex < m; ++tagIndex) /* m is the number of tags */
        {
            c = typeStr[tagIndex+1];
            if (c == 'b')
            { /* A blob has to be the last item, until we get more elaborate. */
                if (tagIndex != m-1)
                {
                    pd_error(x, "packOSC: Since I don't know how big the blob is, Blob must be the last item in the list");
                    goto cleanup;
                }
                /* Pack all the remaining arguments as a blob */
                for (; typedArgIndex < nArgs; ++typedArgIndex, ++argvIndex)
                {
#ifdef DEBUG
                    printf("packOSC_blob %d:\n", nArgs);
#endif
                    args[typedArgIndex] = packOSC_blob(&argv[argvIndex], x);
                    /* Make sure it was blobbable */
                    if (args[typedArgIndex].type != BLOB_osc) goto cleanup;
                }
            }
            else if (!(c == 'T' || c == 'F' || c == 'N' || c == 'I')) /* not no data */
            {
                if (c == 'm')
                { // pack the next four arguments into one int
                  args[typedArgIndex++] = packOSC_packMIDI(&argv[argvIndex], x);
                  argvIndex += 4;
                }
                else args[typedArgIndex++] = packOSC_forceatom(&argv[argvIndex++], c, x);
            }
        }
        //if(packOSC_writetypedmessage(x, x->x_oscbuf, messageName, nArgs, args, typeStr))
        if(packOSC_writetypedmessage(x, x->x_oscbuf, messageName, typedArgIndex, args, typeStr))
        {
            pd_error(x, "packOSC: usage error, packOSC_writetypedmessage failed.");
            goto cleanup;
        }
    }
    else
    {
        for (i = 0; i < (unsigned)(argc-1); i++)
        {
            args[i] = packOSC_parseatom(&argv[i+1], x);
#ifdef DEBUG
            switch (args[i].type)
            {
                case INT_osc:
                    printf("packOSC: cell-cont: %d\n", args[i].datum.i);
                    break;
                case FLOAT_osc:
                    printf("packOSC: cell-cont: %f\n", args[i].datum.f);
                    break;
                case STRING_osc:
                    printf("packOSC: cell-cont: %s\n", args[i].datum.s);
                    break;
                case BLOB_osc:
                    printf("packOSC: blob\n");
                    break;
                case NOTYPE_osc:
                    printf("packOSC: unknown type\n");
                    break;

            }
            printf("packOSC:   type-id: %d\n", args[i].type);
#endif
        }
        if(packOSC_writemessage(x, x->x_oscbuf, messageName, i, args))
        {
            pd_error(x, "packOSC: usage error, packOSC_writemessage failed.");
            goto cleanup;
        }
    }

    if(!x->x_bundle)
    {
        packOSC_sendbuffer(x);
    }

cleanup:
    if (typeStr != NULL) freebytes(typeStr, typeStrTotalSize);
    if (args != NULL) freebytes(args, argsSize);
    x->x_reentry_count--;
}

static void packOSC_send_type_forced(t_packOSC *x, t_symbol *s, int argc, t_atom *argv)
{ /* typetags are the argument following the OSC path */
    x->x_typetags |= 2;/* tell packOSC_sendtyped to use the specified typetags... */
    packOSC_sendtyped(x, s, argc, argv);
    x->x_typetags &= ~2;/* ...this time only */
}

static void packOSC_send(t_packOSC *x, t_symbol *s, int argc, t_atom *argv)
{
    if(!argc)
    {
        pd_error(x, "packOSC: not sending empty message.");
        return;
    }
    packOSC_sendtyped(x, s, argc, argv);
}

static void packOSC_anything(t_packOSC *x, t_symbol *s, int argc, t_atom *argv)
{
/* If the message starts with '/', assume it's an OSC path and send it */
    t_atom*ap = 0;

    if ((*s->s_name)!='/')
    {
        pd_error(x, "packOSC: bad path: '%s'", s->s_name);
        return;
    }

    ap = (t_atom*)getbytes((argc+1)*sizeof(t_atom));
    SETSYMBOL(ap, s);
    memcpy(ap+1, argv, argc * sizeof(t_atom));

    packOSC_send(x, gensym("send"), argc+1, ap);

    freebytes(ap, (argc+1)*sizeof(t_atom));

}

static void packOSC_free(t_packOSC *x)
{
    if (x->x_bufferForOSCbuf != NULL) freebytes((void *)x->x_bufferForOSCbuf, sizeof(char)*x->x_buflength);
    if (x->x_bufferForOSClist != NULL) freebytes((void *)x->x_bufferForOSClist, sizeof(t_atom)*x->x_buflength);
}

void packOSC_setup(void)
{
    packOSC_class = class_new(gensym("packOSC"), (t_newmethod)packOSC_new,
        (t_method)packOSC_free,
        sizeof(t_packOSC), 0, A_DEFFLOAT, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_path,
        gensym("prefix"), A_DEFSYM, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_settypetags,
        gensym("typetags"), A_DEFFLOAT, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_setbufsize,
        gensym("bufsize"), A_DEFFLOAT, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_usepdtime,
        gensym("usepdtime"), A_FLOAT, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_setTimeTagOffset,
        gensym("timetagoffset"), A_DEFFLOAT, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_send,
        gensym("send"), A_GIMME, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_send,
        gensym("senduntyped"), A_GIMME, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_send_type_forced,
        gensym("sendtyped"), A_GIMME, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_openbundle,
        gensym("["), 0, 0);
    class_addmethod(packOSC_class, (t_method)packOSC_closebundle,
        gensym("]"), 0, 0);
    class_addanything(packOSC_class, (t_method)packOSC_anything);
}

static typedArg packOSC_parseatom(t_atom *a, t_packOSC *x)
{
    typedArg returnVal;
    t_float  f;
    int      i;
    t_symbol s;
    char     buf[MAXPDSTRING];

    atom_string(a, buf, MAXPDSTRING);
#ifdef DEBUG
    printf("packOSC: atom type %d (%s)\n", a->a_type, buf);
#endif
    /* It might be an int, a float, or a string */
    switch (a->a_type)
    {
        case A_FLOAT:
            f = atom_getfloat(a);
            i = atom_getint(a);
            if (f == (t_float)i)
            { /* assume that if the int and float are the same, it's an int */
                returnVal.type = INT_osc;
                returnVal.datum.i = i;
            }
            else
            {
                returnVal.type = FLOAT_osc;
                returnVal.datum.f = f;
            }
            return returnVal;
        case A_SYMBOL:
            s = *atom_getsymbol(a);
            returnVal.type = STRING_osc;
            returnVal.datum.s = s.s_name;
            return returnVal;
        default:
            atom_string(a, buf, MAXPDSTRING);
            pd_error(x, "packOSC: atom type %d not implemented (%s)", a->a_type, buf);
            returnVal.type = NOTYPE_osc;
            returnVal.datum.s = NULL;
            return returnVal;
    }
}

static typedArg packOSC_blob(t_atom *a, t_packOSC *x)
{ /* ctype is one of i,f,s,T,F,N,I*/
    typedArg    returnVal;
    t_float     f;
    int         i;

    returnVal.type = NOTYPE_osc;
    returnVal.datum.s = NULL;
    /* the atoms must all be bytesl */
    if(a->a_type != A_FLOAT)
    {
        pd_error(x, "packOSC_blob: all values must be floats");
        return returnVal;
    }
    f = atom_getfloat(a);
    i = (int)f;
    if (i != f)
    {
        pd_error(x, "packOSC_blob: all values must be whole numbers");
        return returnVal;
    }
    if ((i < -128) || (i > 255))
    {
        pd_error(x, "packOSC_blob: all values must be bytes");
        return returnVal;
    }
    returnVal.type = BLOB_osc;
    returnVal.datum.i = i;
    return returnVal;
}

static typedArg packOSC_packMIDI(t_atom *a, t_packOSC *x)
{ /* pack four bytes at a into one int32 */
  int         i;
  typedArg    returnVal;
  int         m[4];

  for (i = 0; i < 4; ++i, ++a)
  {
#ifdef DEBUG
    atom_string(a, buf, MAXPDSTRING);
    printf("packOSC: atom type %d (%s)\n", a->a_type, buf);
#endif
    if ((a->a_type != A_FLOAT))
    {
      pd_error(x, "packOSC: MIDI parameters must be floats");
      returnVal.type = NOTYPE_osc;
      returnVal.datum.s = NULL;
      return returnVal;
    }
    m[i] = atom_getint(a);
#ifdef DEBUG
    printf("packOSC_packMIDI: float to integer %d\n", m[i]);
#endif
    if ((i < 2) && (m[i] != (m[i] & 0x0FF)))
    {
      pd_error(x, "packOSC: MIDI parameters must be less than 256");
      returnVal.type = NOTYPE_osc;
      returnVal.datum.s = NULL;
      return returnVal;
    }
    else if ((i > 1) && (m[i] != (m[i] & 0x07F)))
    {
      pd_error(x, "packOSC: MIDI parameters must be less than 128");
      returnVal.type = NOTYPE_osc;
      returnVal.datum.s = NULL;
      return returnVal;
    }
  }
  returnVal.type = INT_osc;
  returnVal.datum.i = (m[0]<<24) + (m[1]<<16) + (m[2]<<8) + m[3];
  return returnVal;
}

static typedArg packOSC_forceatom(t_atom *a, char ctype, t_packOSC *x)
{ /* ctype is one of i,f,s,T,F,N,I*/
    typedArg    returnVal;
    t_float     f;
    int         i;
    t_symbol    s;
    static char buf[MAXPDSTRING];

#ifdef DEBUG
    atom_string(a, buf, MAXPDSTRING);
    printf("packOSC: atom type %d (%s)\n", a->a_type, buf);
#endif
    /* the atom might be a float, or a symbol */
    switch (a->a_type)
    {
        case A_FLOAT:
            switch (ctype)
            {
                case 'i':
                    returnVal.type = INT_osc;
                    returnVal.datum.i = atom_getint(a);
#ifdef DEBUG
                    printf("packOSC_forceatom: float to integer %d\n", returnVal.datum.i);
#endif
                    break;
                case 'f':
                    returnVal.type = FLOAT_osc;
                    returnVal.datum.f = atom_getfloat(a);
#ifdef DEBUG
                    printf("packOSC_forceatom: float to float %f\n", returnVal.datum.f);
#endif
                    break;
                case 's':
                    f = atom_getfloat(a);
                    sprintf(buf, "%f", f);
                    returnVal.type = STRING_osc;
                    returnVal.datum.s = buf;
#ifdef DEBUG
                    printf("packOSC_forceatom: float to string %s\n", returnVal.datum.s);
#endif
                    break;
                default:
                    pd_error(x, "packOSC: unknown OSC type %c", ctype);
                    returnVal.type = NOTYPE_osc;
                    returnVal.datum.s = NULL;
                    break;
            }
            break;
        case A_SYMBOL:
            s = *atom_getsymbol(a);
            switch (ctype)
            {
                case 'i':
                    i = atoi(s.s_name);
                    returnVal.type = INT_osc;
                    returnVal.datum.i = i;
#ifdef DEBUG
                    printf("packOSC_forceatom: symbol to integer %d\n", returnVal.datum.i);
#endif
                    break;
                case 'f':
                    f = atof(s.s_name);
                    returnVal.type = FLOAT_osc;
                    returnVal.datum.f = f;
#ifdef DEBUG
                    printf("packOSC_forceatom: symbol to float %f\n", returnVal.datum.f);
#endif
                    break;
                case 's':
                    returnVal.type = STRING_osc;
                    returnVal.datum.s = s.s_name;
#ifdef DEBUG
                    printf("packOSC_forceatom: symbol to string %s\n", returnVal.datum.s);
#endif
                    break;
                default:
                    pd_error(x, "packOSC: unknown OSC type %c", ctype);
                    returnVal.type = NOTYPE_osc;
                    returnVal.datum.s = NULL;
                    break;
            }
            break;
        default:
            atom_string(a, buf, MAXPDSTRING);
            pd_error(x, "packOSC: atom type %d not implemented (%s)", a->a_type, buf);
            returnVal.type = NOTYPE_osc;
            returnVal.datum.s = NULL;
            break;
    }
    return returnVal;
}

static int packOSC_writetypedmessage
(t_packOSC *x, OSCbuf *buf, char *messageName, int numArgs, typedArg *args, char *typeStr)
{
    int i, j, returnVal;

#ifdef DEBUG
    printf("packOSC_writetypedmessage: messageName %p (%s) typeStr %p (%s)\n",
        messageName, messageName, typeStr, typeStr);
#endif
    returnVal = OSC_writeAddressAndTypes(x, buf, messageName, typeStr);

    if (returnVal)
    {
        pd_error(x, "packOSC: Problem writing address. (%d)", returnVal);
        return returnVal;
    }
    for (j = i = 0; (typeStr[i+1]!= 0) || (j < numArgs); j++, i++)
    {
        while (typeStr[i+1] == 'T' || typeStr[i+1] == 'F' || typeStr[i+1] == 'I' || typeStr[i+1] == 'N')
        {
#ifdef DEBUG
            printf("packOSC_writetypedmessage: NULL [%c]\n", typeStr[i+1]);
#endif
            returnVal = OSC_writeNullArg(x, buf, typeStr[i+1]);
            ++i;
        }
        if (j < numArgs)
        {
            switch (args[j].type)
            {
                case INT_osc:
#ifdef DEBUG
                    printf("packOSC_writetypedmessage: int [%d]\n", args[j].datum.i);
#endif
                    returnVal = OSC_writeIntArg(x, buf, args[j].datum.i);
                    break;
                case FLOAT_osc:
#ifdef DEBUG
                    printf("packOSC_writetypedmessage: float [%f]\n", args[j].datum.f);
#endif
                    returnVal = OSC_writeFloatArg(x, buf, args[j].datum.f);
                    break;
                case STRING_osc:
#ifdef DEBUG
                    printf("packOSC_writetypedmessage: string [%s]\n", args[j].datum.s);
#endif
                    returnVal = OSC_writeStringArg(x, buf, args[j].datum.s);
                    break;
                case BLOB_osc:
                    /* write all the blob elements at once */
#ifdef DEBUG
                    printf("packOSC_writetypedmessage calling OSC_writeBlobArg\n");
#endif
                    return OSC_writeBlobArg(x, buf, &args[j], numArgs-j);
                default:

                    break; /* types with no data */
            }
        }
    }
    return returnVal;
}

static int packOSC_writemessage(t_packOSC *x, OSCbuf *buf, char *messageName, int numArgs, typedArg *args)
{
    int j, returnVal = 0, numTags;
#ifdef DEBUG
    printf("packOSC_writemessage buf %p bufptr %p messageName %s %d args typetags %d\n", buf, buf->bufptr, messageName, numArgs, x->x_typetags);
#endif

    if (!x->x_typetags)
    {
#ifdef DEBUG
        printf("packOSC_writemessage calling OSC_writeAddress with x->x_typetags %d\n", x->x_typetags);
#endif
        returnVal = OSC_writeAddress(x, buf, messageName);
        if (returnVal)
        {
            pd_error(x, "packOSC: Problem writing address.");
        }
    }
    else
    {
        char *typeTags;

        /* First figure out the type tags */
        for (numTags = 0; numTags < numArgs; numTags++)
        {
            if (args[numTags].type == BLOB_osc) break; /* blob has one type tag and is the last element */
        }
        typeTags=(char*)getbytes(sizeof(char)*(numTags+2)); /* number of args + ',' + '\0' */

        typeTags[0] = ',';
        for (j = 0; j < numTags; ++j)
        {
            switch (args[j].type)
            {
                case INT_osc:
                    typeTags[j+1] = 'i';
                    break;
                case FLOAT_osc:
                    typeTags[j+1] = 'f';
                    break;
                case STRING_osc:
                    typeTags[j+1] = 's';
                    break;
                case BLOB_osc:
                    typeTags[j+1] = 'b';
                    break;
                default:
                    pd_error(x, "packOSC: arg %d type is unrecognized(%d)", j, args[j].type);
                    break;
            }
        }
        typeTags[j+1] = '\0';
#ifdef DEBUG
        printf("packOSC_writemessage calling OSC_writeAddressAndTypes with x->x_typetags %d typeTags %p (%s)\n", x->x_typetags, typeTags, typeTags);
#endif
        returnVal = OSC_writeAddressAndTypes(x, buf, messageName, typeTags);
        if (returnVal)
        {
            pd_error(x, "packOSC: Problem writing address.");
        }
        freebytes(typeTags, sizeof(char)*(numTags+2));
    }
    for (j = 0; j < numArgs; j++)
    {
        switch (args[j].type)
        {
            case INT_osc:
                returnVal = OSC_writeIntArg(x, buf, args[j].datum.i);
                break;
            case FLOAT_osc:
                returnVal = OSC_writeFloatArg(x, buf, args[j].datum.f);
                break;
            case STRING_osc:
                returnVal = OSC_writeStringArg(x, buf, args[j].datum.s);
                break;
            case BLOB_osc:
#ifdef DEBUG
                printf("packOSC_writemessage calling OSC_writeBlobArg\n");
#endif
                return OSC_writeBlobArg(x, buf, &args[j], numArgs-j); /* All the remaining args are blob */
            default:
                break; /* just skip bad types (which we won't get anyway unless this code is buggy) */
        }
    }
    return returnVal;
}

static void packOSC_sendbuffer(t_packOSC *x)
{
    int             i;
    int             length;
    unsigned char   *buf;
    int             reentry_count=x->x_reentry_count;      /* must be on stack for recursion */
    size_t          bufsize=sizeof(t_atom)*x->x_buflength; /* must be on stack for recursion */
    t_atom          *atombuffer=x->x_bufferForOSClist;     /* must be on stack in the case of recursion */

    if(reentry_count>0) /* if we are recurse, let's move atombuffer to the stack */
        atombuffer=(t_atom *)getbytes(bufsize);

    if(!atombuffer) {
        pd_error(x, "packOSC: unable to allocate %lu bytes for atombuffer", (long)bufsize);
        return;
    }

#ifdef DEBUG
    printf("packOSC_sendbuffer: Sending buffer...\n");
#endif
    if (OSC_isBufferEmpty(x->x_oscbuf))
    {
        pd_error(x, "packOSC_sendbuffer() called but buffer empty");
        return;
    }
    if (!OSC_isBufferDone(x->x_oscbuf))
    {
        pd_error(x, "packOSC_sendbuffer() called but buffer not ready!, not exiting");
        return;
    }
    length = OSC_packetSize(x->x_oscbuf);
    buf = (unsigned char *)OSC_getPacket(x->x_oscbuf);
#ifdef DEBUG
    printf("packOSC_sendbuffer: length: %u\n", length);
#endif

    /* convert the bytes in the buffer to floats in a list */
    for (i = 0; i < length; ++i) SETFLOAT(&atombuffer[i], buf[i]);

    /* cleanup the OSCbuffer structure (so we are ready for recursion) */
    OSC_initBuffer(x->x_oscbuf, x->x_buflength, x->x_bufferForOSCbuf);

    /* send the list out the outlet */
    outlet_list(x->x_listout, &s_list, length, atombuffer);

    /* cleanup our 'stack'-allocated atombuffer in the case of reentrancy */
    if(reentry_count>0)
        freebytes(atombuffer, bufsize);
}

/* The next part is copied and morphed from OSC-client.c. */
/*
  Author: Matt Wright
  Version 2.2: Calls htonl in the right places 20000620
  Version 2.3: Gets typed messages right.
 */

/*
    pd
    -------------

    raf@interaccess.com:
    rev. for Win32 build  (verified under Win-2ooo) 11-April-2002

    -- changed licence part (20040820) jdl
    -- Version 2.4 changes not in here (20040820) jdl

*/


static void OSC_initBuffer(OSCbuf *buf, size_t size, char *byteArray)
{
    buf->buffer = byteArray;
    buf->size = size;
    OSC_resetBuffer(buf);
}

static void OSC_resetBuffer(OSCbuf *buf)
{
    buf->bufptr = buf->buffer;
    buf->state = EMPTY;
    buf->bundleDepth = 0;
    buf->prevCounts[0] = 0;
    buf->gettingFirstUntypedArg = 0;
    buf->typeStringPtr = 0;
}

static int OSC_isBufferEmpty(const OSCbuf *buf)
{
    return buf->bufptr == buf->buffer;
}

static size_t OSC_freeSpaceInBuffer(const OSCbuf *buf)
{
    return buf->size - (buf->bufptr - buf->buffer);
}

static int OSC_isBufferDone(const OSCbuf *buf)
{
    return (buf->state == DONE || buf->state == ONE_MSG_ARGS);
}

static char *OSC_getPacket(const OSCbuf *buf)
{
    return buf->buffer;
}

static int OSC_packetSize(const OSCbuf *buf)
{
    return (buf->bufptr - buf->buffer);
}

static int OSC_CheckOverflow(void *x, const OSCbuf *buf, size_t bytesNeeded)
{
    if ((bytesNeeded) > OSC_freeSpaceInBuffer(buf))
    {
        pd_error(x, "packOSC: buffer overflow");
        return 1;
    }
    return 0;
}

static void PatchMessageSize(OSCbuf *buf)
{
    uint32_t size = buf->bufptr - ((char *) buf->thisMsgSize) - 4;
    *(buf->thisMsgSize) = htonl(size);
}

static int OSC_openBundle(void *x, OSCbuf *buf, OSCTimeTag tt)
{
    if (buf->state == ONE_MSG_ARGS)
    {
        pd_error(x, "packOSC: Can't open a bundle in a one-message packet");
        return 3;
    }

    if (buf->state == DONE)
    {
        pd_error(x, "packOSC: This packet is finished; can't open a new bundle");
        return 4;
    }

    if (++(buf->bundleDepth) >= MAX_BUNDLE_NESTING)
    {
        pd_error(x, "packOSC: Bundles nested too deeply: maybe change MAX_BUNDLE_NESTING from %d and recompile", MAX_BUNDLE_NESTING);
        return 2;
    }

    if (CheckTypeTag(x, buf, '\0')) return 9;

    if (buf->state == GET_ARGS)
    {
        PatchMessageSize(buf);
    }

    if (buf->state == EMPTY)
    {
        /* Need 16 bytes for "#bundle" and time tag */
        if(OSC_CheckOverflow(x, buf, 16)) return 1;
    }
    else
    {
        /* This bundle is inside another bundle, so we need to leave
          a blank size count for the size of this current bundle. */
        if(OSC_CheckOverflow(x, buf, 20))return 1;
        *((uint32_t *)buf->bufptr) = 0xaaaaaaaa;
        buf->prevCounts[buf->bundleDepth] = (uint32_t *)buf->bufptr;

        buf->bufptr += 4;
    }

    buf->bufptr += OSC_padString(buf->bufptr, "#bundle");


    *((OSCTimeTag *) buf->bufptr) = tt;

    if (htonl(1) != 1)
    {
        /* Byte swap the 8-byte integer time tag */
        uint32_t *intp = (uint32_t *)buf->bufptr;
        intp[0] = htonl(intp[0]);
        intp[1] = htonl(intp[1]);

        /* tt  is a struct of two 32-bit words, and even though
          each word was wrong-endian, they were in the right order
          in the struct.) */
    }

    buf->bufptr += sizeof(OSCTimeTag);

    buf->state = NEED_COUNT;

    buf->gettingFirstUntypedArg = 0;
    buf->typeStringPtr = 0;
    return 0;
}


static int OSC_closeBundle(void *x, OSCbuf *buf)
{
    if (buf->bundleDepth == 0)
    {
        /* This handles EMPTY, ONE_MSG, ARGS, and DONE */
        pd_error(x, "packOSC: Can't close bundle: no bundle is open!");
        return 5;
    }

    if (CheckTypeTag(x, buf, '\0')) return 9;

    if (buf->state == GET_ARGS)
    {
        PatchMessageSize(buf);
    }

    if (buf->bundleDepth == 1)
    {
        /* Closing the last bundle: No bundle size to patch */
        buf->state = DONE;
    }
    else
    {
        /* Closing a sub-bundle: patch bundle size */
        int size = buf->bufptr - ((char *) buf->prevCounts[buf->bundleDepth]) - 4;
        *(buf->prevCounts[buf->bundleDepth]) = htonl(size);
        buf->state = NEED_COUNT;
    }

    --buf->bundleDepth;
    buf->gettingFirstUntypedArg = 0;
    buf->typeStringPtr = 0;
    return 0;
}

static int OSC_writeAddress(void *x, OSCbuf *buf, char *name)
{
    uint32_t paddedLength;
#ifdef DEBUG
    printf("-->OSC_writeAddress buf %p bufptr %p name %s\n", buf, buf->bufptr, name);
#endif
    if (buf->state == ONE_MSG_ARGS)
    {
        pd_error(x, "packOSC: This packet is not a bundle, so you can't write another address");
        return 7;
    }

    if (buf->state == DONE)
    {
        pd_error(x, "packOSC: This packet is finished; can't write another address");
        return 8;
    }

    if (CheckTypeTag(x, buf, '\0')) return 9;

    paddedLength = OSC_effectiveStringLength(name);
#ifdef DEBUG
    printf("OSC_writeAddress paddedLength %d\n", paddedLength);
#endif

    if (buf->state == EMPTY)
    {
        /* This will be a one-message packet, so no sizes to worry about */
        if(OSC_CheckOverflow(x, buf, paddedLength))return 1;
        buf->state = ONE_MSG_ARGS;
    }
    else
    {
        /* GET_ARGS or NEED_COUNT */
        if(OSC_CheckOverflow(x, buf, 4+paddedLength))return 1;
        if (buf->state == GET_ARGS)
        {
            /* Close the old message */
            PatchMessageSize(buf);
        }
        buf->thisMsgSize = (uint32_t *)buf->bufptr;
        *(buf->thisMsgSize) = 0xbbbbbbbb;
        buf->bufptr += 4;
        buf->state = GET_ARGS;
    }

    /* Now write the name */
    buf->bufptr += OSC_padString(buf->bufptr, name);
    buf->typeStringPtr = 0;
    buf->gettingFirstUntypedArg = 1;

    return 0;
}

static int OSC_writeAddressAndTypes(void *x, OSCbuf *buf, char *name, char *types)
{
    int      result;
    uint32_t paddedLength;

#ifdef DEBUG
    printf("OSC_writeAddressAndTypes buf %p name %s types %s\n", buf, name, types);
#endif
    if (buf == NULL) return 10;
    if (CheckTypeTag(x, buf, '\0')) return 9;

    result = OSC_writeAddress(x, buf, name);

    if (result) return result;

    paddedLength = OSC_effectiveStringLength(types);

    if(OSC_CheckOverflow(x, buf, paddedLength))return 1;

    buf->typeStringPtr = buf->bufptr + 1; /* skip comma */
    buf->bufptr += OSC_padString(buf->bufptr, types);
#ifdef DEBUG
    printf("OSC_writeAddressAndTypes buf->typeStringPtr now %p (%s) buf->bufptr now %p (%s)\n",
        buf->typeStringPtr, buf->typeStringPtr, buf->bufptr, buf->bufptr);
#endif

    buf->gettingFirstUntypedArg = 0;
    buf->typeStringPtr = 0;// ready for a new type string
    return 0;
}

static int CheckTypeTag(void *x, const OSCbuf *buf, char expectedType)
{
    char c;
    const char*typeStringPtr = buf->typeStringPtr;

    if (typeStringPtr)
    {
#ifdef DEBUG
        printf("CheckTypeTag typeStringPtr %p (%s)\n", typeStringPtr, typeStringPtr);
#endif
        c = *typeStringPtr;
#ifdef DEBUG
        printf("CheckTypeTag buf %p expectedType %c c is %c\n", buf, expectedType, c);
#endif
        if (c != expectedType)
        {
            if (expectedType == '\0')
            {
                pd_error(x, "packOSC: According to the type tag (%c) I expected more arguments.", c);
            }
            else if (*typeStringPtr == '\0')
            {
                pd_error(x, "packOSC: According to the type tag I didn't expect any more arguments.");
            }
            else
            {
                pd_error(x, "packOSC: According to the type tag I expected an argument of a different type.");
                pd_error(x, "* Expected %c, string now %s\n", expectedType, typeStringPtr);
            }
            return 9;
        }
        ++typeStringPtr;
    }
    return 0;
}

static int OSC_writeFloatArg(void *x, OSCbuf *buf, float arg)
{
    union intfloat32
    {
        int     i;
        float   f;
    };
    union intfloat32 if32;

    if(OSC_CheckOverflow(x, buf, 4))return 1;

    if (CheckTypeTag(x, buf, 'f')) return 9;

    /* Pretend arg is a long int so we can use htonl() */
    if32.f = arg;

    *((uint32_t *) buf->bufptr) = htonl(if32.i);

    buf->bufptr += 4;

    buf->gettingFirstUntypedArg = 0;
    return 0;
}

static int OSC_writeIntArg(void *x, OSCbuf *buf, uint32_t arg)
{
    if(OSC_CheckOverflow(x, buf, 4))return 1;
    if (CheckTypeTag(x, buf, 'i')) return 9;

    *((uint32_t *) buf->bufptr) = htonl(arg);
    buf->bufptr += 4;

    buf->gettingFirstUntypedArg = 0;
    return 0;
}

static int OSC_writeBlobArg(void *x, OSCbuf *buf, typedArg *arg, size_t nArgs)
{
    size_t i;
    unsigned char b;

/* pack all the args as single bytes following a 4-byte length */
    if(OSC_CheckOverflow(x, buf, nArgs+4))return 1;
    if (CheckTypeTag(x, buf, 'b')) return 9;

    *((uint32_t *) buf->bufptr) = htonl(nArgs);
#ifdef DEBUG
    printf("OSC_writeBlobArg length : %lu\n", nArgs);
#endif
    buf->bufptr += 4;

    for (i = 0; i < nArgs; i++)
    {
        if (arg[i].type != BLOB_osc)
        {
            pd_error(x, "packOSC: blob element %lu not blob type", (long unsigned)i);
            return 9;
        }
        b = (unsigned char)((arg[i].datum.i)&0x0FF);/* force int to 8-bit byte */
#ifdef DEBUG
        printf("OSC_writeBlobArg : %d, %d\n", arg[i].datum.i, b);
#endif
        buf->bufptr[i] = b;
    }
    i = OSC_WriteBlobPadding(buf->bufptr, i);
    buf->bufptr += i;
    buf->gettingFirstUntypedArg = 0;
    return 0;
}

static int OSC_writeStringArg(void *x, OSCbuf *buf, const char *arg)
{
    int len;

    if (CheckTypeTag(x, buf, 's')) return 9;

    len = OSC_effectiveStringLength(arg);

    if (buf->gettingFirstUntypedArg && arg[0] == ',')
    {
        /* This un-type-tagged message starts with a string
          that starts with a comma, so we have to escape it
          (with a double comma) so it won't look like a type
          tag string. */

        if(OSC_CheckOverflow(x, buf, len+4))return 1; /* Too conservative */
        buf->bufptr +=
        OSC_padStringWithAnExtraStupidComma(buf->bufptr, arg);

    }
    else
    {
        if(OSC_CheckOverflow(x, buf, len))return 1;
        buf->bufptr += OSC_padString(buf->bufptr, arg);
    }

    buf->gettingFirstUntypedArg = 0;
    return 0;

}

static int OSC_writeNullArg(void *x, OSCbuf *buf, char type)
{ /* Don't write any data, just check the type tag */
    if(OSC_CheckOverflow(x, buf, 4))return 1;
    if (CheckTypeTag(x, buf, type)) return 9;
    buf->gettingFirstUntypedArg = 0;
    return 0;
}

/* String utilities */

static int OSC_strlen(const char *s)
{
    int i;
    for (i = 0; s[i] != '\0'; i++) /* Do nothing */ ;
    return i;
}

#define STRING_ALIGN_PAD 4
static int OSC_effectiveStringLength(const char *string)
{
    int len = OSC_strlen(string) + 1;  /* We need space for the null char. */

    /* Round up len to next multiple of STRING_ALIGN_PAD to account for alignment padding */
    if ((len % STRING_ALIGN_PAD) != 0)
    {
        len += STRING_ALIGN_PAD - (len % STRING_ALIGN_PAD);
    }
    return len;
}

static int OSC_padString(char *dest, const char *str)
{
    int i;

    for (i = 0; str[i] != '\0'; i++) dest[i] = str[i];

    return OSC_WriteStringPadding(dest, i);
}

static int OSC_padStringWithAnExtraStupidComma(char *dest, const char *str)
{
    int i;

    dest[0] = ',';
    for (i = 0; str[i] != '\0'; i++) dest[i+1] = str[i];

    return OSC_WriteStringPadding(dest, i+1);
}

static int OSC_WriteStringPadding(char *dest, int i)
{
    /* pad with at least one zero to fit 4-byte */
    dest[i] = '\0';
    i++;

    for (; (i % STRING_ALIGN_PAD) != 0; i++) dest[i] = '\0';

    return i;
}

static int OSC_WriteBlobPadding(char *dest, int i)
{
    /* pad if necessary to fit 4-byte */
    for (; (i % STRING_ALIGN_PAD) != 0; i++) dest[i] = '\0';
    return i;
}
/* end packOSC.c*/
