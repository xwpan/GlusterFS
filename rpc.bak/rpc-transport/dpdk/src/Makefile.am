# TODO : need to change transportdir
include $(RTE_SDK)/mk/rte.vars.mk

transport_LTLIBRARIES = dpdk.la
transportdir = $(libdir)/glusterfs/$(PACKAGE_VERSION)/rpc-transport

dpdk_la_LDFLAGS = -module -avoid-version -nostartfiles

dpdk_la_SOURCES = dpdk.c
dpdk_la_LIBADD = $(top_builddir)/libglusterfs/src/libglusterfs.la 

DPDK_MACHINE_FLAGS = $(shell cat /home/dcslab/pxw/glusterfs-3.9.1/mtcp/dpdk/include/cflags.txt)
DPDK_LIB_FLAGS = $(shell cat /home/dcslab/pxw/glusterfs-3.9.1/mtcp/dpdk/lib/ldflags.txt)
DPDK_INC = /home/dcslab/pxw/glusterfs-3.9.1/mtcp/dpdk/include

LIBS += -m64 -g -O3 -pthread -lrt -march=native -export-dynamic  -L/home/dcslab/pxw/glusterfs-3.9.1/mtcp/mtcp/lib/libmtcp.a  -L/home/dcslab/pxw/glusterfs-3.9.1/mtcp/dpdk/lib  -lmtcp -lpthread -lssl -L/lib/libnuma.so -lnuma  -lrt -ldl  ${DPDK_LIB_FLAGS}

noinst_HEADERS = dpdk.h  debug.h


AM_CPPFLAGS = $(GF_CPPFLAGS) \
	-I$(top_srcdir)/libglusterfs/src -I$(top_srcdir)/rpc/rpc-lib/src/  -I$(top_srcdir)/mtcp/mtcp/include   \
	 ${DPDK_MACHINE_FLAGS} -I${DPDK_INC} -include $(DPDK_INC)/rte_config.h  -I$(top_srcdir)/mtcp/util/include	\
	-I$(top_srcdir)/rpc/xdr/src -I$(top_srcdir)/mtcp/dpdk-16.11/x86_64-native-linuxapp-gcc/include

AM_CFLAGS = -Wall $(GF_CFLAGS)

CLEANFILES = *~


