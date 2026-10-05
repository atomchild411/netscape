/* -*- Mode: C; tab-width: 4 -*-
 *   mktls.c --- TLS for netlib, from OpenSSL, as an NSPR I/O layer.
 *
 * See mktls.h.  The layer sits on top of the NSPR TCP socket and talks to
 * the server through the socket's OS descriptor (SSL_set_fd): the socket
 * stays non-blocking, and SSL_read/SSL_write wanting more I/O come back as
 * PR_WOULD_BLOCK_ERROR, which netlib already handles.
 *
 * Certificates are checked against OpenSSL's default store (on IRIX,
 * pkgsrc's /opt/pkgsrc/etc/openssl/certs from mozilla-rootcerts-openssl;
 * SSL_CERT_FILE and SSL_CERT_DIR override it), with the server's name.
 * A build that carries its own roots (the IRIX tardist, with OpenSSL linked
 * in) names them with -DNS_CA_FILE="path": they are trusted as well.
 */

#include "mkutils.h"
#include "mktls.h"

#include "prerror.h"
#include "prmem.h"
#include "plstr.h"
#include "prprf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

extern int MK_UNABLE_TO_CONNECT;
extern int MK_TCP_READ_ERROR;
extern int MK_TCP_WRITE_ERROR;

typedef struct NetTLS {
	SSL *ssl;
	char *host;
	XP_Bool handshake_done;
} NetTLS;

static SSL_CTX *net_tls_ctx = NULL;
static int net_tls_debug = -1;

/* NETSCAPE_NET_DEBUG=1 in the environment: log the handshake to stderr. */
static void
net_tls_log(const char *fmt, ...)
{
	va_list ap;

	if (net_tls_debug < 0)
		net_tls_debug = getenv("NETSCAPE_NET_DEBUG") != NULL;
	if (!net_tls_debug)
		return;
	va_start(ap, fmt);
	fputs("tls: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
static PRDescIdentity net_tls_identity = PR_INVALID_IO_LAYER;
static PRIOMethods net_tls_methods;

static NetTLS *
net_tls_of(PRFileDesc *fd)
{
	PRFileDesc *layer;

	if (fd == NULL || net_tls_identity == PR_INVALID_IO_LAYER)
		return NULL;
	layer = PR_GetIdentitiesLayer(fd, net_tls_identity);
	return layer ? (NetTLS *) layer->secret : NULL;
}

/* The SSL error queue as one line, for an error message. */
static char *
net_tls_error_text(const char *what, int ssl_error)
{
	char buf[256];
	unsigned long e = ERR_get_error();

	if (e)
		ERR_error_string_n(e, buf, sizeof buf);
	else if (ssl_error == SSL_ERROR_SYSCALL)
		PR_snprintf(buf, sizeof buf, "connection closed (errno %d)", errno);
	else
		PR_snprintf(buf, sizeof buf, "SSL error %d", ssl_error);
	ERR_clear_error();
	return PR_smprintf("%s: %s", what, buf);
}

/* Map an SSL_read/SSL_write result to NSPR's conventions. */
static PRInt32
net_tls_io_result(NetTLS *tls, int rv)
{
	int err;

	if (rv > 0)
		return rv;
	err = SSL_get_error(tls->ssl, rv);
	switch (err) {
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
		PR_SetError(PR_WOULD_BLOCK_ERROR, 0);
		return -1;
	case SSL_ERROR_ZERO_RETURN:	/* close_notify: end of data */
		return 0;
	case SSL_ERROR_SYSCALL:
		/* An EOF without close_notify: many servers end that way. */
		if (ERR_peek_error() == 0 && errno == 0)
			return 0;
		/* fall through */
	default:
		ERR_clear_error();
		PR_SetError(PR_IO_ERROR, err);
		return -1;
	}
}

static PRInt32 PR_CALLBACK
net_tls_read(PRFileDesc *fd, void *buf, PRInt32 amount)
{
	NetTLS *tls = (NetTLS *) fd->secret;

	if (amount <= 0)
		return 0;
	errno = 0;
	return net_tls_io_result(tls, SSL_read(tls->ssl, buf, amount));
}

static PRInt32 PR_CALLBACK
net_tls_write(PRFileDesc *fd, const void *buf, PRInt32 amount)
{
	NetTLS *tls = (NetTLS *) fd->secret;

	if (amount <= 0)
		return 0;
	errno = 0;
	return net_tls_io_result(tls, SSL_write(tls->ssl, buf, amount));
}

static PRInt32 PR_CALLBACK
net_tls_recv(PRFileDesc *fd, void *buf, PRInt32 amount, PRIntn flags,
			 PRIntervalTime timeout)
{
	return net_tls_read(fd, buf, amount);
}

static PRInt32 PR_CALLBACK
net_tls_send(PRFileDesc *fd, const void *buf, PRInt32 amount, PRIntn flags,
			 PRIntervalTime timeout)
{
	return net_tls_write(fd, buf, amount);
}

static PRInt32 PR_CALLBACK
net_tls_available(PRFileDesc *fd)
{
	NetTLS *tls = (NetTLS *) fd->secret;

	return SSL_pending(tls->ssl);
}

/* Data OpenSSL already decrypted is ready now, whatever the socket says. */
static PRInt16 PR_CALLBACK
net_tls_poll(PRFileDesc *fd, PRInt16 in_flags, PRInt16 *out_flags)
{
	NetTLS *tls = (NetTLS *) fd->secret;

	*out_flags = 0;
	if ((in_flags & PR_POLL_READ) && tls->handshake_done
		&& SSL_pending(tls->ssl) > 0) {
		*out_flags = PR_POLL_READ;
		return in_flags;
	}
	return (fd->lower->methods->poll)(fd->lower, in_flags, out_flags);
}

static PRStatus PR_CALLBACK
net_tls_close(PRFileDesc *fd)
{
	NetTLS *tls = (NetTLS *) fd->secret;
	PRStatus status;

	if (tls) {
		if (tls->handshake_done)
			(void) SSL_shutdown(tls->ssl);	/* best effort, non-blocking */
		SSL_free(tls->ssl);
		PR_FREEIF(tls->host);
		PR_Free(tls);
		fd->secret = NULL;
	}
	ERR_clear_error();
	status = (fd->lower->methods->close)(fd->lower);
	fd->dtor(fd);
	return status;
}

static XP_Bool
net_tls_init(char **error_msg)
{
	if (net_tls_ctx)
		return TRUE;

	if (!OPENSSL_init_ssl(0, NULL)) {
		*error_msg = net_tls_error_text("TLS initialization failed", 0);
		return FALSE;
	}
	net_tls_ctx = SSL_CTX_new(TLS_client_method());
	if (!net_tls_ctx) {
		*error_msg = net_tls_error_text("TLS initialization failed", 0);
		return FALSE;
	}
	SSL_CTX_set_min_proto_version(net_tls_ctx, TLS1_2_VERSION);
	SSL_CTX_set_verify(net_tls_ctx, SSL_VERIFY_PEER, NULL);
	{
		XP_Bool have_roots = SSL_CTX_set_default_verify_paths(net_tls_ctx);
#ifdef NS_CA_FILE
		if (SSL_CTX_load_verify_locations(net_tls_ctx, NS_CA_FILE, NULL))
			have_roots = TRUE;
		else
			net_tls_log("cannot load %s", NS_CA_FILE);
#endif
		ERR_clear_error();
		if (!have_roots) {
			*error_msg = PL_strdup("Cannot load trusted certificates.");
			SSL_CTX_free(net_tls_ctx);
			net_tls_ctx = NULL;
			return FALSE;
		}
	}
	/* Partial writes are fine: netlib writes what it is told it wrote. */
	SSL_CTX_set_mode(net_tls_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE
					 | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	net_tls_identity = PR_GetUniqueIdentity("netlib TLS");
	net_tls_methods = *PR_GetDefaultIOMethods();
	net_tls_methods.file_type = PR_DESC_LAYERED;
	net_tls_methods.close = net_tls_close;
	net_tls_methods.read = net_tls_read;
	net_tls_methods.write = net_tls_write;
	net_tls_methods.recv = net_tls_recv;
	net_tls_methods.send = net_tls_send;
	net_tls_methods.available = net_tls_available;
	net_tls_methods.poll = net_tls_poll;
	return TRUE;
}

MODULE_PRIVATE int
NET_TLS_Wrap(PRFileDesc *sock, const char *host, char **error_msg)
{
	PRFileDesc *layer;
	NetTLS *tls;
	int osfd;

	if (!net_tls_init(error_msg))
		return MK_UNABLE_TO_CONNECT;

	osfd = PR_FileDesc2NativeHandle(sock);
	tls = PR_NEWZAP(NetTLS);
	if (!tls || osfd < 0) {
		PR_FREEIF(tls);
		*error_msg = PL_strdup("TLS: no socket to secure");
		return MK_UNABLE_TO_CONNECT;
	}
	tls->host = PL_strdup(host);
	tls->ssl = SSL_new(net_tls_ctx);
	if (!tls->ssl || !SSL_set_fd(tls->ssl, osfd)
		|| !SSL_set_tlsext_host_name(tls->ssl, tls->host)
		|| !SSL_set1_host(tls->ssl, tls->host)) {
		*error_msg = net_tls_error_text("TLS setup failed", 0);
		if (tls->ssl)
			SSL_free(tls->ssl);
		PR_Free(tls->host);
		PR_Free(tls);
		return MK_UNABLE_TO_CONNECT;
	}
	SSL_set_hostflags(tls->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
	net_tls_log("wrap fd %d for %s", osfd, tls->host);
	SSL_set_connect_state(tls->ssl);

	layer = PR_CreateIOLayerStub(net_tls_identity, &net_tls_methods);
	if (!layer) {
		SSL_free(tls->ssl);
		PR_Free(tls->host);
		PR_Free(tls);
		*error_msg = PL_strdup("TLS: out of memory");
		return MK_UNABLE_TO_CONNECT;
	}
	layer->secret = (PRFilePrivate *) tls;
	/* PR_PushIOLayer swaps the descriptors' contents: SOCK stays the
	 * handle netlib holds, and is now the top (TLS) layer. */
	if (PR_PushIOLayer(sock, PR_TOP_IO_LAYER, layer) != PR_SUCCESS) {
		layer->secret = NULL;
		layer->dtor(layer);
		SSL_free(tls->ssl);
		PR_Free(tls->host);
		PR_Free(tls);
		*error_msg = PL_strdup("TLS: cannot layer the socket");
		return MK_UNABLE_TO_CONNECT;
	}
	return 0;
}

MODULE_PRIVATE int
NET_TLS_Handshake(PRFileDesc *sock, XP_Bool *want_write, char **error_msg)
{
	NetTLS *tls = net_tls_of(sock);
	int rv, err;
	long verify;

	*want_write = FALSE;
	if (!tls) {
		*error_msg = PL_strdup("TLS: not a TLS socket");
		return MK_UNABLE_TO_CONNECT;
	}
	if (tls->handshake_done)
		return MK_CONNECTED;

	errno = 0;
	rv = SSL_do_handshake(tls->ssl);
	if (rv == 1) {
		tls->handshake_done = TRUE;
		net_tls_log("%s: %s, %s", tls->host, SSL_get_version(tls->ssl),
					SSL_CIPHER_get_name(SSL_get_current_cipher(tls->ssl)));
		return MK_CONNECTED;
	}
	err = SSL_get_error(tls->ssl, rv);
	net_tls_log("%s: handshake step: ssl error %d", tls->host, err);
	if (err == SSL_ERROR_WANT_READ)
		return MK_WAITING_FOR_CONNECTION;
	if (err == SSL_ERROR_WANT_WRITE) {
		*want_write = TRUE;
		return MK_WAITING_FOR_CONNECTION;
	}

	verify = SSL_get_verify_result(tls->ssl);
	if (verify != X509_V_OK) {
		ERR_clear_error();
		*error_msg = PR_smprintf(
			"The certificate of %s could not be verified:\n%s.",
			tls->host, X509_verify_cert_error_string(verify));
	} else {
		char *what = PR_smprintf("A secure connection to %s failed", tls->host);
		*error_msg = net_tls_error_text(what ? what : "TLS handshake failed", err);
		PR_FREEIF(what);
	}
	net_tls_log("%s", *error_msg ? *error_msg : "(no message)");
	return MK_UNABLE_TO_CONNECT;
}

MODULE_PRIVATE XP_Bool
NET_TLS_IsTLS(PRFileDesc *sock)
{
	return net_tls_of(sock) != NULL;
}

MODULE_PRIVATE PRInt32
NET_TLS_Pending(PRFileDesc *sock)
{
	NetTLS *tls = net_tls_of(sock);

	return (tls && tls->handshake_done) ? SSL_pending(tls->ssl) : 0;
}

MODULE_PRIVATE char *
NET_TLS_Describe(PRFileDesc *sock)
{
	NetTLS *tls = net_tls_of(sock);

	if (!tls || !tls->handshake_done)
		return NULL;
	return PR_smprintf("%s, %s", SSL_get_version(tls->ssl),
					   SSL_CIPHER_get_name(SSL_get_current_cipher(tls->ssl)));
}
