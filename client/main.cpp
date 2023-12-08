/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

// #include "agq.h"
// #include "circle.h"
// #include "common.h"
#include "renderer.h"
#include "vdisplay_client.h"
// #include "vecmath.h"

#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <initializer_list>
#include <memory>
#include <sys/time.h>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <android/sensor.h>
#include <android_native_app_glue.h>

#include "common.h"

#define USE_GAME_RENDER

#ifndef USE_GAME_RENDER
static void init_vdpy(struct virtio_backend_info *info) {
    vdpy_gfx_ui_init(info->native_window);
}

extern struct pci_vdev_ops pci_ops_virtio_gpu;
static struct virtio_backend_info virtio_gpu_info = {
	.pci_vdev_ops = &pci_ops_virtio_gpu,
	.hook_before_init = init_vdpy,
};
#endif

namespace {
int animating = 0;

/**
 * Process the next input event.
 */
int32_t engine_handle_input(struct android_app*, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        animating = 1;
        return 1;
    }
    return 0;
}

/**
 * Process the next main command.
 */
void engine_handle_cmd(struct android_app* app, int32_t cmd) {    
    Renderer *renderer = (Renderer *)(app->userData);
    switch (cmd) {
        case APP_CMD_SAVE_STATE:
            // We are not saving the state.
            LOGI("APP_CMD_SAVE_STATE");
            break;
        case APP_CMD_INIT_WINDOW:
            // The window is being shown, get it ready.
            LOGI("APP_CMD_INIT_WINDOW");
            if (app->window != NULL) {
                LOGI("APP_CMD_INIT_WINDOW -1");
#ifdef USE_GAME_RENDER
                renderer->init(app->window);
                renderer->draw();
#else
                virtio_gpu_info.native_window = app->window;
                create_backend_thread(&virtio_gpu_info);
#endif

                animating = 1;
            }
            LOGI("APP_CMD_INIT_WINDOW -2");
            break;
        case APP_CMD_TERM_WINDOW:
            // The window is being hidden or closed, clean it up.
#ifdef USE_GAME_RENDER
            renderer->terminate();
#else
            close_backend_thread();
#endif
            LOGI("APP_CMD_TERM_WINDOW");
            animating = 0;
            break;
        case APP_CMD_LOST_FOCUS:
            // Also stop animating.
            LOGI("APP_CMD_LOST_FOCUS");
            animating = 0;
#ifdef USE_GAME_RENDER
            renderer->draw();
#endif
            break;
        default:
            LOGI("APP_CMD %d", cmd);
            break;
    }
}

} // end of anonymous namespace

/**
 * This is the main entry point of a native application that is using
 * android_native_app_glue.  It runs in its own thread, with its own
 * event loop for receiving input events and doing other things.
 */
void android_main(struct android_app* state) {
    std::srand(0);

    LOGI("Running with SDK %d", state->activity->sdkVersion);

    std::unique_ptr<Renderer> renderer(new Renderer());
    std::unique_ptr<DisplayClient> display_client(new DisplayClient(renderer.get()));
    display_client->start();

    state->userData = renderer.get();
    state->onAppCmd = engine_handle_cmd;
    state->onInputEvent = engine_handle_input;

    // loop waiting for stuff to do.
    while (1) {
        // Read all pending events.
        int events;
        struct android_poll_source* source;

        // If not animating, we will block forever waiting for events.
        // If animating, we loop until all events are read, then continue
        // to draw the next frame of animation.
        while (ALooper_pollAll(0, NULL, &events, (void**)&source) >= 0) {
            LOGI("ALooper_pollAll -1");
            // Process this event.
            if (source != NULL) {
                source->process(state, source);
            }

            LOGI("ALooper_pollAll -2");
            // Check if we are exiting.
            if (state->destroyRequested != 0) {
#ifdef USE_GAME_RENDER
                renderer->terminate();
#else
                close_backend_thread();
#endif
                LOGI("state->destroyRequested != 0, exit...");
                return;
            }
            LOGI("ALooper_pollAll -3");
        }

#ifdef USE_GAME_RENDER
        if (animating) {
            // renderer->update();

            // Drawing is throttled to the screen update rate, so there
            // is no need to do timing here.
            renderer->draw();

            // Broadcast intent every 5 seconds.
            // static auto last_timestamp = std::chrono::steady_clock::now();
            // auto now = std::chrono::steady_clock::now();
            // if (now - last_timestamp >= std::chrono::seconds(5)) {
            //     last_timestamp = now;
            //     android::GameQualification qualification;
            //     qualification.startLoop(state->activity);
            // }
        }
#else
    #ifndef VDPY_SEPERATE_THREAD
        if (virtio_gpu_info.vdev_inited) {
            vdpy_sdl_display_proc(virtio_gpu_info.vdev_termed);
        }
    #endif
#endif
    }
}
