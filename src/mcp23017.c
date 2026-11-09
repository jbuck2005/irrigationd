#include "mcp23017.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <errno.h>
#include <string.h>

// -----------------------------------------------------------------------------
// MCP23017 Register Map (BANK=0 mode)
// -----------------------------------------------------------------------------
#define MCP23017_IODIRA   0x00   // I/O direction register for port A
#define MCP23017_IODIRB   0x01   // I/O direction register for port B
#define MCP23017_OLATA    0x14   // Output latch register for port A
#define MCP23017_OLATB    0x15   // Output latch register for port B
#define MCP23017_GPIOA    0x12   // Input register for port A
#define MCP23017_GPIOB    0x13   // Input register for port B

// -----------------------------------------------------------------------------
// Internal driver state
// -----------------------------------------------------------------------------
static int i2c_fd = -1;                  // File descriptor for /dev/i2c-X
static pthread_mutex_t *driver_lock = 0; // Optional external lock
static uint8_t olat_a = 0x00;            // Cached output states for port A
static uint8_t olat_b = 0x00;            // Cached output states for port B

// -----------------------------------------------------------------------------
// Low-level I²C helpers
// -----------------------------------------------------------------------------
static int i2c_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    if (write(i2c_fd, buf, 2) != 2) {
        fprintf(stderr, "i2c_write_reg: reg=0x%02x failed: %s\n", reg, strerror(errno));
        return -1;
    }
    return 0;
}

static int i2c_read_reg(uint8_t reg, uint8_t *val) {
    if (write(i2c_fd, &reg, 1) != 1) {
        fprintf(stderr, "i2c_read_reg: select reg=0x%02x failed: %s\n", reg, strerror(errno));
        return -1;
    }
    if (read(i2c_fd, val, 1) != 1) {
        fprintf(stderr, "i2c_read_reg: read reg=0x%02x failed: %s\n", reg, strerror(errno));
        return -1;
    }
    return 0;
}

// Writes the cached latches, then makes every pin an output. The latches go
// first so a pin never drives a stale value, even for a moment.
static int configure_chip(void) {
    if (i2c_write_reg(MCP23017_OLATA, olat_a) < 0) return -1;
    if (i2c_write_reg(MCP23017_OLATB, olat_b) < 0) return -1;
    if (i2c_write_reg(MCP23017_IODIRA, 0x00) < 0) return -1;
    if (i2c_write_reg(MCP23017_IODIRB, 0x00) < 0) return -1;
    return 0;
}

// A brown-out, such as from a solenoid starting, can reset the chip: every
// pin goes back to an input and writes to the latches still succeed, so valves
// would silently stop following commands. Check before each write, and set the
// chip up again if it was reset.
static void recover_if_reset(void) {
    uint8_t dir_a = 0, dir_b = 0;
    if (i2c_read_reg(MCP23017_IODIRA, &dir_a) < 0 || i2c_read_reg(MCP23017_IODIRB, &dir_b) < 0) {
        return;                                                                    // The write that follows reports the failure
    }
    if (dir_a != 0x00 || dir_b != 0x00) {
        fprintf(stderr, "MCP23017 was reset (IODIR %02x %02x): setting it up again\n", dir_a, dir_b);
        (void)configure_chip();
    }
}

// -----------------------------------------------------------------------------
// Driver open/close
// -----------------------------------------------------------------------------
int mcp_i2c_open(const char *dev) {
    i2c_fd = open(dev, O_RDWR);
    if (i2c_fd < 0) {
        fprintf(stderr, "mcp_i2c_open: failed to open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    if (ioctl(i2c_fd, I2C_SLAVE, 0x20) < 0) { // MCP23017 default address
        fprintf(stderr, "mcp_i2c_open: failed to set slave 0x20: %s\n", strerror(errno));
        close(i2c_fd);
        i2c_fd = -1;
        return -1;
    }

    // Initialize all outputs to OFF (0), then make all 16 pins outputs
    olat_a = 0x00;
    olat_b = 0x00;
    if (configure_chip() < 0) {
        close(i2c_fd);
        i2c_fd = -1;
        return -1;
    }

    return 0;
}

void mcp_i2c_close(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
}

// -----------------------------------------------------------------------------
// Thread safety support
// -----------------------------------------------------------------------------
void mcp_enable_thread_safety(pthread_mutex_t *lock) {
    driver_lock = lock;
}

void mcp_lock(void) {
    if (driver_lock) pthread_mutex_lock(driver_lock);
}

void mcp_unlock(void) {
    if (driver_lock) pthread_mutex_unlock(driver_lock);
}

// -----------------------------------------------------------------------------
// Zone mapping helpers
// -----------------------------------------------------------------------------
static int zone_to_pin(int zone, uint8_t *is_b, uint8_t *mask) {
    if (zone < 1 || zone > MAX_ZONE) return -1;
    int pin = zone - 1;                      // zone 1 = pin 0
    *is_b = (pin >= 8);
    *mask = 1 << (pin % 8);
    return 0;
}

// -----------------------------------------------------------------------------
// Zone control
// -----------------------------------------------------------------------------
int mcp_set_zone_state(int zone, int state) {
    uint8_t is_b, mask;
    if (zone_to_pin(zone, &is_b, &mask) < 0) return -1;

    int rc;
    mcp_lock();
    recover_if_reset();
    uint8_t reg = is_b ? MCP23017_OLATB : MCP23017_OLATA;
    uint8_t *olat = is_b ? &olat_b : &olat_a;
    if (state) *olat |= mask; else *olat &= ~mask;
    rc = i2c_write_reg(reg, *olat);

    // Read the latch back, so a write the chip didn't take counts as failed
    // and the caller retries a close
    uint8_t got = 0;
    if (rc == 0 && (i2c_read_reg(reg, &got) < 0 || got != *olat)) {
        fprintf(stderr, "MCP23017: latch 0x%02x reads %02x, wanted %02x\n", reg, got, *olat);
        rc = -1;
    }
    mcp_unlock();

    return rc;
}

int mcp_check_chip(void) {
    uint8_t dir_a = 0, dir_b = 0, got_a = 0, got_b = 0;
    int rc = 0;
    mcp_lock();
    if (i2c_read_reg(MCP23017_IODIRA, &dir_a) < 0 || i2c_read_reg(MCP23017_IODIRB, &dir_b) < 0 ||
        i2c_read_reg(MCP23017_OLATA, &got_a) < 0 || i2c_read_reg(MCP23017_OLATB, &got_b) < 0) {
        rc = -1;
    } else if (dir_a != 0x00 || dir_b != 0x00 || got_a != olat_a || got_b != olat_b) {
        fprintf(stderr, "MCP23017 lost its setup (IODIR %02x %02x, latches %02x %02x, wanted %02x %02x): "
                        "setting it up again\n", dir_a, dir_b, got_a, got_b, olat_a, olat_b);
        rc = configure_chip() < 0 ? -1 : 1;
    }
    mcp_unlock();
    return rc;
}

int mcp_get_zone_state(int zone) {
    uint8_t is_b, mask;
    if (zone_to_pin(zone, &is_b, &mask) < 0) return -1;

    uint8_t val = 0;
    int rc = i2c_read_reg(is_b ? MCP23017_GPIOB : MCP23017_GPIOA, &val);
    if (rc < 0) return -1;

    return (val & mask) ? 1 : 0;
}