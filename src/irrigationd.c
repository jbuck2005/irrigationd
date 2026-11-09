#include <stdio.h>                                                                 // Standard I/O (fprintf, etc.)
#include <stdlib.h>                                                                // Memory management, exit codes
#include <string.h>                                                                // String handling (strstr, strcmp)
#include <unistd.h>                                                                // POSIX API (read, write, close)
#include <errno.h>                                                                 // errno variable and strerror()
#include <pthread.h>                                                               // POSIX threads for concurrency
#include <sys/socket.h>                                                            // Sockets API
#include <netinet/in.h>                                                            // sockaddr_in, htons, INADDR_ANY
#include <arpa/inet.h>                                                             // inet_ntop, inet_addr, inet_pton
#include <sys/types.h>                                                             // Basic system data types
#include <time.h>                                                                  // Sleep handling
#include <sys/time.h>                                                              // struct timeval for timeouts
#include <stdarg.h>                                                                // Variadic functions
#include <stdint.h>                                                                // int64_t for millisecond clocks
#include <limits.h>                                                                // LONG_MAX for LRU eviction
#include <signal.h>                                                                // SIGTERM/SIGINT/SIGPIPE handling

#include <openssl/err.h>                                                           // ERR_clear_error
#include "tls.h"                                                                   // TLS with the token as a pre-shared key
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
// Connections may be TLS 1.3 using the token as a pre-shared key (see tls.h),
// in which case the handshake proves the token and TOKEN= is not needed, or
// plain text with TOKEN=, unless IRRIGATIOND_PLAIN_TOKEN=no turns that off.
//
// Token authentication is mandatory: if IRRIGATIOND_TOKEN is not set in the
// environment (via /etc/default/irrigationd), the daemon refuses to start.       // Prevents insecure deployments
//
// -----------------------------------------------------------------------------

#ifndef SERVER_PORT
#define SERVER_PORT     4242                                                       // TCP port to listen on
#endif
#define CMD_BUF_SZ      256                                                        // Max command length in bytes
#define STATUS_BUF_SZ   (32 + 16 * MAX_ZONE)                                       // Header, plus up to 16 bytes per zone
#define MAX_CLIENTS     16                                                         // Max concurrent client connections
#define MAX_CLIENTS_PER_IP 4                                                       // Max concurrent connections from one address
#define RESERVED_SLOTS  4                                                          // Kept for the trusted address and known clients
#define MAX_KNOWN_CLIENTS 4                                                        // Addresses that recently used the token
#define KNOWN_CLIENT_MS (15 * 60 * 1000)                                           // How long a client stays known after using it
#define REFUSAL_LOG_BURST 10                                                       // Refused connections logged per window...
#define REFUSAL_LOG_WINDOW_MS 10000                                                // ...before the rest are only counted
#define CONN_DEADLINE_MS 5000                                                      // A whole connection: handshake, command and reply
#define MAX_ZONE_RUNTIME 7200                                                      // Safety: max zone runtime = 2 hours
#define WATCHDOG_INTERVAL 30                                                       // Watchdog check interval in seconds
#define ZONE_COOLDOWN_SECS 600                                                     // Wait after a valve reaches MAX_ZONE_RUNTIME
#define ZONE_AT_LIMIT   1                                                          // start_zone: no time left, or still cooling down
#define CLOSE_RETRY_MS  1000                                                       // Retry interval when a valve fails to close
#define SHUTDOWN_CLOSE_RETRY_MS 500                                                // On a stop signal, retry a failed close this often
#define SHUTDOWN_CLOSE_RETRIES  60                                                 // ...for up to 30 seconds

// FIX: Rate Limiting Configuration
#define MAX_TRACKED_IPS   256                                                      // Max IPs to track for rate limiting
#define RATE_LIMIT_COUNT  10                                                       // Max unauthenticated connections per IP...
#define RATE_LIMIT_PERIOD 60                                                       // ...within this many seconds
#define RATE_LIMIT_DELAY  1                                                        // Seconds an over-limit connection waits before its token is checked
#define RATE_LIMIT_MAX_WAITING 2                                                   // Over-limit connections an IP may have open at once

// -----------------------------------------------------------------------------
// Shared state
// -----------------------------------------------------------------------------
// zone_lock guards zone_state, zone_end_ms and zone_timers, and is held across
// every valve write, so a zone's valve and its deadline always change together.
static int zone_state[MAX_ZONE+1];          // zone_state[z] = 1 if ON, 0 if OFF
static int64_t zone_end_ms[MAX_ZONE+1];     // When the current run ends (0 = no run)
static int64_t zone_reopen_ms[MAX_ZONE+1];  // Runs refused until this time (0 = none)
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t zone_end_changed;     // Wakes the run timer when a deadline changes
// Every open client connection, so a slow one can be cut off at its deadline
// and no address can take every slot. One slot is kept for the trusted address.
typedef struct {
    int in_use;
    int fd;
    uint32_t ip_addr;                   // Network byte order
    int64_t deadline_ms;                // Monotonic; past it, the socket is shut down
} ClientSlot;

static ClientSlot client_slots[MAX_CLIENTS];
// Addresses that proved they hold the token, and when. Guarded by client_slots_lock.
static struct { uint32_t ip_addr; int64_t seen_ms; } known_clients[MAX_KNOWN_CLIENTS];
static pthread_mutex_t client_slots_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *g_auth_token = NULL;
static struct in_addr g_trusted_addr;       // Exempt from the rate limit, if set
static int g_have_trusted_addr = 0;
static SSL_CTX *g_tls_ctx = NULL;           // Server side of TLS with the token as key
static int g_plain_allowed = 1;             // IRRIGATIOND_PLAIN_TOKEN: accept TOKEN= in plain text
static int64_t g_plain_warned_ms = 0;       // Last plain text warning, at most one an hour
static pthread_mutex_t plain_warn_lock = PTHREAD_MUTEX_INITIALIZER;

// FIX: Data structures for rate limiting
typedef struct {
    uint32_t ip_addr;                   // IP address in network byte order
    time_t timestamps[RATE_LIMIT_COUNT]; // Circular buffer of connection times
    int head;                           // Index of the oldest timestamp
    int count;                          // Number of timestamps in the buffer
    int waiting;                        // Open connections that are over the limit
    time_t last_seen;                   // For LRU eviction
} RateLimitEntry;

static RateLimitEntry rate_limit_table[MAX_TRACKED_IPS];
static pthread_mutex_t rate_limit_lock = PTHREAD_MUTEX_INITIALIZER;

// Safety: Watchdog data structures
typedef struct {
    int64_t start_ms;                   // When the valve opened (0 if OFF)
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
    if (on && zone_timers[zone].start_ms == 0) {
        zone_timers[zone].start_ms = now_ms();
    } else if (!on) {
        zone_timers[zone].start_ms = 0;
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
// MAX_ZONE_RUNTIME limits how long the valve stays open in one go, so replacing
// a run cannot extend it past that: the new run is shortened to fit, and refused
// with ZONE_AT_LIMIT if no time is left. It is also refused during the cooldown
// that follows, so repeated commands can't reopen the valve as soon as it closes.
static int start_zone(int zone, int duration) {
    pthread_mutex_lock(&zone_lock);
    int64_t now = now_ms();
    if (now < zone_reopen_ms[zone]) {
        pthread_mutex_unlock(&zone_lock);
        return ZONE_AT_LIMIT;
    }

    if (zone_timers[zone].start_ms == 0) {
        zone_timers[zone].start_ms = now;
    }

    int64_t start_ms = zone_timers[zone].start_ms;
    int64_t end_ms = now + (int64_t)duration * 1000;
    if (end_ms > start_ms + (int64_t)MAX_ZONE_RUNTIME * 1000) {
        end_ms = start_ms + (int64_t)MAX_ZONE_RUNTIME * 1000;
    }
    if (end_ms <= now) {
        pthread_mutex_unlock(&zone_lock);
        return ZONE_AT_LIMIT;
    }

    zone_timers[zone].max_duration = (int)((end_ms - start_ms + 999) / 1000);
    zone_end_ms[zone] = end_ms;
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
                if (now - zone_timers[z].start_ms >= (int64_t)MAX_ZONE_RUNTIME * 1000) {
                    zone_reopen_ms[z] = now + (int64_t)ZONE_COOLDOWN_SECS * 1000;
                }
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
static void send_status(struct conn *conn) {
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
    conn_write(conn, line, len);
}

// -----------------------------------------------------------------------------
// Command parser
// -----------------------------------------------------------------------------

// Parses a whole decimal number. Returns -1 if it is empty, has anything after
// the digits, or is too large for a long.
static int parse_number(const char *text, long *value) {
    char *endptr;
    errno = 0;
    *value = strtol(text, &endptr, 10);
    return (endptr == text || *endptr != '\0' || errno != 0) ? -1 : 0;
}

// Looks at every byte of what was sent, so timing does not show how much matched.
static int token_matches(const char *given) {
    size_t want_len = strlen(g_auth_token);
    size_t given_len = strlen(given);
    size_t diff = want_len ^ given_len;

    for (size_t i = 0; i < given_len; i++) {
        diff |= (unsigned char)given[i] ^ (unsigned char)g_auth_token[i % want_len];
    }
    return diff == 0;
}

// A plain text login works, but anyone watching the network saw the token, so
// say so. At most once an hour: Home Assistant polls every 15 seconds.
static void warn_plain_token(const char *addrbuf) {
    pthread_mutex_lock(&plain_warn_lock);
    int64_t now = now_ms();
    int warn = g_plain_warned_ms == 0 || now - g_plain_warned_ms >= 3600 * 1000;
    if (warn) {
        g_plain_warned_ms = now;
    }
    pthread_mutex_unlock(&plain_warn_lock);
    if (warn) {
        fprintf(stderr, "[%s] WARNING: token received in plain text. Update the client to use TLS, "
                        "then set IRRIGATIOND_PLAIN_TOKEN=no\n", addrbuf);
    }
}

// Sets *authenticated once the token has been accepted, whatever happens after.
static int parse_command(struct conn *conn, const char *cmd_const, const char *addrbuf,
                         int over_limit, int *authenticated) {
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

    // Over TLS the handshake has already proved the token, so TOKEN= is ignored
    int tls = conn->ssl != NULL;
    if (!tls && (!p_token || !token_matches(p_token))) {
        const char auth_err[] = "ERR auth required\n";
        const char limit_err[] = "ERR rate limit exceeded\n";
        if (over_limit) {
            conn_write(conn, limit_err, sizeof(limit_err) - 1);
        } else {
            conn_write(conn, auth_err, sizeof(auth_err) - 1);
        }
        fprintf(stderr, "[%s] Rejected command (bad/missing token)\n", addrbuf);
        return -1;
    }
    *authenticated = 1;
    if (!tls) {
        warn_plain_token(addrbuf);
    }

    // Not logged: this is polled, and would flood the journal
    if (want_status) {
        send_status(conn);
        return 0;
    }

    if (!p_zone || !p_time) {
        const char err[] = "ERR missing ZONE or TIME\n";
        conn_write(conn, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (missing ZONE/TIME)\n", addrbuf);
        return -1;
    }

    long zone_l;
    if (parse_number(p_zone, &zone_l) != 0) {
        const char err[] = "ERR invalid ZONE format\n";
        conn_write(conn, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (invalid zone format)\n", addrbuf);
        return -1;
    }

    long duration_l;
    if (parse_number(p_time, &duration_l) != 0) {
        const char err[] = "ERR invalid TIME format\n";
        conn_write(conn, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (invalid time format)\n", addrbuf);
        return -1;
    }
    
    // Range check before narrowing, so a huge value cannot wrap into a valid one
    if (zone_l < 1 || zone_l > MAX_ZONE || duration_l < 0 || duration_l > MAX_ZONE_RUNTIME) {
        const char err[] = "ERR invalid ZONE or TIME\n";
        conn_write(conn, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (invalid zone/time)\n", addrbuf);
        return -1;
    }

    int zone = (int)zone_l;
    int duration = (int)duration_l;

    if (duration == 0) {
        pthread_mutex_lock(&zone_lock);
        zone_reopen_ms[zone] = 0;                                                  // A deliberate stop ends any cooldown
        int rc = stop_zone(zone);
        pthread_mutex_unlock(&zone_lock);
        if (rc != 0) {
            const char err[] = "ERR valve write failed\n";
            conn_write(conn, err, sizeof(err) - 1);
            fprintf(stderr, "[%s] Zone %d stop failed, retrying\n", addrbuf, zone);
            return -1;
        }
        const char ok[] = "OK\n";
        conn_write(conn, ok, sizeof(ok) - 1);
        fprintf(stderr, "[%s] Zone %d OFF (immediate stop)\n", addrbuf, zone);
        return 0;
    }

    int start_rc = start_zone(zone, duration);
    if (start_rc == ZONE_AT_LIMIT) {
        const char err[] = "ERR zone at maximum runtime\n";
        conn_write(conn, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Zone %d start refused: at maximum runtime\n", addrbuf, zone);
        return -1;
    }
    if (start_rc != 0) {
        const char err[] = "ERR valve write failed\n";
        conn_write(conn, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Zone %d start failed\n", addrbuf, zone);
        return -1;
    }

    const char ok[] = "OK\n";
    conn_write(conn, ok, sizeof(ok) - 1);
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

static ssize_t read_line(struct conn *conn, char *buf, size_t sz) {
    size_t i = 0;
    while (i < sz - 1) {
        char c;
        ssize_t rc = conn_read(conn, &c, 1);
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

// -----------------------------------------------------------------------------
// Rate Limiting Logic
//
// Only connections that fail to authenticate count. Each one is recorded when
// accepted and the record is cleared when a valid token arrives, so a client
// with the token never reaches the limit however often it connects. Once an IP
// is over the limit its connections are still read, so a valid Stop still works:
// each waits RATE_LIMIT_DELAY before its token is checked, valid or not, which
// paces guessing, and at most RATE_LIMIT_MAX_WAITING may be open at once, so one
// IP cannot hold every client slot. That cap can refuse a valid Stop from an IP
// that is flooding bad tokens, so IRRIGATIOND_TRUSTED_ADDR exempts one address,
// meant for the host that already holds the token.
// -----------------------------------------------------------------------------

enum rate_limit_result { RATE_OK, RATE_OVER_LIMIT, RATE_REFUSED };

// Caller holds rate_limit_lock. Returns NULL if the IP is not tracked and !create.
static RateLimitEntry *find_rate_limit_entry(struct in_addr client_ip, int create) {
    int evict_idx = 0;
    time_t oldest_seen = LONG_MAX;

    for (int i = 0; i < MAX_TRACKED_IPS; ++i) {
        if (rate_limit_table[i].ip_addr == client_ip.s_addr) {
            return &rate_limit_table[i];
        }
        if (rate_limit_table[i].last_seen < oldest_seen) {
            oldest_seen = rate_limit_table[i].last_seen;
            evict_idx = i;
        }
    }

    if (!create) {
        return NULL;
    }

    // IP not found, evict the least recently seen entry
    RateLimitEntry *entry = &rate_limit_table[evict_idx];
    memset(entry, 0, sizeof(*entry));
    entry->ip_addr = client_ip.s_addr;
    return entry;
}

// Records a newly accepted connection, which counts as a failure until it
// authenticates. RATE_OVER_LIMIT must be paired with rate_limit_finish().
static enum rate_limit_result rate_limit_accept(struct in_addr client_ip) {
    pthread_mutex_lock(&rate_limit_lock);

    time_t now = now_ms() / 1000;
    RateLimitEntry *entry = find_rate_limit_entry(client_ip, 1);
    entry->last_seen = now;

    // If buffer is full, the oldest timestamp is at the current head
    if (entry->count == RATE_LIMIT_COUNT && now - entry->timestamps[entry->head] < RATE_LIMIT_PERIOD) {
        enum rate_limit_result result = RATE_REFUSED;
        if (entry->waiting < RATE_LIMIT_MAX_WAITING) {
            entry->waiting++;
            result = RATE_OVER_LIMIT;
        }
        pthread_mutex_unlock(&rate_limit_lock);
        return result;
    }

    // Record the new connection timestamp
    entry->timestamps[entry->head] = now;
    entry->head = (entry->head + 1) % RATE_LIMIT_COUNT;
    if (entry->count < RATE_LIMIT_COUNT) {
        entry->count++;
    }

    pthread_mutex_unlock(&rate_limit_lock);
    return RATE_OK;
}

// Called when a connection ends. A valid token clears the IP's failures.
static void rate_limit_finish(struct in_addr client_ip, int over_limit, int authenticated) {
    pthread_mutex_lock(&rate_limit_lock);

    RateLimitEntry *entry = find_rate_limit_entry(client_ip, 0);
    if (entry) {
        if (over_limit && entry->waiting > 0) {
            entry->waiting--;
        }
        if (authenticated) {
            entry->count = 0;
            entry->head = 0;
        }
    }

    pthread_mutex_unlock(&rate_limit_lock);
}

struct client_arg {
    int cfd;
    struct sockaddr_in cli;
    int over_limit;
    int slot;
};

// Reads one command line and acts on it.
static void handle_line(struct conn *conn, const char *addrbuf, int over_limit, int *authenticated) {
    char buf[CMD_BUF_SZ];
    ssize_t n = read_line(conn, buf, sizeof(buf));
    if (n > 0) {
        parse_command(conn, buf, addrbuf, over_limit, authenticated);
    } else if (n < 0) {
        fprintf(stderr, "[%s] read from client failed: %s\n", addrbuf, strerror(errno));
    }
}

// A finished handshake proves the client holds the token: only then is the
// command read. A failed one counts as a failed login for the rate limit.
static void handle_tls(struct conn *conn, const char *addrbuf, int over_limit, int *authenticated) {
    conn->ssl = SSL_new(g_tls_ctx);
    if (conn->ssl == NULL || SSL_set_fd(conn->ssl, conn->fd) != 1) {
        fprintf(stderr, "[%s] TLS setup failed\n", addrbuf);
        conn_shutdown(conn);
        return;
    }
    if (SSL_accept(conn->ssl) != 1) {
        ERR_clear_error();
        SSL_free(conn->ssl);
        conn->ssl = NULL;
        fprintf(stderr, "[%s] Rejected TLS connection (wrong token or not a client)\n", addrbuf);
        return;
    }
    *authenticated = 1;
    handle_line(conn, addrbuf, over_limit, authenticated);
    conn_shutdown(conn);
}

enum slot_result { SLOT_OK, SLOT_BUSY, SLOT_IP_BUSY };

// Whether `ip` used the token recently. Call with client_slots_lock held.
static int is_known_client(uint32_t ip, int64_t now) {
    for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
        if (known_clients[i].ip_addr == ip && known_clients[i].seen_ms != 0 &&
            now - known_clients[i].seen_ms < KNOWN_CLIENT_MS) {
            return 1;
        }
    }
    return 0;
}

// Remembers that `ip` just used the token, so it can use the reserved slots
// even when IRRIGATIOND_TRUSTED_ADDR isn't set. Replaces the oldest entry.
static void note_known_client(struct in_addr ip) {
    pthread_mutex_lock(&client_slots_lock);
    int64_t now = now_ms();
    int idx = 0;
    for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
        if (known_clients[i].ip_addr == ip.s_addr) { idx = i; break; }
        if (known_clients[i].seen_ms < known_clients[idx].seen_ms) idx = i;
    }
    known_clients[idx].ip_addr = ip.s_addr;
    known_clients[idx].seen_ms = now;
    pthread_mutex_unlock(&client_slots_lock);
}

// Logs a refused connection, but at most REFUSAL_LOG_BURST of them each
// REFUSAL_LOG_WINDOW_MS, so a flood can't wear out the SD card or push the
// daemon's own messages out of the journal. The rest are counted and summed up.
// With cli NULL, only writes the sum once the window is over.
static void log_refusal(const struct sockaddr_in *cli, const char *reason) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int64_t window_start = 0;
    static int logged = 0;
    static long suppressed = 0;
    pthread_mutex_lock(&lock);
    int64_t now = now_ms();
    if (now - window_start >= REFUSAL_LOG_WINDOW_MS) {
        if (suppressed > 0) {
            fprintf(stderr, "%ld more connections were refused and not logged\n", suppressed);
        }
        window_start = now;
        logged = 0;
        suppressed = 0;
    }
    if (cli == NULL) {
        // Nothing to log
    } else if (logged < REFUSAL_LOG_BURST) {
        logged++;
        char addrbuf[64];
        format_client_addr(cli, addrbuf, sizeof(addrbuf));
        fprintf(stderr, "[%s] Connection rejected: %s\n", addrbuf, reason);
    } else {
        suppressed++;
    }
    pthread_mutex_unlock(&lock);
}

// Takes a slot for a new connection, and puts its index in *index.
static enum slot_result client_slot_take(int fd, struct in_addr ip, int trusted, int *index) {
    pthread_mutex_lock(&client_slots_lock);
    // A client that recently used the token, such as Home Assistant, is let in
    // even while unknown addresses hold every other slot
    trusted = trusted || is_known_client(ip.s_addr, now_ms());
    int used = 0, from_ip = 0, free_idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_slots[i].in_use) {
            used++;
            if (client_slots[i].ip_addr == ip.s_addr) from_ip++;
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }

    enum slot_result result = SLOT_OK;
    if (free_idx < 0) {
        result = SLOT_BUSY;
    } else if (!trusted && used >= MAX_CLIENTS - RESERVED_SLOTS) {
        result = SLOT_BUSY;                                                        // The last slots are for trusted and known clients
    } else if (!trusted && from_ip >= MAX_CLIENTS_PER_IP) {
        result = SLOT_IP_BUSY;
    } else {
        client_slots[free_idx] = (ClientSlot){
            .in_use = 1, .fd = fd, .ip_addr = ip.s_addr,
            .deadline_ms = now_ms() + CONN_DEADLINE_MS,
        };
        *index = free_idx;
    }
    pthread_mutex_unlock(&client_slots_lock);
    return result;
}

// Releases a slot. Called before the fd is closed, so the reaper can never
// shut down a socket number that has since been reused.
static void client_slot_release(int index) {
    pthread_mutex_lock(&client_slots_lock);
    client_slots[index].in_use = 0;
    client_slots[index].fd = -1;
    pthread_mutex_unlock(&client_slots_lock);
}

// Cuts off connections past their deadline. Shutting the socket down makes the
// client thread's blocked read return, so it finishes and frees its slot. This
// stops a client that sends a byte every few seconds from holding a slot.
static void *reaper_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(1);
        log_refusal(NULL, NULL);
        pthread_mutex_lock(&client_slots_lock);
        int64_t now = now_ms();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_slots[i].in_use && client_slots[i].fd >= 0 && now > client_slots[i].deadline_ms) {
                shutdown(client_slots[i].fd, SHUT_RDWR);
                client_slots[i].fd = -1;                                           // Once is enough
            }
        }
        pthread_mutex_unlock(&client_slots_lock);
    }
    return NULL;
}

static void *client_thread(void *arg) {
    struct client_arg *carg = arg;
    int cfd = carg->cfd;
    struct sockaddr_in cli = carg->cli;
    int over_limit = carg->over_limit;
    int slot = carg->slot;
    free(carg);

    char addrbuf[64];
    format_client_addr(&cli, addrbuf, sizeof(addrbuf));

    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct conn conn = { .fd = cfd, .ssl = NULL };
    int authenticated = 0;

    // Look at the first byte without taking it: TLS always starts with a
    // handshake record, and a plain text command never does
    unsigned char first;
    ssize_t peeked;
    do {
        peeked = recv(cfd, &first, 1, MSG_PEEK);
    } while (peeked < 0 && errno == EINTR);

    if (peeked == 1) {
        // Paces guessing, whichever way the guess arrives
        if (over_limit) {
            sleep(RATE_LIMIT_DELAY);
        }
        if (first == TLS_RECORD_HANDSHAKE) {
            handle_tls(&conn, addrbuf, over_limit, &authenticated);
        } else if (!g_plain_allowed) {
            const char err[] = "ERR plain text disabled, use TLS\n";
            conn_write(&conn, err, sizeof(err) - 1);
            fprintf(stderr, "[%s] Rejected plain text connection (IRRIGATIOND_PLAIN_TOKEN=no)\n", addrbuf);
        } else {
            handle_line(&conn, addrbuf, over_limit, &authenticated);
        }
    } else if (peeked < 0) {
        fprintf(stderr, "[%s] read from client failed: %s\n", addrbuf, strerror(errno));
    }

    client_slot_release(slot);
    close(cfd);
    if (authenticated) {
        note_known_client(cli.sin_addr);
    }
    rate_limit_finish(cli.sin_addr, over_limit, authenticated);
    return NULL;
}

// -----------------------------------------------------------------------------
// Watchdog thread: safety monitor for runaway zones
// -----------------------------------------------------------------------------

static void *watchdog_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "Watchdog thread started (checking every %d seconds)\n", WATCHDOG_INTERVAL);

    while (1) {
        sleep(WATCHDOG_INTERVAL);
        
        pthread_mutex_lock(&zone_lock);
        int64_t now = now_ms();

        for (int z = 1; z <= MAX_ZONE; z++) {
            if (zone_timers[z].start_ms > 0) {
                time_t elapsed = (now - zone_timers[z].start_ms) / 1000;
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
    int failed[MAX_ZONE + 1] = {0};
    int any_failed = 0;
    for (int z = 1; z <= MAX_ZONE; z++) {
        failed[z] = stop_zone(z) != 0;
        any_failed |= failed[z];
    }

    // systemd doesn't restart a stopped service, so a failed close is retried
    // here rather than left to a restart that may never come
    for (int attempt = 0; any_failed && attempt < SHUTDOWN_CLOSE_RETRIES; attempt++) {
        struct timespec pause = { 0, SHUTDOWN_CLOSE_RETRY_MS * 1000000L };
        nanosleep(&pause, NULL);
        any_failed = 0;
        for (int z = 1; z <= MAX_ZONE; z++) {
            if (failed[z]) {
                failed[z] = stop_zone(z) != 0;
                if (!failed[z]) {
                    fprintf(stderr, "Zone %d: closed on retry\n", z);
                }
                any_failed |= failed[z];
            }
        }
    }
    if (any_failed) {
        fprintf(stderr, "Some zones could not be closed before exiting\n");
    }

    // A failure exit lets systemd restart us, and startup closes all zones again.
    // SIGHUP and SIGQUIT also exit with a failure: no one meant to stop the
    // service, such as "kill -HUP" sent for a reload, so it should come back.
    int stop_requested = sig == SIGTERM || sig == SIGINT;
    exit(any_failed || !stop_requested ? EXIT_FAILURE : EXIT_SUCCESS);
}

// Example values from the docs and older default configs. They are public, so
// they must never work as a token.
static int is_example_token(const char *token) {
    return strcmp(token, "CHANGE_ME_to_a_long_random_token") == 0 ||
           strcmp(token, "your-long-random-token") == 0;
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

    if (g_auth_token && g_auth_token[0] != '\0') {
        fprintf(stderr, "Token enforcement enabled (IRRIGATIOND_TOKEN is set)\n");
    } else {
        fprintf(stderr, "IRRIGATIOND_TOKEN is not set or empty, refusing to start insecure\n");
        exit(EXIT_FAILURE);
    }

    const char *trusted_env = getenv("IRRIGATIOND_TRUSTED_ADDR");
    if (trusted_env && trusted_env[0] != '\0') {
        if (inet_pton(AF_INET, trusted_env, &g_trusted_addr) == 1) {
            g_have_trusted_addr = 1;
            fprintf(stderr, "Rate limit does not apply to %s\n", trusted_env);
        } else {
            fprintf(stderr, "Invalid IRRIGATIOND_TRUSTED_ADDR '%s', rate limit applies to all clients\n", trusted_env);
        }
    }

    // Without it, Home Assistant only gets the reserved slots once it has used
    // the token, so a flood that starts before then keeps it out
    if (!g_have_trusted_addr && have_bind_addr && bind_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        fprintf(stderr, "IRRIGATIOND_TRUSTED_ADDR is not set. Set it to your Home Assistant address, "
                        "so devices without the token can't keep it out right after a restart\n");
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
    // These would otherwise end the daemon without closing any valve
    sigaddset(&stop_signals, SIGHUP);
    sigaddset(&stop_signals, SIGQUIT);
    pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
    signal(SIGPIPE, SIG_IGN);                                                      // A client hanging up must not kill us

    // Safety: Shutdown all zones on startup
    shutdown_all_zones();

    // Checked after closing the valves, so refusing to run can't leave one open
    if (is_example_token(g_auth_token)) {
        fprintf(stderr, "IRRIGATIOND_TOKEN is still an example value, refusing to start. "
                        "Set your own in /etc/default/irrigationd: openssl rand -hex 16\n");
        exit(EXIT_FAILURE);
    }

    g_tls_ctx = tls_server_ctx(g_auth_token);
    if (g_tls_ctx == NULL) {
        fprintf(stderr, "ERROR: Failed to set up TLS\n");
        exit(EXIT_FAILURE);
    }

    // Missing means yes, so an existing install keeps working with clients
    // that don't speak TLS yet. Anything but yes or no turns plain text off.
    const char *plain_env = getenv("IRRIGATIOND_PLAIN_TOKEN");
    if (plain_env == NULL || plain_env[0] == '\0' || strcmp(plain_env, "yes") == 0) {
        g_plain_allowed = 1;
        fprintf(stderr, "Accepting TLS, and plain text logins with TOKEN= "
                        "(set IRRIGATIOND_PLAIN_TOKEN=no once every client uses TLS)\n");
    } else {
        g_plain_allowed = 0;
        if (strcmp(plain_env, "no") != 0) {
            fprintf(stderr, "Invalid IRRIGATIOND_PLAIN_TOKEN '%s', treating it as no\n", plain_env);
        }
        fprintf(stderr, "Accepting TLS only\n");
    }

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

    pthread_t reaper_tid;
    int rprc = pthread_create(&reaper_tid, NULL, reaper_thread, NULL);
    if (rprc != 0) {
        // Without it a slow client could hold a slot for good
        fprintf(stderr, "ERROR: Failed to start connection reaper thread: %s\n", strerror(rprc));
        exit(EXIT_FAILURE);
    }
    pthread_detach(reaper_tid);

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

        // Only refuses an IP already over the limit with connections waiting
        int trusted = g_have_trusted_addr && cli.sin_addr.s_addr == g_trusted_addr.s_addr;
        enum rate_limit_result rate = trusted ? RATE_OK : rate_limit_accept(cli.sin_addr);
        if (rate == RATE_REFUSED) {
            log_refusal(&cli, "rate limit exceeded");
            const char err[] = "ERR rate limit exceeded\n";
            (void)!write(cfd, err, sizeof(err)-1);
            close(cfd);
            continue;
        }

        // Never waits for a slot: a blocked accept loop would shut out everyone
        int slot = -1;
        enum slot_result taken = client_slot_take(cfd, cli.sin_addr, trusted, &slot);
        if (taken != SLOT_OK) {
            log_refusal(&cli, taken == SLOT_IP_BUSY ? "too many connections from this address"
                                                    : "all client slots in use");
            const char err[] = "ERR server busy\n";
            (void)!write(cfd, err, sizeof(err) - 1);
            close(cfd);
            rate_limit_finish(cli.sin_addr, rate == RATE_OVER_LIMIT, 0);
            continue;
        }

        struct client_arg *carg = xmalloc(sizeof(struct client_arg));
        carg->cfd = cfd;
        carg->cli = cli;
        carg->over_limit = rate == RATE_OVER_LIMIT;
        carg->slot = slot;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, carg) != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(errno));
            client_slot_release(slot);
            close(cfd);
            free(carg);
            rate_limit_finish(cli.sin_addr, rate == RATE_OVER_LIMIT, 0);
            continue;
        }
        pthread_detach(tid);                                                       // Frees its stack on exit
    }

    close(listen_fd);
    return 0;
}