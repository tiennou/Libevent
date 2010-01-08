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

#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

/* XXX: Remove */
#include <dnet.h>

#include "event2/buffer.h"
#include "event2/buffer_compat.h"
#include "event2/util.h"
#include "evbuffer-internal.h"
#include "evthread-internal.h"
#include "mm-internal.h"

#include "event2/evsocket.h"

int evsocket_type(evutil_socket_t fd) {
#ifdef WIN32
#error WIN32
#else
    int type = -1;
    socklen_t typelen = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typelen) != 0) {
        event_warn("getsockopt");
    }

    return type;
#endif
}

#ifdef USE_IOVEC_IMPL
#ifdef _EVENT_HAVE_SYS_UIO_H
/* number of iovec we use for writev, fragmentation is going to determine
* how much we end up writing */

#define DEFAULT_WRITE_IOVEC 128

#if defined(UIO_MAXIOV) && UIO_MAXIOV < DEFAULT_WRITE_IOVEC
#define NUM_WRITE_IOVEC UIO_MAXIOV
#elif defined(IOV_MAX) && IOV_MAX < DEFAULT_WRITE_IOVEC
#define NUM_WRITE_IOVEC IOV_MAX
#else
#define NUM_WRITE_IOVEC DEFAULT_WRITE_IOVEC
#endif
// TODO: Remove those, use evbuffer_iovec
#define IOV_TYPE struct iovec
#define IOV_PTR_FIELD iov_base
#define IOV_LEN_FIELD iov_len
#else
#define NUM_WRITE_IOVEC 16
#define IOV_TYPE WSABUF
#define IOV_PTR_FIELD buf
#define IOV_LEN_FIELD len
#endif
#endif
#define NUM_READ_IOVEC 4

#ifdef USE_SENDFILE
int evsocket_sendfile(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch)
{
    struct evbuffer_chain *chain = buffer->first;
    struct evbuffer_chain_fd *info =
    EVBUFFER_CHAIN_EXTRA(struct evbuffer_chain_fd, chain);
#if defined(SENDFILE_IS_MACOSX) || defined(SENDFILE_IS_FREEBSD)
    int res;
    off_t len = chain->off;
#elif defined(SENDFILE_IS_LINUX) || defined(SENDFILE_IS_SOLARIS)
    ev_ssize_t res;
    off_t offset = chain->misalign;
#endif

    ASSERT_EVBUFFER_LOCKED(buffer);

#if defined(SENDFILE_IS_MACOSX)
    res = sendfile(info->fd, fd, chain->misalign, &len, NULL, 0);
    if (res == -1 && !EVUTIL_ERR_RW_RETRIABLE(errno))
        return (-1);

    return (len);
#elif defined(SENDFILE_IS_FREEBSD)
    res = sendfile(info->fd, fd, chain->misalign, chain->off, NULL, &len, 0);
    if (res == -1 && !EVUTIL_ERR_RW_RETRIABLE(errno))
        return (-1);

    return (len);
#elif defined(SENDFILE_IS_LINUX)
    /* TODO(niels): implement splice */
    res = sendfile(fd, info->fd, &offset, chain->off);
    if (res == -1 && EVUTIL_ERR_RW_RETRIABLE(errno)) {
        /* if this is EAGAIN or EINTR return 0; otherwise, -1 */
        return (0);
    }
    return (res);
#elif defined(SENDFILE_IS_SOLARIS)
    res = sendfile(fd, info->fd, &offset, chain->off);
    if (res == -1 && EVUTIL_ERR_RW_RETRIABLE(errno)) {
        /* if this is EAGAIN or EINTR return 0; otherwise, -1 */
        return (0);
    }
    return (res);
#endif
}
#endif

#ifdef USE_IOVEC_IMPL
int evsocket_readv(struct evbuffer *buf, evutil_socket_t fd, ev_ssize_t howmuch)
{
	struct evbuffer_chain *chain, **chainp;
	int n;
	int nvecs, i, remaining;

	if (howmuch < 0 || howmuch > n)
		howmuch = n;

	ASSERT_EVBUFFER_LOCKED(buf);

	chain = buf->last;

	/* Since we can use iovecs, we're willing to use the last
	 * NUM_READ_IOVEC chains. */
	if (_evbuffer_expand_fast(buf, howmuch, NUM_READ_IOVEC) == -1)
		return (-1);
    IOV_TYPE vecs[NUM_READ_IOVEC];
#ifdef _EVBUFFER_IOVEC_IS_NATIVE
	nvecs = _evbuffer_read_setup_vecs(buf, howmuch, vecs,
		NUM_READ_IOVEC, &chainp, 1);
#else
	/* We aren't using the native struct iovec.  Therefore,
	 we are on win32. */
	struct evbuffer_iovec ev_vecs[NUM_READ_IOVEC];
	nvecs = _evbuffer_read_setup_vecs(buf, howmuch, ev_vecs, NUM_READ_IOVEC,
		&chainp, 1);

	for (i=0; i < nvecs; ++i)
		WSABUF_FROM_EVBUFFER_IOV(&vecs[i], &ev_vecs[i]);
#endif /* _EVBUFFER_IOVEC_IS_NATIVE */
    
#ifdef WIN32
	DWORD bytesRead;
	DWORD flags=0;
	if (WSARecv(fd, vecs, nvecs, &bytesRead, &flags, NULL, NULL)) {
		/* The read failed. It might be a close,
		* or it might be an error. */
		if (WSAGetLastError() == WSAECONNABORTED)
			n = 0;
		else
			n = -1;
	} else
		n = bytesRead;
#else
	n = readv(fd, vecs, nvecs);
#endif

	if (n == -1 || n == 0)
		return n;

	remaining = n;
	for (i = 0; i < nvecs; ++i) {
		ev_ssize_t space = CHAIN_SPACE_LEN(*chainp);
		if (space < remaining) {
			(*chainp)->off += space;
			remaining -= space;
		} else {
			(*chainp)->off += remaining;
			buf->last_with_datap = chainp;
			break;
		}
		chainp = &(*chainp)->next;
	}

	return n;
}

int evsocket_writev(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch)
{
	IOV_TYPE iov[NUM_WRITE_IOVEC];
	struct evbuffer_chain *chain = buffer->first;
	int n, i = 0;

	if (howmuch < 0)
		return -1;

	ASSERT_EVBUFFER_LOCKED(buffer);
	/* XXX make this top out at some maximal data length?  if the
	 * buffer has (say) 1MB in it, split over 128 chains, there's
	 * no way it all gets written in one go. */
	while (chain != NULL && i < NUM_WRITE_IOVEC && howmuch) {
#ifdef USE_SENDFILE
		/* we cannot write the file info via writev */
		if (chain->flags & EVBUFFER_SENDFILE)
			break;
#endif
		iov[i].IOV_PTR_FIELD = chain->buffer + chain->misalign;
		if ((size_t)howmuch >= chain->off) {
			iov[i++].IOV_LEN_FIELD = chain->off;
			howmuch -= chain->off;
		} else {
			iov[i++].IOV_LEN_FIELD = howmuch;
			break;
		}
		chain = chain->next;
	}
#ifdef WIN32
	{
		DWORD bytesSent;
		if (WSASend(fd, iov, i, &bytesSent, 0, NULL, NULL))
			n = -1;
		else
			n = bytesSent;
	}
#else
	n = writev(fd, iov, i);
#endif
	return (n);
}
#endif /* USE_IOVEC_IMPL */

int evsocket_send(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch)
{
	ASSERT_EVBUFFER_LOCKED(buffer);
	void *p = evbuffer_pullup(buffer, howmuch);
	return send(fd, p, howmuch, 0);
}

int evsocket_recv(struct evbuffer *buf, evutil_socket_t fd, ev_ssize_t howmuch)
{
	struct evbuffer_chain *chain;
	int n = EVBUFFER_MAX_READ;
	unsigned char *p;

	if (howmuch < 0 || howmuch > n)
		howmuch = n;

	ASSERT_EVBUFFER_LOCKED(buf);
	/* If we don't have FIONREAD, we might waste some space here */
	/* XXX we _will_ waste some space here if there is any space left
	 * over on buf->last. */
	if ((chain = evbuffer_expand_singlechain(buf, howmuch)) == NULL) {
		return (-1);
	}

	/* We can append new data at this point */
	p = chain->buffer + chain->misalign + chain->off;

#ifndef WIN32
	n = read(fd, p, howmuch);
#else
	n = recv(fd, p, howmuch, 0);
#endif

	if (n == -1 || n == 0)
		return (n);

	buf->total_len += n;
	buf->n_add_for_cb += n;

	return n;
}

int evsocket_write(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch)
{
	ASSERT_EVBUFFER_LOCKED(buffer);
	void *p = evbuffer_pullup(buffer, howmuch);
	return write(fd, p, howmuch);
}

int evsocket_recvmsg(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch)
{
	struct evbuffer_chain **chainp;
	struct msghdr msg;
	struct evsocket_info *info;
	int n = -1;

	memset(&msg, 0, sizeof(struct msghdr));

	ASSERT_EVBUFFER_LOCKED(buffer);

	if (_evbuffer_expand_fast(buffer, howmuch, NUM_READ_IOVEC) == -1)
		return (-1);

	msg.msg_iov = mm_calloc(NUM_READ_IOVEC, sizeof(struct evbuffer_iovec));
	msg.msg_iovlen = _evbuffer_read_setup_vecs(buffer, howmuch, msg.msg_iov, NUM_READ_IOVEC, &chainp, 1);

#ifdef WIN32
#error WIN32
#endif

	n = recvmsg(fd, &msg, 0);
	if (n < 0) {
        event_err(-1, "recvmsg %d", n);
	} else {
        info = mm_malloc(sizeof(struct evsocket_info));
        info->addr = mm_malloc(sizeof(struct sockaddr_storage));
		memcpy(info->addr, &msg.msg_name, msg.msg_namelen);
		info->addr_len = msg.msg_namelen;
        
		evbuffer_set_info(buffer, info);
	}
	return n;
}

int evsocket_sendmsg(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch)
{
	struct evbuffer_chain *chain = buffer->first;
	struct msghdr msg;
	struct evsocket_info *info = evbuffer_get_info(buffer);
	int n = -1;
	int i = 0;

	ASSERT_EVBUFFER_LOCKED(buffer);

	msg.msg_iov = mm_calloc(NUM_WRITE_IOVEC, sizeof(struct iovec));
	msg.msg_iovlen = NUM_WRITE_IOVEC;

	while (chain != NULL && i < NUM_WRITE_IOVEC && howmuch) {
#ifdef USE_SENDFILE
		/* we cannot write the file info via sendmsg */
		if (chain->flags & EVBUFFER_SENDFILE) {
			free(msg.msg_iov);
			return -1;
		}
#endif
		msg.msg_iov[i].iov_base = chain->buffer + chain->misalign;
		if ((size_t)howmuch >= chain->off) {
			msg.msg_iov[i++].iov_len = chain->off;
			howmuch -= chain->off;
		} else {
			msg.msg_iov[i++].iov_len = howmuch;
			break;
		}
		chain = chain->next;
	}

	msg.msg_name = info->addr;
	msg.msg_namelen = info->addr_len;
	n = sendmsg(fd, &msg, howmuch);

	return n;
}

int evsocket_recvfrom(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch)
{
	struct evbuffer_chain *chain;
	struct evsocket_info *info;
    struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	int n = -1;
	void *p;

	ASSERT_EVBUFFER_LOCKED(buffer);
	/* If we don't have FIONREAD, we might waste some space here */
	/* XXX we _will_ waste some space here if there is any space left
	 * over on buf->last. */
	if ((chain = evbuffer_expand_singlechain(buffer, howmuch)) == NULL) {
		return (-1);
	}

	/* We can append new data at this point */
	p = chain->buffer + chain->misalign + chain->off;

	n = recvfrom(fd, p, howmuch, 0, (struct sockaddr*)&addr, &addrlen);
	if (n < 0) {
		event_err(-1, "recvfrom");
	} else {
        info = mm_malloc(sizeof(struct evsocket_info));
        info->addr = mm_malloc(sizeof(struct sockaddr_storage));
		memcpy(info->addr, &addr, addrlen);
		info->addr_len = addrlen;

		evbuffer_set_info(buffer, info);
	}
	return n;
}

int evsocket_sendto(struct evbuffer *buffer, evutil_socket_t fd, ev_ssize_t howmuch)
{
	return -1;
}


/**
 * Change the callbacks used for accessing the underlying file description.
 * Passing NULL as readcb or writecb will preserve existing callback.
 */
void
evbuffer_set_fd_cbs(struct evbuffer *buffer, evsocket_cb readcb, evsocket_cb writecb)
{
	EVBUFFER_LOCK(buffer);

	if (readcb != NULL)
		buffer->read_cb = readcb;
	if (writecb != NULL)
		buffer->write_cb = writecb;

	EVBUFFER_UNLOCK(buffer);
}

void evbuffer_set_info(struct evbuffer *buffer, struct evsocket_info *info)
{
	EVBUFFER_LOCK(buffer);
	if (info != buffer->info) {
		if (buffer->info != NULL)
			mm_free(buffer->info);
		buffer->info = info;
	}
	EVBUFFER_UNLOCK(buffer);
}

struct evsocket_info *evbuffer_get_info(struct evbuffer *buffer)
{
	EVBUFFER_LOCK(buffer);
	return buffer->info;
	EVBUFFER_UNLOCK(buffer);
}