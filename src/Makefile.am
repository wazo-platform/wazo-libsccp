
lib_LTLIBRARIES = libsccp.la

libsccp_la_SOURCES = device.c main.c message.c sccp.c \
			device.h message.h sccp.h

libsccp_la_CFLAGS = -Wall -Wextra -D_REETRANT -D_GNU_SOURCE -fPIC
libsccp_la_LDFLAGS = -Wl,--allow-multiple-definition -avoid-version

#-version-info 0:0:0
