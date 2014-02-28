TARGET = chan_sccp.so
OBJECTS = sccp.o sccp_debug.o sccp_config.o sccp_device.o sccp_device_registry.o \
	sccp_msg.o sccp_queue.o sccp_session.o sccp_server.o sccp_task.o
HEADERS = sccp.h sccp_debug.h sccp_config.h sccp_device.h sccp_device_registry.h \
	sccp_msg.h sccp_queue.h sccp_session.h sccp_server.h sccp_task.h \
	sccp_utils.h device/sccp_channel_tech.h device/sccp_rtp_glue.h
CFLAGS = -Wall -Wextra -Wno-unused-parameter -g -fPIC -D'_GNU_SOURCE' -D'AST_MODULE="chan_sccp"'
LDFLAGS = -Wall -shared

ifdef VERSION
	CFLAGS += -D'VERSION="$(VERSION)"'
endif

.PHONY: install clean

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

%.o: %.c $(HEADERS)
	$(CC) -c $(CFLAGS) -o $@ $<

install: $(TARGET)
	mkdir -p $(DESTDIR)/usr/lib/asterisk/modules
	install -m 644 $(TARGET) $(DESTDIR)/usr/lib/asterisk/modules/

clean:
	rm -f $(OBJECTS)
	rm -f $(TARGET)
