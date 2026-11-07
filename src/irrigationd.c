#include <stdio.h>                                                                 // Standard I/O (fprintf, etc.)
#include <stdlib.h>                                                                // Memory management, exit codes
#include <string.h>                                                                // String handling (strstr, strcmp)
#include <unistd.h>                                                                // POSIX API (read, write, close)
#include <errno.h>                                                                 // errno variable and strerror()
#include <pthread.h>                                                               // POSIX threads for concurrency
#include <sys/socket.h>                                                            // Sockets API
#include <netinet/in.h>                                                            // sockaddr_in, htons, INADDR_ANY
#include <arpa/inet.h>                                                             // inet_ntop, inet_addr, inet_pton
#include <semaphore.h>                                                             // POSIX semaphores
#include <sys/types.h>                                                             // Basic system data types
#include <time.h>                                                                  // Sleep handling
#include <sys/time.h>                                                              // struct timeval for timeouts
#include <stdarg.h>                                                                // Variadic functions
#include <stdint.h>                                                                // int64_t for millisecond clocks
#include <signal.h>                                                                // SIGTERM/SIGINT/SIGPIPE handling

#include "mcp23017.h"                                                              // MCP23017 driver interface

// -----------------------------------------------------------------------------
// irrigationd.c - Irrigation controller daemon
//
// This daemon listens for simple text-based commands over TCP and manages
// irrigation zones connected through an MCP23017 GPIO expander.
//
// Example command format (single line, terminated by newline):
//   ZONE=1 TIME=30 TOKEN=your-long-random-token
//
// Commands:
//   ZONE=n TIME=s [TOKEN=secret]                                                   // Turn zone n ON for s seconds
//   ZONE=n TIME=0 [TOKEN=secret]                                                   // Turn zone n OFF immediately
//   STATUS [TOKEN=secret]                                                          // Report every zone, see send_status()
//
// Token authentication is mandatory: if IRRIGATIOND_TOKEN is not set in the
// environment (via /etc/default/irrigationd), the daemon refuses to start.       // Prevents insecure deployments
//
// -----------------------------------------------------------------------------

#define SERVER_PORT     4242                                                       // TCP port to listen on
#define CMD_BUF_SZ      256                                                        // Max command length in bytes
#define STATUS_BUF_SZ   (32 + 16 * MAX_ZONE)                                       // Header, plus up to 16 bytes per zone
#define MAX_CLIENTS     16                                                         // Max concurrent client connections
#define MAX_ZONE_RUNTIME 7200                                                      // Safety: max zone runtime = 2 hours
#define WATCHDOG_INTERVAL 30                                                       // Watchdog check interval in seconds
#define CLOSE_RETRY_MS  1000                                                       // Retry interval when a valve fails to close

// FIX: Rate Limiting Configuration
#define MAX_TRACKED_IPS   256                                                      // Max IPs to track for rate limiting
#define RATE_LIMIT_COUNT  10                                                       // Max connections per IP...
#define RATE_LIMIT_PERIOD 60                                                       // ...within this many seconds

// -----------------------------------------------------------------------------
// Shared state
// -----------------------------------------------------------------------------
// zone_lock guards zone_state, zone_end_ms and zone_timers, and is held across
// every valve write, so a zone's valve and its deadline always change together.
static int zone_state[MAX_ZONE+1];          // zone_state[z] = 1 if ON, 0 if OFF
static int64_t zone_end_ms[MAX_ZONE+1];     // When the current run ends (0 = no run)
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t zone_end_changed;     // Wakes the run timer when a deadline changes
static sem_t client_slots;
static const char *g_auth_token = NULL;

// FIX: Data structures for rate limiting
typedef struct {
    uint32_t ip_addr;                   // IP address in network byte order
    time_t timestamps[RATE_LIMIT_COUNT]; // Circular buffer of connection times
    int head;                           // Index of the oldest timestamp
    int count;                          // Number of timestamps in the buffer
    time_t last_seen;                   // For LRU eviction
} RateLimitEntry;

static RateLimitEntry rate_limit_table[MAX_TRACKED_IPS];
static pthread_mutex_t rate_limit_lock = PTHREAD_MUTEX_INITIALIZER;

// Safety: Watchdog data structures
typedef struct {
    time_t start_time;                  // When zone was turned ON (0 if OFF)
    int max_duration;                   // Maximum allowed runtime in seconds
} ZoneTimer;

static ZoneTimer zone_timers[MAX_ZONE+1];


// -----------------------------------------------------------------------------
// Memory-safe allocation wrappers with centralized error handling
// -----------------------------------------------------------------------------

static void *xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) {
        fprintf(stderr, "malloc(%zu) failed: %s\n", sz, strerror(errno));          // stderr logging
        exit(EXIT_FAILURE);
    }
    return p;
}

// -----------------------------------------------------------------------------
// Zone control
// -----------------------------------------------------------------------------

// Monotonic clock: run deadlines must not move when NTP steps the wall clock.
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Caller holds zone_lock. Returns 0, or -1 if the valve write failed.
static int set_zone_state(int zone, int state) {
    if (zone < 1 || zone > MAX_ZONE) {
        fprintf(stderr, "set_zone_state: invalid zone %d\n", zone);
        return -1;
    }

    mcp_lock();
    int rc = mcp_set_zone_state(zone, state);
    mcp_unlock();

    // A valve that failed to close may still be open, so keep treating it as ON
    int on = state || rc != 0;
    zone_state[zone] = on;

    // Update zone timer tracking
    if (state || (on && zone_timers[zone].start_time == 0)) {
        zone_timers[zone].start_time = now_ms() / 1000;
    } else if (!on) {
        zone_timers[zone].start_time = 0;
        zone_timers[zone].max_duration = 0;
    }

    if (rc != 0) {
        fprintf(stderr, "Zone %d: valve write failed (wanted %s)\n", zone, state ? "ON" : "OFF");
    } else {
        fprintf(stderr, "Zone %d -> %s\n", zone, state ? "ON" : "OFF");
    }
    return rc;
}

// Caller holds zone_lock. Ends any run on the zone. If the valve will not close
// the deadline stays set, so the run timer keeps retrying until it does.
static int stop_zone(int zone) {
    int rc = set_zone_state(zone, 0);
    zone_end_ms[zone] = rc == 0 ? 0 : now_ms() + CLOSE_RETRY_MS;
    pthread_cond_signal(&zone_end_changed);
    return rc;
}

// Starts a run, replacing any run already on the zone. The deadline is set
// before the valve opens and under the same lock, so an open valve always has
// one, and a replaced run has nothing left that can close the valve early.
static int start_zone(int zone, int duration) {
    pthread_mutex_lock(&zone_lock);
    zone_timers[zone].max_duration = duration;
    zone_end_ms[zone] = now_ms() + (int64_t)duration * 1000;
    int rc = set_zone_state(zone, 1);
    if (rc != 0) {
        stop_zone(zone);
    }
    pthread_cond_signal(&zone_end_changed);
    pthread_mutex_unlock(&zone_lock);
    return rc;
}

// -----------------------------------------------------------------------------
// Run timer thread: closes each zone when its current run ends
// -----------------------------------------------------------------------------

static void *run_timer_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&zone_lock);

    while (1) {
        int64_t now = now_ms();
        int64_t next = 0;

        for (int z = 1; z <= MAX_ZONE; z++) {
            if (zone_end_ms[z] != 0 && zone_end_ms[z] <= now) {
                stop_zone(z);
            }
            if (zone_end_ms[z] != 0 && (next == 0 || zone_end_ms[z] < next)) {
                next = zone_end_ms[z];
            }
        }

        if (next == 0) {
            pthread_cond_wait(&zone_end_changed, &zone_lock);
        } else {
            struct timespec ts = { .tv_sec = next / 1000, .tv_nsec = (next % 1000) * 1000000 };
            pthread_cond_timedwait(&zone_end_changed, &zone_lock, &ts);
        }
    }

    return NULL;
}

// -----------------------------------------------------------------------------
// Status report
// -----------------------------------------------------------------------------

// Writes one line reporting every zone, without touching any valve:
//   OK STATUS V=1 ZONES=14 Z1=OFF:0 Z2=ON:28 ... Z14=OFF:0
// Each zone is Zn=STATE:SECONDS, with seconds remaining rounded up. The README
// documents this format and the Home Assistant integration parses it, so change
// V if the meaning of an existing field ever changes.
static void send_status(int cfd) {
    char line[STATUS_BUF_SZ];
    int len = snprintf(line, sizeof(line), "OK STATUS V=1 ZONES=%d", MAX_ZONE);

    pthread_mutex_lock(&zone_lock);
    int64_t now = now_ms();
    for (int z = 1; z <= MAX_ZONE; z++) {
        int64_t left_ms = zone_state[z] ? zone_end_ms[z] - now : 0;
        long seconds = left_ms > 0 ? (long)((left_ms + 999) / 1000) : 0;
        len += snprintf(line + len, sizeof(line) - len, " Z%d=%s:%ld",
                        z, zone_state[z] ? "ON" : "OFF", seconds);
    }
    pthread_mutex_unlock(&zone_lock);

    line[len++] = '\n';
    write(cfd, line, len);
}

// -----------------------------------------------------------------------------
// Command parser
// -----------------------------------------------------------------------------

static int parse_command(int cfd, const char *cmd_const, const char *addrbuf) {
    char cmd[CMD_BUF_SZ];
    strncpy(cmd, cmd_const, sizeof(cmd));
    cmd[sizeof(cmd) - 1] = '\0';

    char *p_zone = NULL, *p_time = NULL, *p_token = NULL;
    int want_status = 0;
    char *saveptr;
    char *token = strtok_r(cmd, " \t\n\r", &saveptr);

    while (token != NULL) {
        if (strncmp(token, "ZONE=", 5) == 0) {
            p_zone = token + 5;
        } else if (strncmp(token, "TIME=", 5) == 0) {
            p_time = token + 5;
        } else if (strncmp(token, "TOKEN=", 6) == 0) {
            p_token = token + 6;
        } else if (strcmp(token, "STATUS") == 0) {
            want_status = 1;
        }
        token = strtok_r(NULL, " \t\n\r", &saveptr);
    }

    if (!p_token || strcmp(p_token, g_auth_token) != 0) {
        const char err[] = "ERR auth required\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (bad/missing token)\n", addrbuf);
        return -1;
    }

    // Not logged: this is polled, and would flood the journal
    if (want_status) {
        send_status(cfd);
        return 0;
    }

    if (!p_zone || !p_time) {
        const char err[] = "ERR missing ZONE or TIME\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (missing ZONE/TIME)\n", addrbuf);
        return -1;
    }

    char *endptr;
    long zone_l = strtol(p_zone, &endptr, 10);
    if (*endptr != '\0') {
        const char err[] = "ERR invalid ZONE format\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (invalid zone format)\n", addrbuf);
        return -1;
    }

    long duration_l = strtol(p_time, &endptr, 10);
    if (*endptr != '\0') {
        const char err[] = "ERR invalid TIME format\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (invalid time format)\n", addrbuf);
        return -1;
    }
    
    int zone = (int)zone_l;
    int duration = (int)duration_l;

    if (zone < 1 || zone > MAX_ZONE || duration < 0 || duration > MAX_ZONE_RUNTIME) {
        const char err[] = "ERR invalid ZONE or TIME\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (invalid zone/time)\n", addrbuf);
        return -1;
    }

    if (duration == 0) {
        pthread_mutex_lock(&zone_lock);
        int rc = stop_zone(zone);
        pthread_mutex_unlock(&zone_lock);
        if (rc != 0) {
            const char err[] = "ERR valve write failed\n";
            write(cfd, err, sizeof(err) - 1);
            fprintf(stderr, "[%s] Zone %d stop failed, retrying\n", addrbuf, zone);
            return -1;
        }
        const char ok[] = "OK\n";
        write(cfd, ok, sizeof(ok) - 1);
        fprintf(stderr, "[%s] Zone %d OFF (immediate stop)\n", addrbuf, zone);
        return 0;
    }

    if (start_zone(zone, duration) != 0) {
        const char err[] = "ERR valve write failed\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Zone %d start failed\n", addrbuf, zone);
        return -1;
    }

    const char ok[] = "OK\n";
    write(cfd, ok, sizeof(ok) - 1);
    fprintf(stderr, "[%s] Zone %d ON for %d seconds\n", addrbuf, zone, duration);
    return 0;
}

// -----------------------------------------------------------------------------
// Client thread handler (per connection)
// -----------------------------------------------------------------------------

// Format a client address (IPv4) into "ip:port"
static void format_client_addr(const struct sockaddr_in *cli, char *buf, size_t sz) {
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &cli->sin_addr, ip, sizeof(ip))) {
        snprintf(buf, sz, "unknown");
    } else {
        snprintf(buf, sz, "%s:%d", ip, ntohs(cli->sin_port));
    }
}

static ssize_t read_line(int fd, char *buf, size_t sz) {
    size_t i = 0;
    while (i < sz - 1) {
        char c;
        ssize_t rc = read(fd, &c, 1);
        if (rc == 1) {
            if (c == '\n') break;
            if (c != '\r') buf[i++] = c;
        } else if (rc == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            return -1;
        }
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

struct client_arg {
    int cfd;
    struct sockaddr_in cli;
};

static void *client_thread(void *arg) {
    struct client_arg *carg = arg;
    int cfd = carg->cfd;
    struct sockaddr_in cli = carg->cli;
    free(carg);

    char addrbuf[64];
    format_client_addr(&cli, addrbuf, sizeof(addrbuf));

    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[CMD_BUF_SZ];
    ssize_t n = read_line(cfd, buf, sizeof(buf));
    if (n > 0) {
        parse_command(cfd, buf, addrbuf);
    } else if (n < 0) {
        fprintf(stderr, "[%s] read from client failed: %s\n", addrbuf, strerror(errno));
    }

    close(cfd);
    sem_post(&client_slots);
    return NULL;
}

// -----------------------------------------------------------------------------
// Rate Limiting Logic
// -----------------------------------------------------------------------------

// FIX: New function to check and update rate limits for a given IP
static int is_rate_limited(struct in_addr client_ip) {
    pthread_mutex_lock(&rate_limit_lock);

    time_t now = time(NULL);
    int found_idx = -1;
    int evict_idx = 0; // Default to evicting the first entry
    time_t oldest_seen = (time_t)-1;

    for (int i = 0; i < MAX_TRACKED_IPS; ++i) {
        if (rate_limit_table[i].ip_addr == client_ip.s_addr) {
            found_idx = i;
            break;
        }
        if (rate_limit_table[i].last_seen < oldest_seen) {
            oldest_seen = rate_limit_table[i].last_seen;
            evict_idx = i;
        }
    }

    if (found_idx == -1) {
        // IP not found, evict the least recently seen entry
        found_idx = evict_idx;
        rate_limit_table[found_idx].ip_addr = client_ip.s_addr;
        rate_limit_table[found_idx].count = 0;
        rate_limit_table[found_idx].head = 0;
    }

    RateLimitEntry *entry = &rate_limit_table[found_idx];
    entry->last_seen = now;

    // If buffer is full, the oldest timestamp is at the current head
    if (entry->count == RATE_LIMIT_COUNT) {
        time_t oldest_ts = entry->timestamps[entry->head];
        if (now - oldest_ts < RATE_LIMIT_PERIOD) {
            // All timestamps are within the rate limit period
            pthread_mutex_unlock(&rate_limit_lock);
            return 1; // Rate limited
        }
    }

    // Record the new connection timestamp
    entry->timestamps[entry->head] = now;
    entry->head = (entry->head + 1) % RATE_LIMIT_COUNT;
    if (entry->count < RATE_LIMIT_COUNT) {
        entry->count++;
    }

    pthread_mutex_unlock(&rate_limit_lock);
    return 0; // Not rate limited
}

// -----------------------------------------------------------------------------
// Watchdog thread: safety monitor for runaway zones
// -----------------------------------------------------------------------------

static void *watchdog_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "Watchdog thread started (checking every %d seconds)\n", WATCHDOG_INTERVAL);

    while (1) {
        sleep(WATCHDOG_INTERVAL);
        
        time_t now = now_ms() / 1000;
        pthread_mutex_lock(&zone_lock);

        for (int z = 1; z <= MAX_ZONE; z++) {
            if (zone_timers[z].start_time > 0) {
                time_t elapsed = now - zone_timers[z].start_time;
                int max_allowed = zone_timers[z].max_duration > 0 ?
                                  zone_timers[z].max_duration : MAX_ZONE_RUNTIME;

                // Safety margin: add 60 seconds to account for timing variations
                if (elapsed > (max_allowed + 60)) {
                    fprintf(stderr, "WATCHDOG: Zone %d has been ON for %ld seconds (max %d), forcing OFF\n",
                            z, (long)elapsed, max_allowed);
                    stop_zone(z);
                }
            }
        }

        pthread_mutex_unlock(&zone_lock);
    }

    return NULL;
}

// -----------------------------------------------------------------------------
// Startup: Force all zones OFF for safety
// -----------------------------------------------------------------------------

static void shutdown_all_zones(void) {
    fprintf(stderr, "Startup safety: forcing all zones OFF...\n");
    pthread_mutex_lock(&zone_lock);
    for (int z = 1; z <= MAX_ZONE; z++) {
        stop_zone(z);
    }
    pthread_mutex_unlock(&zone_lock);
    fprintf(stderr, "All zones shut down\n");
}

// -----------------------------------------------------------------------------
// Signal thread: close every valve before the daemon stops
// -----------------------------------------------------------------------------

static void *signal_thread(void *arg) {
    int sig = 0;
    sigwait((const sigset_t *)arg, &sig);
    fprintf(stderr, "Received signal %d: closing all zones and exiting\n", sig);

    // Keep the lock until exit, so no command can open a valve after this
    pthread_mutex_lock(&zone_lock);
    int failed = 0;
    for (int z = 1; z <= MAX_ZONE; z++) {
        failed |= stop_zone(z);
    }

    // A failure exit lets systemd restart us, and startup closes all zones again
    exit(failed ? EXIT_FAILURE : EXIT_SUCCESS);
}

// -----------------------------------------------------------------------------
// Main server loop
// -----------------------------------------------------------------------------

int main(void) {
    // ... (environment loading and setup as before)
    g_auth_token = getenv("IRRIGATIOND_TOKEN");
    const char *bind_env = getenv("IRRIGATIOND_BIND_ADDR");
    struct in_addr bind_addr = {0};
    int have_bind_addr = 0;
    if (bind_env && bind_env[0] != '\0') {
        if (inet_pton(AF_INET, bind_env, &bind_addr) == 1) {
            have_bind_addr = 1;
        } else {
            fprintf(stderr, "Invalid IRRIGATIOND_BIND_ADDR '%s', falling back to 127.0.0.1\n", bind_env);
        }
    }

    if (g_auth_token) {
        fprintf(stderr, "Token enforcement enabled (IRRIGATIOND_TOKEN is set)\n");
    } else {
        fprintf(stderr, "IRRIGATIOND_TOKEN is not set, refusing to start insecure\n");
        exit(EXIT_FAILURE);
    }

    if (mcp_i2c_open("/dev/i2c-1") < 0) {
        fprintf(stderr, "ERROR: Failed to open I2C bus\n");
        exit(EXIT_FAILURE);
    }

    // The run timer's deadlines are on the monotonic clock
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&zone_end_changed, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    // Handle these in signal_thread only: threads inherit this mask
    static sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
    signal(SIGPIPE, SIG_IGN);                                                      // A client hanging up must not kill us

    // Safety: Shutdown all zones on startup
    shutdown_all_zones();

    // Runs depend on these threads, so refuse to start without them
    pthread_t run_timer_tid, signal_tid;
    int rtrc = pthread_create(&run_timer_tid, NULL, run_timer_thread, NULL);
    int sgrc = pthread_create(&signal_tid, NULL, signal_thread, &stop_signals);
    if (rtrc != 0 || sgrc != 0) {
        fprintf(stderr, "ERROR: Failed to start run timer or signal thread: %s\n", strerror(rtrc ? rtrc : sgrc));
        exit(EXIT_FAILURE);
    }
    pthread_detach(run_timer_tid);
    pthread_detach(signal_tid);

    // Start watchdog thread
    pthread_t watchdog_tid;
    int wdrc = pthread_create(&watchdog_tid, NULL, watchdog_thread, NULL);
    if (wdrc != 0) {
        fprintf(stderr, "WARNING: Failed to start watchdog thread: %s\n", strerror(wdrc));
    } else {
        pthread_detach(watchdog_tid);  // Let it run independently
        fprintf(stderr, "Watchdog thread started successfully\n");
    }

    sem_init(&client_slots, 0, MAX_CLIENTS);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if (have_bind_addr) {
        addr.sin_addr = bind_addr;
        char bstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, bstr, sizeof(bstr));
        fprintf(stderr, "Binding to %s:%d\n", bstr, SERVER_PORT);
    } else {
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        fprintf(stderr, "Binding to 127.0.0.1:%d\n", SERVER_PORT);
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, 8) < 0) {
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "irrigationd listening on port %d\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "accept failed: %s\n", strerror(errno));
            continue;
        }

        // FIX: Check rate limit before processing the connection
        if (is_rate_limited(cli.sin_addr)) {
            char addrbuf[64];
            format_client_addr(&cli, addrbuf, sizeof(addrbuf));
            fprintf(stderr, "[%s] Connection rejected: rate limit exceeded\n", addrbuf);
            const char err[] = "ERR rate limit exceeded\n";
            write(cfd, err, sizeof(err)-1);
            close(cfd);
            continue;
        }

        if (sem_wait(&client_slots) != 0) {
            fprintf(stderr, "Failed to acquire client slot: %s\n", strerror(errno));
            const char err[] = "ERR server busy\n";
            write(cfd, err, sizeof(err) - 1);
            close(cfd);
            continue;
        }

        struct client_arg *carg = xmalloc(sizeof(struct client_arg));
        carg->cfd = cfd;
        carg->cli = cli;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, carg) != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(errno));
            close(cfd);
            free(carg);
            sem_post(&client_slots);
            continue;
        }
        pthread_detach(tid);                                                       // Frees its stack on exit
    }

    close(listen_fd);
    return 0;
}