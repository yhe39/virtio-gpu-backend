#ifndef CLIENT_RENDERER_H
#define CLIENT_RENDERER_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "vdisplay.h"

class Renderer {
public:
    Renderer();
    
    struct egl_display_ops {
        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    };
    
    struct egl_ctx {
        //struct android_app* app;
        NativeWindowType window;
        int32_t width;
        int32_t height;
        
	    bool egl_dmabuf_supported;

        EGLContext eglContext;
        EGLDisplay eglDisplay;
        EGLSurface eglSurface;

        uint64_t modifier;
        struct surface cur_surf;
	    EGLImage egl_img;
        GLuint surf_tex;

        // Handle to a program object
        GLuint programObject;
        GLuint programObjectExternal;
    };
    
    struct egl_display_ops gl_ops;
    struct egl_ctx gl_ctx;

    int init(NativeWindowType window);
    void terminate();
    int makeCurrent();

    void draw();
    void vdpy_surface_set(struct surface *surf);
    void vdpy_surface_update();
    void vdpy_set_modifier(uint64_t modifier);
private:
    typedef struct{
        short x, y;
        short w, h;
    } SDL_Rect;

    int egl_render_copy(GLuint src_tex,
				   const SDL_Rect * dstrect  __attribute__((unused)), bool is_dmabuf);
    int egl_create_dma_tex(GLuint *texid);

    GLuint esLoadShader ( GLenum type, const char *shaderSrc );
    GLuint esLoadProgram ( const char *vertShaderSrc, const char *fragShaderSrc );
    bool initialized;
};

#endif // CLIENT_RENDERER_H
