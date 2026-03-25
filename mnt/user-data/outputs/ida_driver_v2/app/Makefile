# Makefile for ida_app userspace application
#
# Usage:
#   make          - build ida_app
#   make clean    - remove build artifacts
#   make install  - copy binary and config to system paths

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -g
# Include path for ida_uapi.h
CFLAGS  += -I../include
# lrt needed for shm_open on some older glibc (RHEL/Rocky 8)
LDFLAGS := -lrt

TARGET  := ida_app
SRCS    := ida_app.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 644 ida_app.conf /etc/
	@echo "Installed $(TARGET) to /usr/local/bin/"
	@echo "Installed ida_app.conf to /etc/"
