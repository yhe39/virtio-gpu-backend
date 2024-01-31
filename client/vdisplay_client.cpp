#include "vdisplay_client.h"

extern "C"
{
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vdisplay_protocol.h"
}

#include "common.h"

#define SERVER_SOCK_PATH  "/data/local/ipc/virt_disp_server"
#define CLIENT_SOCK_PATH  "/data/local/ipc/virt_disp_client"

DisplayClient::DisplayClient(Renderer * rd) : client_sock(-1), force_exit(false), renderer(rd)
{}

int DisplayClient::start()
{
    int ret, len;
    struct sockaddr_un client_sockaddr;

    client_sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_sock == -1) {
        LOGE("SOCKET ERROR = %s\n", strerror(errno));
        return -1;
    }

    int flags;
    flags = fcntl(client_sock, F_GETFL, 0);
    fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);

    memset(&client_sockaddr, 0, sizeof(struct sockaddr_un));
    client_sockaddr.sun_family = AF_UNIX;
    strcpy(client_sockaddr.sun_path, CLIENT_SOCK_PATH);
    len = sizeof(client_sockaddr);

    ::unlink(CLIENT_SOCK_PATH);
    ret = ::bind(client_sock, (struct sockaddr *) &client_sockaddr, len);
    if (ret == -1){
        LOGE("BIND ERROR: %s\n", strerror(errno));
        shutdown(client_sock, SHUT_RDWR);
        close(client_sock);
        client_sock = -1;
        return -1;
    }

    force_exit = false;
    exit_fd = eventfd(0, 0);
    if (exit_fd == -1) {
        LOGE("failed to create exit fd\n");
        return -1;
    }

    work_tid = make_shared<thread>(work_thread, this);
    return ret;
}

int DisplayClient::stop()
{
    hotplug(0);

    int ret;
    uint64_t m = 1;
    force_exit = true;
    ret = write(exit_fd, &m, sizeof(m));
    if (ret != sizeof(m))
        LOGE("failed to write exit mesg - %s\n", strerror(errno));
    return 0;
}


int DisplayClient::term()
{
    work_tid->join();
    work_tid.reset();
    close(exit_fd);

    if (client_sock != -1) {
        shutdown(client_sock, SHUT_RDWR);
        close(client_sock);
        client_sock = -1;
    }

    return 0;
}


int DisplayClient::connect()
{
    int ret, len;
    struct sockaddr_un server_sockaddr;

    memset(&server_sockaddr, 0, sizeof(struct sockaddr_un));
    server_sockaddr.sun_family = AF_UNIX;
    strcpy(server_sockaddr.sun_path, SERVER_SOCK_PATH);

    len = sizeof(server_sockaddr);
    ret = ::connect(client_sock, (struct sockaddr *) &server_sockaddr, len);
    if(ret == -1){
        LOGE("CONNECT ERROR = %s\n", strerror(errno));
    } else {
        LOGI("CONNECT OK! ret = %d\n", ret);
    }

    return ret;
}

static inline int _send(int fd, void* dt, int len)
{
    char * data= (char *)dt;
    int ret, sent_bytes = 0;

    do {
        ret = send(fd, data + sent_bytes, len - sent_bytes, 0);
        if (ret <= 0) {
            if (errno != EAGAIN)
                LOGE("_send fail (%d vs. %d) %s!", ret, len - sent_bytes, strerror(errno));
            continue;
        }

        sent_bytes += ret;
        if (sent_bytes == len) {
            break;
        }
    } while (errno == EAGAIN);
    return ret;
}

static inline int _recv(int fd, void* dt, int len)
{
    char * data= (char *)dt;
    int ret, got_bytes = 0;

    do {
        ret = recv(fd, data + got_bytes, len - got_bytes, 0);
        if (ret <= 0) {
            if (errno != EAGAIN)
                LOGE("_recv fail (%d vs. %d) %s!", ret, len - got_bytes, strerror(errno));
            continue;
        }

        got_bytes += ret;
        if (got_bytes == len) {
            break;
        }
    } while (errno == EAGAIN);
    return ret;
}

int DisplayClient::hotplug(int in)
{
    int ret;
    struct dpy_evt_header evt_hdr;
    std::unique_lock<mutex> lk(sock_mtx);

    if (client_sock == -1) {
        LOGE("%s() invalid client socket", __func__);
        return -1;
    }

    evt_hdr.e_type = DPY_EVENT_HOTPLUG;
    evt_hdr.e_magic = DISPLAY_MAGIC_CODE;
    evt_hdr.e_size = sizeof(in);
    ret = _send(client_sock, &evt_hdr, sizeof(evt_hdr));
    if (ret != sizeof(evt_hdr)) {
        LOGE("%s() send header fail(%d vs. 0x%lx) %s", __func__, ret, (unsigned long)sizeof(evt_hdr), strerror(errno));
        return -1;
    }

    ret = _send(client_sock, &in, sizeof(in));
    if (ret != sizeof(in)) {
        LOGE("%s() send body fail(%d vs. 0x%lx) %s", __func__, ret, (unsigned long)sizeof(in), strerror(errno));
        return -1;
    }
    return 0;
}

void * DisplayClient::work_thread(DisplayClient *cur_ctx)
{
    bool is_connected = false;
    int ret;
    struct dpy_evt_header msg_header;
    char buf[256];

    int epollfd = epoll_create1 (0);
    if (epollfd == -1) {
        LOGE ("epoll_create1");
        return NULL;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.fd = cur_ctx->client_sock;
    if (epoll_ctl (epollfd, EPOLL_CTL_ADD, cur_ctx->client_sock, &event) == -1) {
        LOGE ("epoll_ctl");
        return NULL;
    }

    event.events = EPOLLIN;
    event.data.fd = cur_ctx->exit_fd;
    if (epoll_ctl (epollfd, EPOLL_CTL_ADD, cur_ctx->exit_fd, &event) == -1) {
        LOGE ("epoll_ctl2");
        return NULL;
    }

    if (cur_ctx->renderer)
        cur_ctx->renderer->makeCurrent();

     while (!cur_ctx->force_exit) {
        if (!is_connected) {
            if (cur_ctx->connect() == 0) {
                is_connected = true;
                cur_ctx->hotplug(1);
            } else {
                usleep(500000);
                continue;
            }
         }

        // Buffer to hold events
        struct epoll_event events[5];
        int numEvents = epoll_wait (epollfd, events, 5, -1);
        if (numEvents == -1) {
            LOGE ("epoll_wait");
            continue;
        }

        // Process events
        for (int i = 0; (i < numEvents) && !cur_ctx->force_exit; i++) {
            // Check if the event is for the server socket
            if (events[i].data.fd != cur_ctx->client_sock) {
                if (events[i].data.fd == cur_ctx->exit_fd) {
                    LOGI("%s() -exit!\n", __func__);
                    break;
                }

	            LOGE("%s() -client socket fd wrong!\n", __func__);
                continue;
            }

            if (!(events[i].events & EPOLLIN)) {
                LOGE("poll client error: 0x%x", events[i].events);
                continue;
            }

            do {
                std::unique_lock<mutex> lk(cur_ctx->sock_mtx);

                ret = _recv(cur_ctx->client_sock, &msg_header, sizeof(msg_header));
                if (ret != sizeof(msg_header)) {
                    LOGE("recv event header fail (%d vs. 0x%lx) %s!", ret, (unsigned long)sizeof(msg_header), strerror(errno));
                    break;
                }

                if (msg_header.e_magic != DISPLAY_MAGIC_CODE) {
                    // data error, clear receive buffer
                    LOGE("recv data err!");
                    break;
                }

                if (msg_header.e_size > 0) {
                    ret = _recv(cur_ctx->client_sock, &buf, msg_header.e_size);
                    if (ret != msg_header.e_size) {
                        LOGE("recv event body fail (%d vs. %d) %s!", ret, msg_header.e_size, strerror(errno));
                        break;
                    }
                }

//                LOGI("got event type: 0x%x", msg_header.e_type);
                switch (msg_header.e_type) {
                    case DPY_EVENT_SURFACE_SET:
                    {
                        struct surface * surf = (struct surface *)buf;
                        ret = recv_fd(cur_ctx->client_sock, &surf->dma_info.dmabuf_fd);
                        if (ret < 0) {
                            LOGE("recv_fd failed! (ret=%d)", ret);
                            break;
                        }
                        lk.unlock();

                        if (cur_ctx->renderer)
                            cur_ctx->renderer->vdpy_surface_set(surf);
                        break;
                    }
                    case DPY_EVENT_SURFACE_UPDATE:
                    {
                        lk.unlock();

                        if (cur_ctx->renderer)
                            cur_ctx->renderer->vdpy_surface_update();
                        break;
                    }
                    case DPY_EVENT_SET_MODIFIER:
                    {
                        lk.unlock();

                        if (cur_ctx->renderer)
                            cur_ctx->renderer->vdpy_set_modifier(*(uint64_t *)buf);
                        break;
                    }
                    // case DPY_EVENT_DISPLAY_INFO:
                    // {
                    //     lk.unlock();
                    //     struct display_info *info = (struct display_info *)buf;
                    //     vscr->info.xoff = info->xoff;
                    //     vscr->info.yoff = info->yoff;
                    //     vscr->info.width = info->width;
                    //     vscr->info.height = info->height;
                    //     break;
                    // }
                    default:
                        lk.unlock();
                        break;
                }
            } while(0);
        }
    }

    LOGE ("%s() exit!", __func__);
    return NULL;
}

int DisplayClient::recv_fd(int sock_fd, int *fd)
{
    ssize_t ret;
    struct msghdr msg = {};
    int rdata[4] = {0};
    struct iovec vec;
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmptr;

    vec.iov_base = rdata;
    vec.iov_len = 16;
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    do {
        ret = ::recvmsg(sock_fd, &msg, MSG_WAITALL);
    } while ((ret < 0) && (errno == EAGAIN));

    if (ret < 0) {
        LOGE("recvmsg() ret < 0 Error: %s\n", strerror(errno));
        return -1;
    }

    cmptr = CMSG_FIRSTHDR(&msg);
    if (cmptr == NULL) {
        LOGE("recvmsg() no cmsg hdr\n");
        return -1;
    }

    if ((cmptr->cmsg_len != CMSG_LEN(sizeof(int)))
        || (cmptr->cmsg_level != SOL_SOCKET)
        || (cmptr->cmsg_type != SCM_RIGHTS)) {
        LOGE("recvmsg() cmsg error\n");
        return -1;
    }

    *fd = *((int*)CMSG_DATA(cmptr));
    return 0;
}
