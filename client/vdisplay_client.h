#ifndef VDISPLAY_CLIENT_H
#define VDISPLAY_CLIENT_H

#include "renderer.h"

#include <mutex>
#include <thread>

using namespace std;

class DisplayClient {
public:
    DisplayClient(Renderer * rd);
    int start();
    int stop();
    int term();

    int connect();
    int hotplug(int in);

private:

    static int recv_fd(int sock_fd, int *fd);
    static void * work_thread(DisplayClient *cur_ctx);
    int client_sock;
    std::mutex sock_mtx;

    bool force_exit;
    int exit_fd;
    shared_ptr<thread> work_tid;

    Renderer *renderer;
};

#endif // VDISPLAY_CLIENT_H
