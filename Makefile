# Compiler and flags
CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -Iinclude
LDFLAGS := -pthread

SRC_DIR := src
OBJ_DIR := build
BIN_DIR := bin
SYSTEMD_DIR := systemd
EXAMPLES_DIR := examples

# Programs
PROGRAMS := irrigationctl irrigationd

# Source files
IRRIGATIONCTL_SRCS:= $(SRC_DIR)/irrigationctl.c
IRRIGATIOND_SRCS  := $(SRC_DIR)/irrigationd.c $(SRC_DIR)/mcp23017.c

# Object files
IRRIGATIONCTL_OBJS:= $(IRRIGATIONCTL_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
IRRIGATIOND_OBJS  := $(IRRIGATIOND_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Default target
all: $(BIN_DIR)/irrigationctl $(BIN_DIR)/irrigationd

# Build rules
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/irrigationctl: $(IRRIGATIONCTL_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/irrigationd: $(IRRIGATIOND_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Create build directories if they don't exist
$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

# Install target
install: all
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(BIN_DIR)/* $(DESTDIR)/usr/local/bin/
	install -d $(DESTDIR)/etc/systemd/system
	install -m 644 $(SYSTEMD_DIR)/irrigationd.service $(DESTDIR)/etc/systemd/system/
	install -d $(DESTDIR)/etc/default
	echo "Consdier running: sudo systemctl daemon-reload"
	echo ""
	@if [ ! -f $(DESTDIR)/etc/default/irrigationd ]; then \
		install -m 644 $(EXAMPLES_DIR)/irrigationd $(DESTDIR)/etc/default/irrigationd; \
		echo "Installed default /etc/default/irrigationd"; \
	else \
		echo "Skipping /etc/default/irrigationd (already exists)"; \
	fi

# Development target: run irrigationd in foreground
dev: $(BIN_DIR)/irrigationd
	IRRIGATIOND_TOKEN=devtoken IRRIGATIOND_BIND_ADDR=127.0.0.1 $(BIN_DIR)/irrigationd

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
