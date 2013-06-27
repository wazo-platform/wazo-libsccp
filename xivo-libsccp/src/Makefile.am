lib_LTLIBRARIES = libsccp.la
libsccp_includedir=./
libsccp_la_SOURCES = device.c main.c message.c sccp.c sccp_config.c
libsccp_include_HEADERS = device.h message.h sccp.h sccp_config.h
libsccp_la_CFLAGS = -Wall -Wextra -Wno-unused -Wundef -Wmissing-format-attribute -Wformat=2 -g -D_REETRANT -D_GNU_SOURCE -fPIC
libsccp_la_LDFLAGS = -Wl,--allow-multiple-definition -avoid-version
##-version-info 0:0:0
# -Wno-missing-field-initializers -Wno-unused -Wno-missing-braces
