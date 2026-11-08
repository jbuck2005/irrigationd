// irrigationctl - send one command to irrigationd and print its reply.
//
// The command goes over TLS, using the token as a pre-shared key (see tls.h),
// so the token itself is never sent. The token comes from IRRIGATION_TOKEN, or
// from a TOKEN= field in the command, which is then left out of what is sent.
//
// IRRIGATION_PLAIN=1 sends the command in plain text instead, as before, for
// an irrigationd that predates TLS. The token is then readable on the network.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "tls.h"

#define DEFAULT_PORT 4242
#define DEFAULT_HOST "127.0.0.1"
#define LINE_SZ      512

// Copies the command into out without its TOKEN= field, and points *token at
// that field's value inside token_buf. Returns -1 if the command is too long.
static int split_token(const char *cmd, char *out, size_t out_sz,
                       char *token_buf, size_t token_sz, const char **token) {
    char copy[LINE_SZ];
    if (strlen(cmd) >= sizeof(copy)) {
        return -1;
    }
    strcpy(copy, cmd);
    out[0] = '\0';
    *token = NULL;

    size_t len = 0;
    char *saveptr;
    for (char *field = strtok_r(copy, " \t", &saveptr); field != NULL;
         field = strtok_r(NULL, " \t", &saveptr)) {
        if (strncmp(field, "TOKEN=", 6) == 0) {
            if (strlen(field + 6) >= token_sz) {
                return -1;
            }
            strcpy(token_buf, field + 6);
            *token = token_buf;
            continue;
        }
        int n = snprintf(out + len, out_sz - len, "%s%s", len ? " " : "", field);
        if (n < 0 || (size_t)n >= out_sz - len) {
            return -1;
        }
        len += (size_t)n;
    }
    return 0;
}

static int connect_to_daemon(void) {
    const char *host = getenv("IRRIGATION_HOST");
    if (!host) host = DEFAULT_HOST;
    int port = getenv("IRRIGATION_PORT") ? atoi(getenv("IRRIGATION_PORT")) : DEFAULT_PORT;

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &servaddr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IRRIGATION_HOST '%s'\n", host);
        return -1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s \"COMMAND\"\n", argv[0]);
        fprintf(stderr, "The token comes from IRRIGATION_TOKEN, or a TOKEN= field in the command.\n");
        exit(EXIT_FAILURE);
    }

    int plain = getenv("IRRIGATION_PLAIN") && strcmp(getenv("IRRIGATION_PLAIN"), "1") == 0;

    char command[LINE_SZ];
    char token_buf[LINE_SZ];
    const char *token = NULL;
    if (split_token(argv[1], command, sizeof(command), token_buf, sizeof(token_buf), &token) != 0) {
        fprintf(stderr, "Command too long\n");
        exit(EXIT_FAILURE);
    }
    const char *env_token = getenv("IRRIGATION_TOKEN");
    if (env_token && env_token[0] != '\0') {
        token = env_token;
    } else if (token != NULL) {
        // Other users can see it in ps, and it stays in shell history
        fprintf(stderr, "Warning: a token on the command line can be seen by other users and stays in "
                        "your shell history. Set IRRIGATION_TOKEN instead.\n");
    }
    if (token == NULL || token[0] == '\0') {
        fprintf(stderr, "No token: set IRRIGATION_TOKEN, or add TOKEN=... to the command\n");
        exit(EXIT_FAILURE);
    }

    // The daemon reads one line, so it must end in a newline
    char line[2 * LINE_SZ];
    int len = plain ? snprintf(line, sizeof(line), "%s TOKEN=%s\n", command, token)
                    : snprintf(line, sizeof(line), "%s\n", command);
    if (len < 0 || (size_t)len >= sizeof(line)) {
        fprintf(stderr, "Command too long\n");
        exit(EXIT_FAILURE);
    }

    int sockfd = connect_to_daemon();
    if (sockfd < 0) {
        exit(EXIT_FAILURE);
    }

    struct conn conn = { .fd = sockfd, .ssl = NULL };
    SSL_CTX *ctx = NULL;
    if (!plain) {
        ctx = tls_client_ctx(token);
        conn.ssl = ctx ? SSL_new(ctx) : NULL;
        if (conn.ssl == NULL || SSL_set_fd(conn.ssl, sockfd) != 1) {
            fprintf(stderr, "TLS setup failed\n");
            exit(EXIT_FAILURE);
        }
        if (SSL_connect(conn.ssl) != 1) {
            ERR_clear_error();
            fprintf(stderr, "TLS handshake failed: the token is wrong, the server isn't irrigationd, "
                            "or irrigationd is too old for TLS (IRRIGATION_PLAIN=1 sends it in plain text)\n");
            SSL_free(conn.ssl);
            close(sockfd);
            exit(EXIT_FAILURE);
        }
    }

    if (conn_write(&conn, line, (size_t)len) < 0) {
        fprintf(stderr, "Sending the command failed\n");
        conn_shutdown(&conn);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // The reply is one line
    char buf[256];
    size_t got = 0;
    while (got < sizeof(buf) - 1) {
        ssize_t n = conn_read(&conn, buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        if (memchr(buf, '\n', got)) break;
    }
    buf[got] = '\0';
    if (got > 0) {
        printf("Response: %s\n", buf);
    }

    conn_shutdown(&conn);
    SSL_CTX_free(ctx);
    close(sockfd);
    return got > 0 ? 0 : EXIT_FAILURE;
}
