# Copyright (C) 2016 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

include Makefile-decls.am

privdatadir=$(pkglibdir)

ACLOCAL_AMFLAGS += -I m4 ${ACLOCAL_FLAGS}
AM_CPPFLAGS += -DDATADIR='"$(datadir)"' \
	-DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR=\"$(datadir)/locale\"
AM_CFLAGS += $(WARN_CFLAGS)

libglnx_srcpath := $(srcdir)/libglnx
libglnx_cflags := $(BUILDDEP_GIO_UNIX_CFLAGS) "-I$(libglnx_srcpath)"
libglnx_libs := $(BUILDDEP_GIO_UNIX_LIBS)
include libglnx/Makefile-libglnx.am.inc
noinst_LTLIBRARIES += libglnx.la

include src/Makefile-inc.am
