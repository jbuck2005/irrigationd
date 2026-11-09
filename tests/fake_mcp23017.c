// Stand-in for the MCP23017 driver, so "make test" runs irrigationd without
// I2C hardware. Valve writes only change an array, and are logged so tests
// can see them. FAKE_FAILED_CLOSES=N makes the first N closes of an open
// valve fail, like a passing I2C error.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "mcp23017.h"

static int fake_state[MAX_ZONE + 1];

int mcp_i2c_open(const char *i2c_device) { (void)i2c_device; return 0; }
void mcp_i2c_close(void) {}
void mcp_enable_thread_safety(pthread_mutex_t *lock) { (void)lock; }
void mcp_lock(void) {}
void mcp_unlock(void) {}
int mcp_config_outputs(void) { return 0; }

int mcp_set_zone_state(int zone, int state) {
    static int failures_left = -1;
    if (zone < 1 || zone > MAX_ZONE) return -1;
    if (failures_left < 0) {
        const char *env = getenv("FAKE_FAILED_CLOSES");
        failures_left = env ? atoi(env) : 0;
    }
    if (!state && fake_state[zone] && failures_left > 0) {
        failures_left--;
        fprintf(stderr, "fake valve: zone %d close failed\n", zone);
        return -1;
    }
    if (fake_state[zone] != state) {
        fprintf(stderr, "fake valve: zone %d %s\n", zone, state ? "ON" : "OFF");
    }
    fake_state[zone] = state;
    return 0;
}

int mcp_get_zone_state(int zone) {
    return (zone < 1 || zone > MAX_ZONE) ? -1 : fake_state[zone];
}
