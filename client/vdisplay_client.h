#ifndef VDISPLAY_CLIENT_H
#define VDISPLAY_CLIENT_H

#include "renderer.h"

#include <thread>

using namespace std;

class DisplayClient {
public:
    DisplayClient(Renderer * rd);
    int start();
    int term();

    int connect();

private:

    static void * work_thread(DisplayClient *cur_ctx);
    int client_sock;
    bool is_connected;
	shared_ptr<thread> server_tid;

    Renderer *renderer;
};

#endif // VDISPLAY_CLIENT_H
