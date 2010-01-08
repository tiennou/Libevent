/*
 * Copyright (c) 2010 Etienne Samson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _EVENT2_EVSOCKET_H_
#define _EVENT2_EVSOCKET_H_

/* send file support */
#if defined(_EVENT_HAVE_SYS_SENDFILE_H) && defined(_EVENT_HAVE_SENDFILE) && defined(__linux__)
#define USE_SENDFILE		1
#define SENDFILE_IS_LINUX	1
#elif defined(_EVENT_HAVE_SENDFILE) && defined(__FreeBSD__)
#define USE_SENDFILE		1
#define SENDFILE_IS_FREEBSD	1
#elif defined(_EVENT_HAVE_SENDFILE) && defined(__APPLE__)
#define USE_SENDFILE		1
#define SENDFILE_IS_MACOSX	1
#elif defined(_EVENT_HAVE_SENDFILE) && defined(__sun__) && defined(__svr4__)
#define USE_SENDFILE		1
#define SENDFILE_IS_SOLARIS	1
#endif

/* iovec support */
#if defined(_EVENT_HAVE_SYS_UIO_H) || defined(WIN32)
#define USE_IOVEC_IMPL
#endif

/* Additional info available to clients */
struct evsocket_info {
    struct sockaddr *addr;
    socklen_t addr_len;
};

int evsocket_type(evutil_socket_t fd);

typedef int (*evsocket_cb)(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);

#ifdef USE_SENDFILE
int evsocket_sendfile(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);
#endif

#ifdef USE_IOVEC_IMPL
int evsocket_readv(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);
int evsocket_writev(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);
#endif

int evsocket_send(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);
int evsocket_recv(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);

/* evsocket_read: use evsocket_readv */
int evsocket_write(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);

/*
evsocket_pread
evsocket_pwrite
*/

int evsocket_recvmsg(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);
int evsocket_sendmsg(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);

int evsocket_recvfrom(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);
int evsocket_sendto(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch);

void evbuffer_set_fd_cbs(struct evbuffer *buffer, evsocket_cb readcb, evsocket_cb writecb);

void evbuffer_set_info(struct evbuffer *buffer, struct evsocket_info *info);
struct evsocket_info *evbuffer_get_info(struct evbuffer *buffer);

#endif