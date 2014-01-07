TARGET = chan_sccp.so
OBJECTS = sccp.o
HEADERS = sccp_config.h
CFLAGS = -Wall -Wextra -Wno-unused-parameter -g -fPIC -D'_GNU_SOURCE' -D'AST_MODULE="xivo-libsccp"' 
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
