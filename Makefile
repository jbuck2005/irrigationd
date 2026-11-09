# Compiler and flags
CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -Iinclude
LDFLAGS := -pthread
# OpenSSL 1.1.1 or later, for TLS 1.3 with a pre-shared key: sudo apt install libssl-dev
LDLIBS  := -lssl -lcrypto

SRC_DIR := src
OBJ_DIR := build
BIN_DIR := bin
SYSTEMD_DIR := systemd

# Programs
PROGRAMS := irrigationctl irrigationd

# Source files
IRRIGATIONCTL_SRCS:= $(SRC_DIR)/irrigationctl.c $(SRC_DIR)/tls.c
IRRIGATIOND_SRCS  := $(SRC_DIR)/irrigationd.c $(SRC_DIR)/mcp23017.c $(SRC_DIR)/tls.c

# Object files
IRRIGATIONCTL_OBJS:= $(IRRIGATIONCTL_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
IRRIGATIOND_OBJS  := $(IRRIGATIOND_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Default target
all: $(BIN_DIR)/irrigationctl $(BIN_DIR)/irrigationd

# Build rules
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/irrigationctl: $(IRRIGATIONCTL_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/irrigationd: $(IRRIGATIOND_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

# Test build: the real daemon with a fake valve driver, on a spare port, so it
# runs on any Linux machine without I2C hardware
TEST_PORT := 14242
$(BIN_DIR)/irrigationd-test: $(SRC_DIR)/irrigationd.c $(SRC_DIR)/tls.c tests/fake_mcp23017.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -DSERVER_PORT=$(TEST_PORT) $^ -o $@ $(LDFLAGS) $(LDLIBS)

test: $(BIN_DIR)/irrigationd-test $(BIN_DIR)/irrigationctl
	python3 tests/test_protocol.py $(BIN_DIR)/irrigationd-test $(BIN_DIR)/irrigationctl $(TEST_PORT)

.PHONY: all install dev clean test

# Create build directories if they don't exist
$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

# Install target
install: all
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(BIN_DIR)/irrigationctl $(BIN_DIR)/irrigationd $(DESTDIR)/usr/local/bin/
	install -d $(DESTDIR)/etc/systemd/system
	install -m 644 $(SYSTEMD_DIR)/irrigationd.service $(DESTDIR)/etc/systemd/system/
	install -d $(DESTDIR)/etc/default
	@DESTDIR="$(DESTDIR)" sh scripts/install-user.sh
	@DESTDIR="$(DESTDIR)" sh scripts/install-config.sh
	@# Let systemd see the new unit file; skipped for a staging install or without systemd
	@if [ -z "$(DESTDIR)" ] && [ -d /run/systemd/system ]; then systemctl daemon-reload && echo "Reloaded systemd units"; fi

# Development target: run irrigationd in foreground
dev: $(BIN_DIR)/irrigationd
	IRRIGATIOND_TOKEN=devtoken IRRIGATIOND_BIND_ADDR=127.0.0.1 $(BIN_DIR)/irrigationd

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
