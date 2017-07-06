top_srcdir   = /home/dcslab/pxw/glusterfs-3.9.1/mtcp/apps/apache_benchmark
top_builddir = /home/dcslab/pxw/glusterfs-3.9.1/mtcp/apps/apache_benchmark
srcdir       = /home/dcslab/pxw/glusterfs-3.9.1/mtcp/apps/apache_benchmark/srclib/pcre
builddir     = /home/dcslab/pxw/glusterfs-3.9.1/mtcp/apps/apache_benchmark/srclib/pcre
VPATH        = /home/dcslab/pxw/glusterfs-3.9.1/mtcp/apps/apache_benchmark/srclib/pcre
LTLIBRARY_NAME = libpcre.la
LTLIBRARY_SOURCES = maketables.c get.c study.c pcre.c

CLEAN_TARGETS = dftables chartables.c
DISTCLEAN_TARGETS = pcre.h pcre-config config.h config.log config.status $(CLEAN_TARGETS)

include $(top_srcdir)/build/ltlib.mk

config.h:
	touch $@

$(LTLIBRARY_OBJECTS) dftables.lo: config.h

dftables: dftables.lo
	$(LINK) $(EXTRA_LDFLAGS) dftables.lo $(EXTRA_LIBS)

$(srcdir)/chartables.c: dftables
	./dftables $@

pcre.lo: $(srcdir)/chartables.c
