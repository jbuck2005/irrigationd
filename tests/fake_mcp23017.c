// Stand-in for the MCP23017 driver, so "make test" runs irrigationd without
// I2C hardware. Valve writes always succeed and only change an array.

#include <pthread.h>

#include "mcp23017.h"

static int fake_state[MAX_ZONE + 1];

int mcp_i2c_open(const char *i2c_device) { (void)i2c_device; return 0; }
void mcp_i2c_close(void) {}
void mcp_enable_thread_safety(pthread_mutex_t *lock) { (void)lock; }
void mcp_lock(void) {}
void mcp_unlock(void) {}
int mcp_config_outputs(void) { return 0; }

int mcp_set_zone_state(int zone, int state) {
    if (zone < 1 || zone > MAX_ZONE) return -1;
    fake_state[zone] = state;
    return 0;
}

int mcp_get_zone_state(int zone) {
    return (zone < 1 || zone > MAX_ZONE) ? -1 : fake_state[zone];
}
