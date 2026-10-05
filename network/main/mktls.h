/* -*- Mode: C; tab-width: 4 -*-
 *   mktls.h --- TLS for netlib, from OpenSSL, as an NSPR I/O layer.
 *
 * The source release left SSL out (modules/security/freenav has stubs
 * only).  This puts TLS (1.2 and later) under an existing TCP socket:
 * once NET_TLS_Wrap has pushed the layer, PR_Read, PR_Write, PR_Poll and
 * PR_Close on the socket go through OpenSSL, so the protocol code reads
 * and writes as it does over plain TCP.
 */

#ifndef MKTLS_H
#define MKTLS_H

#include "xp.h"
#include "prio.h"

XP_BEGIN_PROTOS

/* Put TLS on SOCK (a connected, non-blocking TCP socket) for a server
 * named HOST (no port): SNI and certificate name checks use it.
 * Returns 0, or a negative MK_ error with *error_msg set. */
extern int NET_TLS_Wrap(PRFileDesc *sock, const char *host, char **error_msg);

/* Advance the handshake.  Returns MK_CONNECTED once it is done,
 * MK_WAITING_FOR_CONNECTION while it waits for the socket (*want_write
 * tells whether for writing or for reading), or a negative MK_ error with
 * *error_msg set (a certificate that does not verify, for one). */
extern int NET_TLS_Handshake(PRFileDesc *sock, XP_Bool *want_write,
							 char **error_msg);

/* Is SOCK a TLS socket? */
extern XP_Bool NET_TLS_IsTLS(PRFileDesc *sock);

/* Decrypted bytes OpenSSL holds for SOCK that the next PR_Read returns
 * without reading the socket.  select() cannot see them: a reader must
 * come back for them by itself. */
extern PRInt32 NET_TLS_Pending(PRFileDesc *sock);

/* "TLSv1.3, TLS_AES_256_GCM_SHA384" for the connection on SOCK, or NULL.
 * The string is the caller's (PR_Free). */
extern char *NET_TLS_Describe(PRFileDesc *sock);

XP_END_PROTOS

#endif /* MKTLS_H */
