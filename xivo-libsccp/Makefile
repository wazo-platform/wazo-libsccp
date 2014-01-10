TARGET = chan_sccp.so
OBJECTS = sccp.o sccp_config.o sccp_device.o sccp_queue.o sccp_session.o sccp_server.o
HEADERS = sccp_config.h sccp_device.h sccp_queue.h sccp_session.h sccp_server.h
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
