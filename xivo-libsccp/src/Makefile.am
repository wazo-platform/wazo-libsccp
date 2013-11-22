lib_LTLIBRARIES = libsccp.la
libsccp_includedir=./
libsccp_la_SOURCES = sccp_config.c sccp_debug.c sccp_device.c sccp_line.c sccp_message.c sccp_task.c sccp.c
libsccp_include_HEADERS = sccp_config.h sccp_debug.h sccp_device.h sccp_line.h sccp_message.h sccp_task.h sccp_utils.h sccp.h
libsccp_la_CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wundef -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Winit-self -Wmissing-format-attribute -Wformat=2 -g -D_GNU_SOURCE -fPIC
libsccp_la_LDFLAGS = -Wl,--allow-multiple-definition -avoid-version
##-version-info 0:0:0
# -Wno-missing-field-initializers -Wno-unused -Wno-missing-braces
