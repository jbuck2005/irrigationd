#ifndef IRRIGATION_TLS_H
#define IRRIGATION_TLS_H

#include <stddef.h>
#include <sys/types.h>
#include <openssl/ssl.h>

// -----------------------------------------------------------------------------
// TLS 1.3 with a pre-shared key, shared by irrigationd and irrigationctl
// -----------------------------------------------------------------------------
//
// Both ends turn the token into a 32 byte key:
//
//   key = SHA-256("irrigationd-psk-v1:" + token)
//
// and open a TLS 1.3 session with it under the identity "irrigationd". Only a
// side that knows the token can finish the handshake, so the token itself never
// crosses the network, and nothing sent inside can be read, changed or replayed.
// The handshake also does a fresh key exchange (psk_dhe_ke), so a token that
// leaks later can't decrypt traffic captured earlier. No certificates are used.
//
// A TLS connection always starts with the byte TLS_RECORD_HANDSHAKE, which a
// plain text command never does, so the daemon tells them apart on one port.
// -----------------------------------------------------------------------------

#define TLS_PSK_LABEL        "irrigationd-psk-v1:"
#define TLS_PSK_IDENTITY     "irrigationd"
#define TLS_PSK_LEN          32
#define TLS_RECORD_HANDSHAKE 0x16

// One client connection: plain text when ssl is NULL, TLS otherwise.
struct conn {
    int fd;
    SSL *ssl;
};

// Derives the pre-shared key from the token. Returns 0, or -1 on failure.
int tls_derive_psk(const char *token, unsigned char key[TLS_PSK_LEN]);

// Builds the context for one side. Returns NULL on failure (reason on stderr).
SSL_CTX *tls_server_ctx(const char *token);
SSL_CTX *tls_client_ctx(const char *token);

// Read and write on either kind of connection, like read() and write().
ssize_t conn_read(struct conn *c, void *buf, size_t len);
ssize_t conn_write(struct conn *c, const void *buf, size_t len);

// Sends close_notify if TLS, then frees the session. Does not close the fd.
void conn_shutdown(struct conn *c);

#endif
