# Autoconf
#

AC_PREREQ(2.61)
AC_INIT([xivo-libsccp], [m4_esyscmd([build-tools/print-version])])

AM_INIT_AUTOMAKE([-Wall -Werror foreign])
#LT_PREREQ([1.5])

AH_TOP([#include "config_undef.h"], [Remove PACKAGE_ stuff from the config.h])

AC_CONFIG_SRCDIR([src/sccp.c])
AC_CONFIG_HEADERS(config.h)
AC_DEFINE([AST_MODULE],["chan_sccp"],[Module name])

CFLAGS=-g
AC_PROG_CC
AC_PROG_LIBTOOL

AC_CONFIG_FILES([Makefile
		src/Makefile])

AC_OUTPUT
