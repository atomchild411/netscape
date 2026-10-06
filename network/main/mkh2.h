/* -*- Mode: C; tab-width: 4 -*-
 *   mkh2.h --- HTTP/2 for netlib, from nghttp2, behind HTTP/1.1 sockets.
 *
 * A TLS connection whose server picks "h2" (ALPN) becomes a session, and
 * each request on it a stream.  A stream is a socket to the HTTP code:
 * what it writes is an HTTP/1.1 request, which goes out as HTTP/2 frames,
 * and what it reads is an HTTP/1.1 response ("Connection: close", the body
 * ending with the stream), made from the frames that come back.  So the
 * HTTP state machine, its header parsing, cookies, redirects, caching and
 * authentication work as they do for HTTP/1.1; what changes is that later
 * requests to the host need no new connection or handshake, and run side
 * by side on the one connection.
 *
 * A stream's descriptor is a duplicate of the session's socket under a
 * layer of its own: select() and PR_Poll see the connection, and the
 * layer's poll reports data the session already holds for the stream.
 */

#ifndef MKH2_H
#define MKH2_H

#include "xp.h"
#include "prio.h"

XP_BEGIN_PROTOS

/* Is HTTP/2 to be offered?  (NS_HTTP2=0 in the environment says no.) */
extern XP_Bool NET_H2_Enabled(void);

/* A new stream on a live session to HOST ("name" or "name:port"), or NULL
 * if there is none (or it has no room for another stream). */
extern PRFileDesc *NET_H2_StreamFor(const char *host);

/* SOCK is a new connection to HOST, for https.  Until its handshake says
 * whether the server speaks HTTP/2, further requests to HOST wait for it
 * (NET_H2_StreamFor gives them streams on it) instead of each making a
 * connection of its own.  Returns TRUE if it is so registered: then one of
 * NET_H2_Adopt, NET_H2_NotH2 or NET_H2_Abandon must follow for SOCK. */
extern XP_Bool NET_H2_Connecting(PRFileDesc *sock, const char *host);

/* SOCK's server speaks HTTP/1.1: requests waiting for it fail (to be tried
 * again on connections of their own), and HOST is not waited for again. */
extern void NET_H2_NotH2(PRFileDesc *sock, const char *host);

/* SOCK failed or was given up before its handshake ended. */
extern void NET_H2_Abandon(PRFileDesc *sock);

/* SOCK, a TLS socket whose handshake picked h2, becomes the session for
 * HOST; the session owns it from now on.  Returns the first stream, or NULL
 * (SOCK is then closed). */
extern PRFileDesc *NET_H2_Adopt(PRFileDesc *sock, const char *host);

/* Is FD a stream? */
extern XP_Bool NET_H2_IsStream(PRFileDesc *fd);

XP_END_PROTOS

#endif /* MKH2_H */
