// TLS 1.3 with a pre-shared key, shared by irrigationd and irrigationctl.
// See include/tls.h for the scheme.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include "tls.h"

// The only cipher suite offered. Its hash must be SHA-256, the hash the
// pre-shared key is used with.
#define TLS_SUITE "TLS_AES_128_GCM_SHA256"
static const unsigned char TLS_SUITE_ID[2] = { 0x13, 0x01 };

// One process has one token, so one key.
static unsigned char g_psk[TLS_PSK_LEN];

int tls_derive_psk(const char *token, unsigned char key[TLS_PSK_LEN]) {
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    unsigned int len = 0;
    int ok = md != NULL &&
             EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(md, TLS_PSK_LABEL, strlen(TLS_PSK_LABEL)) == 1 &&
             EVP_DigestUpdate(md, token, strlen(token)) == 1 &&
             EVP_DigestFinal_ex(md, key, &len) == 1 &&
             len == TLS_PSK_LEN;
    EVP_MD_CTX_free(md);
    return ok ? 0 : -1;
}

static void print_ssl_errors(const char *what) {
    unsigned long err = ERR_get_error();
    char buf[256];
    if (err == 0) {
        fprintf(stderr, "%s failed\n", what);
    }
    while (err != 0) {
        ERR_error_string_n(err, buf, sizeof(buf));
        fprintf(stderr, "%s failed: %s\n", what, buf);
        err = ERR_get_error();
    }
}

// A session that holds the pre-shared key, as OpenSSL wants it for TLS 1.3.
static SSL_SESSION *psk_session(SSL *ssl) {
    const SSL_CIPHER *cipher = SSL_CIPHER_find(ssl, TLS_SUITE_ID);
    SSL_SESSION *sess = SSL_SESSION_new();
    if (cipher == NULL || sess == NULL ||
        SSL_SESSION_set1_master_key(sess, g_psk, sizeof(g_psk)) != 1 ||
        SSL_SESSION_set_cipher(sess, cipher) != 1 ||
        SSL_SESSION_set_protocol_version(sess, TLS1_3_VERSION) != 1) {
        SSL_SESSION_free(sess);
        return NULL;
    }
    return sess;
}

// Server: an unknown identity gets no key, so its handshake fails.
static int psk_find_session(SSL *ssl, const unsigned char *identity, size_t identity_len,
                            SSL_SESSION **sess) {
    *sess = NULL;
    if (identity_len != strlen(TLS_PSK_IDENTITY) ||
        memcmp(identity, TLS_PSK_IDENTITY, identity_len) != 0) {
        return 1;
    }
    *sess = psk_session(ssl);
    return *sess != NULL;
}

// Client: offer the key under our identity.
static int psk_use_session(SSL *ssl, const EVP_MD *md, const unsigned char **identity,
                           size_t *identity_len, SSL_SESSION **sess) {
    *sess = NULL;
    if (md != NULL && md != EVP_sha256()) {
        return 1;
    }
    *sess = psk_session(ssl);
    if (*sess == NULL) {
        return 0;
    }
    *identity = (const unsigned char *)TLS_PSK_IDENTITY;
    *identity_len = strlen(TLS_PSK_IDENTITY);
    return 1;
}

static SSL_CTX *common_ctx(const SSL_METHOD *method, const char *token) {
    if (tls_derive_psk(token, g_psk) != 0) {
        fprintf(stderr, "Could not derive the TLS key from the token\n");
        return NULL;
    }
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (ctx == NULL ||
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_ciphersuites(ctx, TLS_SUITE) != 1) {
        print_ssl_errors("TLS setup");
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

SSL_CTX *tls_server_ctx(const char *token) {
    SSL_CTX *ctx = common_ctx(TLS_server_method(), token);
    if (ctx == NULL) {
        return NULL;
    }
    // Every connection is one command, so resumption tickets would only be
    // something else to protect. SSL_OP_ALLOW_NO_DHE_KEX stays unset, so every
    // handshake does a fresh key exchange.
    SSL_CTX_set_num_tickets(ctx, 0);
    SSL_CTX_set_psk_find_session_callback(ctx, psk_find_session);
    return ctx;
}

SSL_CTX *tls_client_ctx(const char *token) {
    SSL_CTX *ctx = common_ctx(TLS_client_method(), token);
    if (ctx == NULL) {
        return NULL;
    }
    // The key proves who the server is, so there is no certificate to check.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_psk_use_session_callback(ctx, psk_use_session);
    return ctx;
}

ssize_t conn_read(struct conn *c, void *buf, size_t len) {
    if (c->ssl == NULL) {
        return read(c->fd, buf, len);
    }
    int n = SSL_read(c->ssl, buf, (int)len);
    if (n > 0) {
        return n;
    }
    int err = SSL_get_error(c->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
        return 0;
    }
    if (err != SSL_ERROR_SYSCALL || errno == 0) {
        errno = EPROTO;
    }
    ERR_clear_error();
    return -1;
}

ssize_t conn_write(struct conn *c, const void *buf, size_t len) {
    if (c->ssl == NULL) {
        size_t done = 0;
        while (done < len) {
            ssize_t n = write(c->fd, (const char *)buf + done, len - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            done += (size_t)n;
        }
        return (ssize_t)done;
    }
    // Without SSL_MODE_ENABLE_PARTIAL_WRITE, SSL_write sends everything or fails
    int n = SSL_write(c->ssl, buf, (int)len);
    if (n <= 0) {
        ERR_clear_error();
        return -1;
    }
    return n;
}

void conn_shutdown(struct conn *c) {
    if (c->ssl != NULL) {
        if (SSL_is_init_finished(c->ssl)) {
            SSL_shutdown(c->ssl);
        }
        SSL_free(c->ssl);
        c->ssl = NULL;
        ERR_clear_error();
    }
}
