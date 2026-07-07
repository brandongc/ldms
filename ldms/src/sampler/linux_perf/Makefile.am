include $(top_srcdir)/ldms/rules.mk


pkglib_LTLIBRARIES =
dist_man7_MANS =

AM_CPPFLAGS = @OVIS_INCLUDE_ABS@
AM_LDFLAGS = @OVIS_LIB_ABS@
COMMON_LIBADD = -lsampler_base -lldms -lovis_util -lcoll \
		@LDFLAGS_GETTIME@

liblinux_perf_la_SOURCES = linux_perf.c
liblinux_perf_la_LIBADD = $(COMMON_LIBADD) -lm -lovis_json
pkglib_LTLIBRARIES += liblinux_perf.la
dist_man7_MANS += ldms-sampler_linux_perf.man

CLEANFILES = $(dist_man7_MANS)

EXTRA_DIST = ldms-sampler_linux_perf.rst
