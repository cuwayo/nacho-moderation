/**
 * @file tls.h
 * @brief OpenSSL TLS adapter using memory BIOs, driven by the io_uring loop.
 *
 * The SSL object uses `BIO_s_mem` for both the read and write BIO; ciphertext is
 * moved explicitly by the caller (io_uring recv feeds @ref tls_feed_in, and
 * @ref tls_drain_out yields bytes to send). This is the ONLY sanctioned way to do
 * TLS here — blocking `SSL_read`/`SSL_write` on a socket is forbidden (SPEC §3).
 *
 * The pump:
 * @dot
 * digraph tls_pump {
 *   rankdir=LR;
 *   want_read  [label="SSL wants read\n(feed rbio)"];
 *   want_write [label="SSL wants write\n(drain wbio -> send)"];
 *   ok         [label="handshake/op done"];
 *   want_read  -> want_write [label="drain wbio"];
 *   want_write -> want_read  [label="recv -> feed rbio"];
 *   want_read  -> ok         [label="SSL_do_handshake=1"];
 * }
 * @enddot
 *
 * @ingroup tls
 */
#ifndef TG_TLS_H
#define TG_TLS_H

#include <stddef.h>

/** @brief TLS settings for a connection. */
typedef struct tls_config {
    const char *host; /**< Host for SNI and certificate verification. */
    int verify;       /**< 1 to verify the peer certificate chain and hostname. */
} tls_config_t;

/** @brief Outcome of a TLS pump step. */
typedef enum {
    TLS_OK = 0,     /**< Operation completed. */
    TLS_WANT_READ,  /**< Needs more ciphertext via tls_feed_in(). */
    TLS_WANT_WRITE, /**< Has ciphertext to send via tls_drain_out(). */
    TLS_ERROR       /**< Unrecoverable TLS error. */
} tls_step_t;

typedef struct tls_session tls_session_t;

/**
 * @brief Create a TLS session in connect mode.
 *
 * @param[in] cfg Settings (host and verify flag).
 *
 * @return A new session, or NULL on failure.
 * @ownership Caller must call tls_session_free().
 * @thread_safety Not thread-safe.
 */
tls_session_t *tls_session_new(const tls_config_t *cfg);

/**
 * @brief Advance the handshake.
 *
 * @param[in] s Session.
 *
 * @return TLS_OK when the handshake is complete, TLS_WANT_READ/WRITE to pump more
 *         data, or TLS_ERROR on failure.
 * @thread_safety Not thread-safe.
 */
tls_step_t tls_handshake_step(tls_session_t *s);

/**
 * @brief Report whether the handshake has completed.
 *
 * @param[in] s Session.
 *
 * @return 1 if established, else 0.
 * @thread_safety Not thread-safe.
 */
int tls_established(const tls_session_t *s);

/**
 * @brief Feed ciphertext received from the socket into the read BIO.
 *
 * @param[in] s   Session.
 * @param[in] buf Ciphertext.
 * @param[in] len Count.
 *
 * @return 0 on success, negative errno on failure.
 * @thread_safety Not thread-safe.
 */
int tls_feed_in(tls_session_t *s, const void *buf, size_t len);

/**
 * @brief Drain pending ciphertext from the write BIO to send.
 *
 * @param[in]  s      Session.
 * @param[out] buf    Destination buffer.
 * @param[in]  cap    Capacity of @p buf.
 * @param[out] out_len Receives the number of bytes available (may be 0).
 *
 * @return 0 on success, negative errno on failure.
 * @thread_safety Not thread-safe.
 */
int tls_drain_out(tls_session_t *s, void *buf, size_t cap, size_t *out_len);

/**
 * @brief Encrypt @p len plaintext bytes into the write BIO.
 *
 * @param[in]  s        Session.
 * @param[in]  buf      Plaintext.
 * @param[in]  len      Count.
 * @param[out] accepted Receives bytes accepted (may be less than @p len).
 *
 * @return TLS_OK / TLS_WANT_READ / TLS_WANT_WRITE / TLS_ERROR.
 * @thread_safety Not thread-safe.
 */
tls_step_t tls_write_plain(tls_session_t *s, const void *buf, size_t len, size_t *accepted);

/**
 * @brief Decrypt available plaintext from the read BIO.
 *
 * @param[in]  s   Session.
 * @param[out] buf Destination.
 * @param[in]  cap Capacity.
 * @param[out] got Receives bytes read (may be 0).
 *
 * @return TLS_OK / TLS_WANT_READ / TLS_WANT_WRITE / TLS_ERROR.
 * @thread_safety Not thread-safe.
 */
tls_step_t tls_read_plain(tls_session_t *s, void *buf, size_t cap, size_t *got);

/**
 * @brief Free the session and its SSL context.
 *
 * @param[in] s Session (may be NULL).
 * @thread_safety Not thread-safe.
 */
void tls_session_free(tls_session_t *s);

#endif /* TG_TLS_H */
