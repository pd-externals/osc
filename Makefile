# Makefile for osc

lib.name = osc

class.sources = \
        packOSC.c \
        pipelist.c \
        routeOSC.c \
        unpackOSC.c

datafiles = \
        LICENSE.txt \
        README.md \
        osc-meta.pd \
        packOSC-help.pd \
        packOSCstream-help.pd \
        packOSCstream.pd \
        pipelist-help.pd \
        routeOSC-help.pd \
        unpackOSC-help.pd \
        unpackOSCstream-help.pd \
        unpackOSCstream.pd

define forWindows
  ldlibs = -lwsock32
endef

cflags = -DHAVE_S_STUFF_H

# This Makefile is based on the Makefile from pd-lib-builder written by
# Katja Vetter. You can get it from:
# https://github.com/pure-data/pd-lib-builder

PDLIBBUILDER_DIR=pd-lib-builder/
include $(firstword $(wildcard $(PDLIBBUILDER_DIR)/Makefile.pdlibbuilder Makefile.pdlibbuilder))
