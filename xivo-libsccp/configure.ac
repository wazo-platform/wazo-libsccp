# Autoconf
#

AC_PREREQ(2.61)
AC_INIT([SCCP library], [1.1beta], [nbouliane@avencall.com], [libsccp], [http://wiki.xivo.fr])

AM_INIT_AUTOMAKE([-Wall -Werror foreign])
#LT_PREREQ([1.5])

AC_CONFIG_SRCDIR([src/sccp.c])
AC_CONFIG_HEADERS(config.h)

CFLAGS=-g
AC_PROG_CC
AC_PROG_LIBTOOL

AC_CONFIG_FILES([Makefile
		src/Makefile])

AC_OUTPUT
