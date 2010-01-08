#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <sys/stat.h>
#include <sys/queue.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>

#include <dnet.h>
#include <event.h>
#include <event2/evsocket.h>

int
make_socket_ai(int (*f)(int, const struct sockaddr *, socklen_t),
               int type,
               struct addrinfo *ai)
{
    struct linger linger;
    int fd, on = 1;
    
    /* Create listen socket */
    fd = socket(AF_INET, type, 0);
    if (fd == -1) {
        warn("socket");
        return -1;
    }
    
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        warn("fcntl(O_NONBLOCK)");
        goto out;
    }
    
    if (fcntl(fd, F_SETFD, 1) == -1) {
        warn("fcntl(F_SETFD)");
        goto out;
    }
    
    if (type == SOCK_STREAM) {
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (void *)&on, sizeof(on));
#endif
        linger.l_onoff = 1;
        linger.l_linger = 5;
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
    }
    
    if ((f)(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
        if (errno != EINPROGRESS) {
            warn("%s: f()", __FUNCTION__);
            goto out;
        }
    }
    
    return fd;
    
out:
    close(fd);
    return -1;
}

int
make_socket(int (*f)(int, const struct sockaddr *, socklen_t),
            int type,
            char *address,
            uint16_t port)
{
    struct addrinfo ai, *aitop;
    char strport[NI_MAXSERV];
    int fd;
    
    memset(&ai, 0, sizeof(ai));
    ai.ai_family = AF_INET;
    ai.ai_socktype = type;
    ai.ai_flags = f != connect ? AI_PASSIVE : 0;
    snprintf(strport, sizeof(strport), "%d", port);
    if (getaddrinfo(address, strport, &ai, &aitop) != 0) {
        warn("%s: getaddrinfo", __FUNCTION__);
        return -1;
    }
    
    fd = make_socket_ai(f, type, aitop);
    
    freeaddrinfo(aitop);
    
    return fd;
}

struct ev_msg {
    struct addr     addr;
    in_port_t       port;
    
    size_t          datalen;
    char            data[];
};

struct gbl {
    struct event_base *base;
    struct bufferevent *ev_socket;
    struct bufferevent *ev_filter;
    int fd;
};


void ev_log_cb(int severity, const char *msg) {
    warnx("%d, %s", severity, msg);
}

struct ev_msg *evmsg_read(struct bufferevent *bev) {
    struct ev_msg *msg = calloc(1, sizeof(struct ev_msg));
    void *tmp = NULL;
    size_t readlen = 0;
    
    /* First read the header */
    readlen = bufferevent_read(bev, msg, sizeof(struct ev_msg));
    if (readlen != sizeof(struct ev_msg)) {
        warnx("Header too short");
        goto error;
    }
    
    /* Reserve room for payload */
    tmp = realloc(msg, sizeof(struct ev_msg) + msg->datalen);
    if (tmp == NULL) {
        warnx("%s: realloc", __FUNCTION__);
        goto error;
    }
    msg = tmp;
    
    /* Read the payload */
    readlen = bufferevent_read(bev, &msg->data, msg->datalen);
    if (readlen != msg->datalen) {
        warnx("%s: too short");
        goto error;
    }
    
    return msg;
error:
    free(msg);
    
    return NULL;
}

struct ev_msg * evmsg_recv(int fd) {
    struct ev_msg *msg = NULL;
    const int BUFFER_SIZE = 1024;
    struct sockaddr sockaddr;
    socklen_t sockaddr_len = sizeof(sockaddr);
    
    msg = malloc(sizeof(struct ev_msg) + BUFFER_SIZE);
    if (msg == NULL)
        goto error;
    
    /* Receive the message from the network */
    msg->datalen = recvfrom(fd, msg->data, 0, 0, &sockaddr, &sockaddr_len);
    if (msg->datalen == -1) {
        warn("%s: recvfrom", __FUNCTION__);
        goto error;
    }
    if (msg->datalen >= BUFFER_SIZE) {
        /* TODO: Check for big message */
        warn("%s: recvfrom filled its buffer", __FUNCTION__);
    }
    
    /* Convert network address */
    if(addr_ston(&sockaddr, &msg->addr) != 0) {
        warn("%s: addr_ston", __FUNCTION__);
        goto error;
    }
    msg->port = ntohs(((struct sockaddr_in *)&sockaddr)->sin_port);
    
    return msg;
error:
    if (msg != NULL)
        free(msg);
    
    return NULL;
}

int evmsg_send(int fd, struct ev_msg *msg) {
    struct sockaddr_in sockaddr;
    int res = 0;
    
    res = addr_ntos(&msg->addr, (struct sockaddr *)&sockaddr);
    if (res != 0) {
        warnx("%s: addr_ntos", __FUNCTION__);
        goto error;
    }
    sockaddr.sin_port = htons(msg->port);
    
    res = sendto(fd, msg->data, msg->datalen, 0, (struct sockaddr *)&sockaddr, sizeof(struct sockaddr_in));
    if (res != msg->datalen) {
        warn("%s: sendto", __FUNCTION__);
        goto error;
    }
        
    return 0;
error:
    return -1;
}

void err_cb(struct bufferevent *bev, short what, void *ctx) {
    printf("%s: %p got ", __FUNCTION__, bev);
    if (what & BEV_EVENT_READING)
        printf("read ");
    if (what & BEV_EVENT_WRITING)
        printf("write ");
    if (what & BEV_EVENT_EOF)
        printf("eof ");
    if (what & BEV_EVENT_ERROR)
        printf("error ");
    if (what & BEV_EVENT_TIMEOUT)
        printf("timeout");
    printf("\n");
}

void ev_read_cb(struct bufferevent *bev, void *ctx) {
    struct gbl *gbl = (struct gbl*)ctx;
    struct evsocket_info *info = evbuffer_get_info(bufferevent_get_input(bev));
    int fd = bufferevent_getfd(bev);
    struct addr addr;
    short port;

    /* Convert network address */
    if(addr_ston(info->addr, &addr) != 0) {
        err(-1, "%s: addr_ston", __FUNCTION__);
    }
    port = ntohs(((struct sockaddr_in *)info->addr)->sin_port);
    
    warnx("%s: <%p>, fd type %d, in=%d, out=%d, \"%s\" from %s:%d", __FUNCTION__, bev,
          evsocket_type(fd),
          evbuffer_get_length(bufferevent_get_input(bev)),
          evbuffer_get_length(bufferevent_get_output(bev)),
          "<data>", ""/*addr_ntoa(&addr)*/, port
          );
#if 0    
    struct ev_msg *msg = evmsg_recv(fd);
    if (msg != NULL) {
        warnx("ev_read: received %s from %s:%d", msg->data, addr_ntoa(&msg->addr), msg->port);
        
        /* Write the complete message to the library socket */
        bufferevent_write(gbl->ev_socket, msg, sizeof(struct ev_msg) + msg->datalen);
        bufferevent_flush(gbl->ev_socket, EV_READ, BEV_FINISHED);
        free(msg);
    }
#endif
}

void ev_write_cb(struct bufferevent *bev, void *ctx) {
    struct gbl *gbl = (struct gbl*)ctx;
    int fd = bufferevent_getfd(bev);
    
    warnx("%s: <%p> in=%d, out=%d", __FUNCTION__, bev,
          evbuffer_get_length(bufferevent_get_input(bev)),
          evbuffer_get_length(bufferevent_get_output(bev)));
    
#if 0
    struct ev_msg *msg = evmsg_read(gbl->ev_socket);
    if (msg != NULL) {
        warnx("ev_write: writing %s to %s:%d", msg->data, addr_ntoa(&msg->addr), msg->port);
        
        evmsg_send(fd, msg);
    } else {
        warnx("ev_write: evmsg_read failed");
    }
#endif
}

enum bufferevent_filter_result
ev_filter_input(struct evbuffer *src,
                struct evbuffer *dst,
                ev_ssize_t dst_limit,
                enum bufferevent_flush_mode mode,
                void *ctx) {
    const u_char *buffer;
    size_t buffer_len;
    int res = 0;
    warnx("%s: src=%d, dst=%d", __FUNCTION__, evbuffer_get_length(src), evbuffer_get_length(dst));
    
    buffer_len = evbuffer_get_length(src);
    buffer = evbuffer_pullup(src, buffer_len);
    
    res = evbuffer_add(dst, buffer, buffer_len);
    if (res != 0)
        warnx("%s: evbuffer_add", __FUNCTION__);
    
    res = evbuffer_drain(src, buffer_len);
    if (res != 0)
        warnx("%s: evbuffer_drain", __FUNCTION__);
    
    warnx("%s: src=%d, dst=%d", __FUNCTION__, evbuffer_get_length(src), evbuffer_get_length(dst));
    return BEV_OK;
}

enum bufferevent_filter_result
ev_filter_output(struct evbuffer *src,
                 struct evbuffer *dst,
                 ev_ssize_t dst_limit,
                 enum bufferevent_flush_mode mode,
                 void *ctx) {
    const u_char *buffer;
    size_t buffer_len;
    int res = 0;
    warnx("%s: src=%d, dst=%d", __FUNCTION__, evbuffer_get_length(src), evbuffer_get_length(dst));
    
    buffer_len = evbuffer_get_length(src);
    buffer = evbuffer_pullup(src, buffer_len);
    
    res = evbuffer_add(dst, buffer, buffer_len);
    if (res != 0)
        warnx("%s: evbuffer_add", __FUNCTION__);
    
    res = evbuffer_drain(src, buffer_len);
    if (res != 0)
        warnx("%s: evbuffer_drain", __FUNCTION__);
    
    warnx("%s: src=%d, dst=%d", __FUNCTION__, evbuffer_get_length(src), evbuffer_get_length(dst));
    return BEV_OK;
}

int main(int argc, const char * argv[]) {
    struct gbl gbl;
    int ret;
    
    event_set_log_callback(ev_log_cb);
    
    gbl.base = event_base_new();
    
    gbl.fd = make_socket(bind, SOCK_DGRAM, "0.0.0.0", 1234);
    warnx("created listening socket on 0.0.0.0:1234");
    
    gbl.ev_socket = bufferevent_socket_new(gbl.base, gbl.fd, 0);
    if (gbl.ev_socket == NULL)
        warnx("bufferevent_pair_new: %d", ret);
    
    bufferevent_setcb(gbl.ev_socket, ev_read_cb, ev_write_cb, err_cb, &gbl);
    
//    evbuffer_set_fd_cbs(bufferevent_get_input(gbl.ev_socket), evsocket_recvfrom, evsocket_sendto);
    
    bufferevent_enable(gbl.ev_socket, EV_READ);
/*
    gbl.ev_filter = bufferevent_filter_new(gbl.ev_pair[cli], ev_filter_input, ev_filter_output, 0, NULL, &gbl);
    bufferevent_enable(gbl.ev_filter, EV_READ | EV_WRITE);
*/
    
    ret = event_base_dispatch(gbl.base);
    
    return ret;
}
