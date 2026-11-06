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

    // Configure all 16 pins as outputs                                          //
    if (i2c_write_reg(MCP23017_IODIRA, 0x00) < 0) return -1;
    if (i2c_write_reg(MCP23017_IODIRB, 0x00) < 0) return -1;

    // Initialize all outputs to OFF (0)                                         //
    olat_a = 0x00;
    olat_b = 0x00;
    if (i2c_write_reg(MCP23017_OLATA, olat_a) < 0) return -1;
    if (i2c_write_reg(MCP23017_OLATB, olat_b) < 0) return -1;

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

    mcp_lock();
    if (is_b) {
        if (state) olat_b |= mask; else olat_b &= ~mask;
        i2c_write_reg(MCP23017_OLATB, olat_b);
    } else {
        if (state) olat_a |= mask; else olat_a &= ~mask;
        i2c_write_reg(MCP23017_OLATA, olat_a);
    }
    mcp_unlock();

    return 0;
}

int mcp_get_zone_state(int zone) {
    uint8_t is_b, mask;
    if (zone_to_pin(zone, &is_b, &mask) < 0) return -1;

    uint8_t val = 0;
    int rc = i2c_read_reg(is_b ? MCP23017_GPIOB : MCP23017_GPIOA, &val);
    if (rc < 0) return -1;

    return (val & mask) ? 1 : 0;
}