#include <stdio.h>
#include <stdlib.h>
#include <zconf.h>
#include <errno.h>
#include <memory.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "common.h"
#include "server.h"
#include "tunnel.h"

struct connection *create_conn(int fd, int type, void *ptr) {
    struct connection *conn = malloc(sizeof(struct connection));
    conn->fd = fd;
    conn->type = type;
    conn->write_buf = null;
    conn->len = 0;
    conn->tag_close = false;
    conn->ptr = ptr;
    return conn;
}

void tag_close_conn(struct connection *conn, Array arr) {
    if (conn == null || conn->tag_close == true) {
        return;
    }

    conn->tag_close = true;
    addArrayForPointer(arr, conn);
    //printf("Tag close connection, fd: %d  type: %d\n", conn->fd, conn->type);
    if (conn->type == S_LISTEN_CLIENT) {
        error("The main socket closed");
        exit(1);
    } else if (conn->type == S_LISTEN_USER) {
        if (conn->ptr != null) {
            struct listen_user *p = conn->ptr;
            tag_close_conn(p->tunnel_conn, arr);

            while (!isQueueEmpty(p->queue)) {
                tag_close_conn(outQueueForPointer(p->queue), arr);
            }
            freeQueue(p->queue);
        }
    } else if (conn->type == S_UNKNOWN) {

    } else if (conn->type == S_TUNNEL) {
        if (conn->ptr != null) {
            struct tunnel *p = conn->ptr;
            tag_close_conn(p->listen_user_conn, arr);
        }
    } else if (conn->type == S_CLIENT) {
        if (conn->ptr != null) {
            struct client_conn *p = conn->ptr;
            tag_close_conn(p->user_conn, arr);
        }
    } else if (conn->type == S_USER) {
        if (conn->ptr != null) {
            struct user_conn *p = conn->ptr;
            tag_close_conn(p->client_conn, arr);
        }
    }
}

int close_conn(struct connection *conn) {
    //printf("Closed connection, fd: %d  type: %d p:%p\n", conn->fd, conn->type, conn);
    if (conn->type == S_TUNNEL) {
        remove_tunnel(conn->fd);
        struct tunnel *t = conn->ptr;
        if (t->token) {
            free(t->token);
        }
    }
    int res = close(conn->fd);
    if (conn->ptr) {
        free(conn->ptr);
    }
    if (conn->write_buf && conn->len > 0) {
        free(conn->write_buf);
    }
    free(conn);
    return res;
}

void close_conn_arr(Array arr) {
    for (int i = 0; i < arr->size; i++) {
        close_conn(getArrayForPointer(arr, i));
    }
}

int write_data(struct connection *conn, const void *buf, size_t len) {
    //???????????????????????????
    if (conn->len > 0) {
        void *bs = malloc(conn->len + len);
        memcpy(bs, conn->write_buf, conn->len);
        memcpy(bs + conn->len, buf, len);
        printf("WF %p\n", conn->write_buf);
        free(conn->write_buf);
        conn->write_buf = bs;
        conn->len = conn->len + len;
        return 1;
    }
    ssize_t res = write(conn->fd, buf, len);
    if ((res == -1 && EAGAIN != errno) || res == 0) {
        //????????????????????????????????????
        perror("write");
        return -1;
    } else if (res == -1) {
        conn->write_buf = malloc(len);
        memcpy(conn->write_buf, buf, len);
        conn->len = len;
        return 1;
    } else if (res < len) {
        conn->write_buf = malloc(len - res);
        memcpy(conn->write_buf, buf + res, len - res);
        conn->len = len - res;
        return 1;
    }
    return 0;
}

int make_socket_non_blocking(int fd) {
    int flags, s;
    // ????????????flag
    flags = fcntl(fd, F_GETFL, 0);
    if (-1 == flags) {
        perror("Get fd status");
        return -1;
    }

    flags |= O_NONBLOCK;

    // ??????flag
    s = fcntl(fd, F_SETFL, flags);
    if (-1 == s) {
        perror("Set fd status");
        return -1;
    }
    return 0;
}

int make_socket_block(int s) {
    int mode = fcntl(s, F_GETFL, 0);
    if (mode == -1)
        return -1;
    if (mode & O_NONBLOCK)
        return fcntl(s, F_SETFL, mode & ~O_NONBLOCK);
    return 0;
}

void log_info(const char *str) {
    syslog(LOG_USER | LOG_INFO, "[INFO] %s\n", str);
}

void log_err(const char *str) {
    syslog(LOG_USER | LOG_INFO, "[ERROR] %s\n", str);
}

boolean is_use(int port) {
    boolean b = true;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        goto END;
    }

    int on = 1;
    // ?????? socket ????????????, ??????????????????????????? Address already in use
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        goto END;
    }

    if (bind(fd, (const struct sockaddr *) &addr, sizeof(addr)) == -1) {
        goto END;
    }
    b = false;
    END:
    close(fd);
    return b;
}