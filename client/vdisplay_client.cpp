#include "vdisplay_client.h"

extern "C"
{
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vdisplay_protocol.h"
}

#include "common.h"

#define SERVER_SOCK_PATH  "/data/virt_disp_server"
#define CLIENT_SOCK_PATH  "/data/virt_disp_client"

DisplayClient::DisplayClient(Renderer * rd) : client_sock(-1), is_connected(false), renderer(rd)
{}

int DisplayClient::start()
{
    int ret, len;
    struct sockaddr_un client_sockaddr; 

    client_sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_sock == -1) {
        LOGE("SOCKET ERROR = %d\n", errno);
        return -1;
    }

    memset(&client_sockaddr, 0, sizeof(struct sockaddr_un));
    client_sockaddr.sun_family = AF_UNIX;   
    strcpy(client_sockaddr.sun_path, CLIENT_SOCK_PATH); 
    len = sizeof(client_sockaddr);
    
    unlink(CLIENT_SOCK_PATH);
    ret = ::bind(client_sock, (struct sockaddr *) &client_sockaddr, len);
    if (ret == -1){
        LOGE("BIND ERROR: %d\n", errno);
        close(client_sock);
        client_sock = -1;
        return -1;
    }

    server_tid = make_shared<thread>(work_thread, this);
    server_tid->join();
    return ret;
}

int DisplayClient::term()
{
    if (client_sock != -1) {
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
        LOGE("CONNECT ERROR = %d\n", errno);
    } else {
        is_connected = true;
        LOGI("CONNECT OK! ret = %d\n", ret);
    }

    return ret;
}

void * DisplayClient::work_thread(DisplayClient *cur_ctx)
{
    int ret;
    struct dpy_evt_header msg_header;
    char buf[256];

    int epollfd = epoll_create1 (0);
    if (epollfd == -1) {
        perror ("epoll_create1");
        return NULL;
    }

    // we have to register the server socket for “epoll” events
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.fd = cur_ctx->client_sock;
    if (epoll_ctl (epollfd, EPOLL_CTL_ADD, cur_ctx->client_sock, &event) == -1) {
        perror ("epoll_ctl");
        exit (EXIT_FAILURE);
    }

    while (true) {
        if (!cur_ctx->is_connected) {
            cur_ctx->connect();
            continue;
        }

        // Buffer to hold events
        struct epoll_event events[5];
        int numEvents = epoll_wait (epollfd, events, 5, -1);
        if (numEvents == -1) {
            perror ("epoll_wait");
            continue;
        }
        
        // Process events
        for (int i = 0; i < numEvents; i++) {
            // Check if the event is for the server socket
            if (events[i].data.fd != cur_ctx->client_sock) {
                continue;
            }

            if (!(events[i].events & EPOLLIN)) {
                LOGE("poll client error: 0x%x", events[i].events);
                continue;
            }
            
            while ((ret = ::recv(cur_ctx->client_sock, &msg_header, sizeof(msg_header), 0) ) > 0) {
                if (ret != sizeof(msg_header)) {
                    LOGE("recv event header fail (%d vs. 0x%lx)!", ret, (unsigned long)sizeof(msg_header));
                    continue;
                }
                
                if (msg_header.e_magic != DISPLAY_MAGIC_CODE) {
                    // data error, clear receive buffer
                    LOGE("recv data err!");
                    while (::recv(cur_ctx->client_sock, &buf, 256, 0) > 0);
                    break;
                }

                ret = ::recv(cur_ctx->client_sock, &buf, msg_header.e_size, 0);
                if (ret != msg_header.e_size)
                    LOGE("recv event body fail (%d vs. %d) !", ret, msg_header.e_size);

                switch (msg_header.e_type) {
                    case DPY_EVENT_SURFACE_SET:
                    {
                        if (cur_ctx->renderer)
                            cur_ctx->renderer->vdpy_surface_set((struct surface *)buf);
                        break;
                    }
                    case DPY_EVENT_SET_MODIFIER:
                    {
                        if (cur_ctx->renderer)
                            cur_ctx->renderer->vdpy_set_modifier(*(uint64_t *)buf);
                        break;
                    }
                    // case DPY_EVENT_DISPLAY_INFO:
                    // {
                    //     struct display_info *info = (struct display_info *)buf;
                    //     vscr->info.xoff = info->xoff;
                    //     vscr->info.yoff = info->yoff;
                    //     vscr->info.width = info->width;
                    //     vscr->info.height = info->height;
                    //     break;
                    // }
                    default:
                        break;
                }
            }
        }
    }
}
