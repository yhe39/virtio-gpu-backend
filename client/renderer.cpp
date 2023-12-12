extern "C"
{
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
}

#include "renderer.h"

#include "common.h"

#define checkGlError(op) { \
	LOGE("%s():%d   CALL %s()\n", __func__, __LINE__, op); \
	for (GLint error = glGetError(); error; error = glGetError()) { \
		LOGE("%s():%d   glError (0x%x) for %s()\n", __func__, __LINE__, error, op); \
	} \
}
#define checkGlError2(op, arg) { \
	LOGE("%s():%d   CALL %s() 0x%x\n", __func__, __LINE__, op, arg); \
	for (GLint error = glGetError(); error; error = glGetError()) { \
		LOGE("%s():%d   glError (0x%x) for %s()\n", __func__, __LINE__, error, op); \
	} \
}
#define checkEglError(op) { \
	LOGE("%s():%d   CALL %s()\n", __func__, __LINE__, op); \
	for (GLint error = eglGetError(); error!=EGL_SUCCESS; error = eglGetError()) { \
		LOGE("%s():%d   eglError (0x%x) for %s()\n", __func__, __LINE__, error, op); \
	} \
}

#define VDPY_MIN_WIDTH 640
#define VDPY_MIN_HEIGHT 480

Renderer::Renderer() : gl_ops(), gl_ctx(), initialized(false)
{
	gl_ops.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
				eglGetProcAddress("eglCreateImageKHR");
	gl_ops.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
				eglGetProcAddress("eglDestroyImageKHR");
	gl_ops.glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
				eglGetProcAddress("glEGLImageTargetTexture2DOES");

	if ((gl_ops.eglCreateImageKHR == NULL) ||
		(gl_ops.eglDestroyImageKHR == NULL) ||
		(gl_ops.glEGLImageTargetTexture2DOES == NULL)) {
		LOGI("DMABuf is not supported.\n");
		gl_ctx.egl_dmabuf_supported = false;
	} else
		gl_ctx.egl_dmabuf_supported = true;
}

int Renderer::init(NativeWindowType window)
{
	EGLint majorVersion;
	EGLint minorVersion;
	EGLint numConfigs = 0, n = -1;
	EGLConfig myConfig;
	int w, h;

	LOGD("%s()", __func__);
	// only support 1 physical screen now

	// create egl surface from native window
	gl_ctx.eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (eglInitialize(gl_ctx.eglDisplay, &majorVersion, &minorVersion) != EGL_TRUE) {
		checkEglError("eglInitialize");
		LOGE("%s, eglInitialize failed.", __func__);
		return -1;
	}

	EGLint s_configAttribs[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_NONE };
	eglChooseConfig(gl_ctx.eglDisplay, s_configAttribs, 0, 0, &numConfigs);
	checkEglError("eglChooseConfig0");
	if (numConfigs <= 0) {
		LOGE("%s, eglChooseConfig failed.", __func__);
		return -1;
	}

	EGLConfig* const configs = (EGLConfig*) malloc(sizeof(EGLConfig) * numConfigs);
	eglChooseConfig(gl_ctx.eglDisplay, s_configAttribs, configs, numConfigs, &n);
	checkEglError("eglChooseConfig1");
	myConfig = configs[0];
	free(configs);

	// gl_ctx.eglContext = SDL_GL_GetCurrentContext();
	EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	gl_ctx.eglContext = eglCreateContext(gl_ctx.eglDisplay, myConfig, EGL_NO_CONTEXT, context_attribs);
	checkEglError("eglCreateContext");

	gl_ctx.eglSurface = eglCreateWindowSurface(gl_ctx.eglDisplay, myConfig, window, NULL);
	checkEglError("eglCreateWindowSurface");
	if (gl_ctx.eglSurface == EGL_NO_SURFACE) {
		LOGE("gelCreateWindowSurface failed.\n");
		return -1;
	}

	EGLBoolean returnValue;
	returnValue = eglMakeCurrent(gl_ctx.eglDisplay, gl_ctx.eglSurface, gl_ctx.eglSurface, gl_ctx.eglContext);
	checkEglError("eglMakeCurrent");
	// checkEglError("eglMakeCurrent", returnValue);
	if (returnValue != EGL_TRUE) {
		LOGE("eglMakeCurrent failed.\n");
		return -1;
	}
	eglQuerySurface(gl_ctx.eglDisplay, gl_ctx.eglSurface, EGL_WIDTH, &w);
	checkEglError("eglQuerySurface0");
	eglQuerySurface(gl_ctx.eglDisplay, gl_ctx.eglSurface, EGL_HEIGHT, &h);
	checkEglError("eglQuerySurface1");
//	w = 0x780;
//	h = 0x438;
	gl_ctx.width = w;
	gl_ctx.height = h;
	LOGI("%s (gl_ctx.eglDisplay/gl_ctx.eglSurface)=0x%lx/0x%lx w/h=%d/%d\n",
	  __func__, (unsigned long)gl_ctx.eglDisplay, (unsigned long)gl_ctx.eglSurface, w, h);
	// static_cast<int>(reinterpret_cast<intptr_t>(ptr))

	if (gl_ctx.width < VDPY_MIN_WIDTH ||
			gl_ctx.width < VDPY_MIN_HEIGHT) {
		LOGE("Too small resolutions. Please check the "
				" graphics system\n");
		// SDL_Quit();
		return -1;
	}

	char vShaderStr[] =
			"#version 300 es							\n"
			"layout(location = 0) in vec4 a_position;   \n"
			"layout(location = 1) in vec2 a_texCoord;   \n"
			"out vec2 v_texCoord;					   \n"
			"void main()								\n"
			"{										  \n"
			"   gl_Position = a_position;			   \n"
			"   v_texCoord = a_texCoord;				\n"
			"}										  \n";

	char fShaderStr[] =
			"#version 300 es									 \n"
			"#extension GL_OES_EGL_image_external : require	  \n"
			"precision mediump float;							\n"
			"layout(location = 0) out vec4 outColor;			 \n"
			"in vec2 v_texCoord;								 \n"
			"uniform samplerExternalOES uTexture;				\n"
			"void main()										 \n"
			"{												   \n"
			"  outColor = texture2D(uTexture, v_texCoord);   \n"
			"}												   \n";
	gl_ctx.programObjectExternal = esLoadProgram(vShaderStr, fShaderStr);
	if (!gl_ctx.programObjectExternal)
		LOGE("%s failed to load programObjectExternal\n", __func__);

	char fShaderStr2[] =
			"#version 300 es									 \n"
			"precision mediump float;							\n"
			"layout(location = 0) out vec4 outColor;			 \n"
			"in vec2 v_texCoord;								 \n"
			"uniform sampler2D uTexture;				\n"
			"void main()										 \n"
			"{												   \n"
			"  outColor = texture2D(uTexture, v_texCoord);   \n"
			"}												   \n";
	gl_ctx.programObject = esLoadProgram(vShaderStr, fShaderStr2);
	if (!gl_ctx.programObject)
		LOGE("%s failed to load programObject\n", __func__);

	returnValue = eglMakeCurrent(gl_ctx.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	checkEglError("eglMakeCurrent");

	initialized = true;
	return 0;
}

void Renderer::terminate()
{
	initialized = false;

	if (gl_ctx.cur_surf.dma_info.dmabuf_fd != 0) {
		close(gl_ctx.cur_surf.dma_info.dmabuf_fd);
		gl_ctx.cur_surf.dma_info.dmabuf_fd = 0;
	}

	// Delete program object
	if (gl_ctx.programObjectExternal) {
		glDeleteProgram(gl_ctx.programObjectExternal);
		checkGlError("glDeleteProgram1");
	}
	if (gl_ctx.programObject) {
		glDeleteProgram(gl_ctx.programObject);
		checkGlError("glDeleteProgram2");
	}

	if (gl_ctx.eglDisplay != EGL_NO_DISPLAY) {
		eglMakeCurrent(gl_ctx.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		checkEglError("eglMakeCurrent");
		if (gl_ctx.eglContext != EGL_NO_CONTEXT) {
			eglDestroyContext(gl_ctx.eglDisplay, gl_ctx.eglContext);
			checkEglError("eglDestroyContext");
		}
		if (gl_ctx.eglSurface != EGL_NO_SURFACE) {
			eglDestroySurface(gl_ctx.eglDisplay, gl_ctx.eglSurface);
			checkEglError("eglDestroySurface");
		}
		eglTerminate(gl_ctx.eglDisplay);
		checkEglError("eglTerminate");
	}
	gl_ctx.eglDisplay = EGL_NO_DISPLAY;
	gl_ctx.eglContext = EGL_NO_CONTEXT;
	gl_ctx.eglSurface = EGL_NO_SURFACE;
}

int Renderer::makeCurrent()
{
	EGLBoolean returnValue;

	LOGI("%s\n", __func__);
	while (!initialized) {
		usleep(500000);
	}

	returnValue = eglMakeCurrent(gl_ctx.eglDisplay, gl_ctx.eglSurface, gl_ctx.eglSurface, gl_ctx.eglContext);
	checkEglError("eglMakeCurrent");
	// checkEglError("eglMakeCurrent", returnValue);
	if (returnValue != EGL_TRUE) {
		LOGE("eglMakeCurrent failed.\n");
		return -1;
	}
	return 0;
}

void Renderer::draw()
{
}

void Renderer::vdpy_surface_set(struct surface *surf)
{
	// int format;
	int i;
	char tmp[256]={};

	// if (vdpy.tid != pthread_self()) {
	// 	LOGE("%s: unexpected code path as unsafe 3D ops in multi-threads env.\n",
	// 		__func__);
	// 	return;
	// }

	LOGI("%s -1\n", __func__);

	if (surf->surf_type == SURFACE_DMABUF) {
		if (gl_ctx.cur_surf.dma_info.dmabuf_fd != 0)
			close(gl_ctx.cur_surf.dma_info.dmabuf_fd);
		gl_ctx.cur_surf = *surf;
	} else {
		/* Unsupported type */
		return;
	}

	if (!initialized)
		return;

	if (gl_ctx.surf_tex) {
		// SDL_DestroyTexture(gl_ctx.surf_tex);
		glDeleteTextures(1, &gl_ctx.surf_tex);
		checkGlError2("glDeleteTextures", gl_ctx.surf_tex);
	}
	if (surf && (surf->surf_type == SURFACE_DMABUF)) {
		egl_create_dma_tex(&gl_ctx.surf_tex);
	}

	/* For the surf_switch, it will be updated in surface_update */
	if (surf->surf_type == SURFACE_DMABUF) {
		EGLImageKHR egl_img = EGL_NO_IMAGE_KHR;
		EGLint attrs[64];
		
		i = 0;
		attrs[i++] = EGL_WIDTH;
		attrs[i++] = surf->width;
		attrs[i++] = EGL_HEIGHT;
		attrs[i++] = surf->height;
		attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
		attrs[i++] = surf->dma_info.surf_fourcc;
		attrs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
		attrs[i++] = surf->dma_info.dmabuf_fd;
		attrs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
		attrs[i++] = surf->stride;
		attrs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
		attrs[i++] = surf->dma_info.dmabuf_offset;
		LOGI("--yue, EGL_WIDTH is %x, surf->width is %x, EGL_HEIGHT is %x, surf->height is %x\n", EGL_WIDTH, surf->width, EGL_HEIGHT, surf->height);
		LOGI("--yue EGL_LINUX_DRM_FOURCC_EXT is %x, dma_info.surf_fourcc is %x, EGL_DMA_BUF_PLANE0_FD_EXT is %x, dma_info.dmabuf_fd is %x\n", EGL_LINUX_DRM_FOURCC_EXT, surf->dma_info.surf_fourcc, EGL_DMA_BUF_PLANE0_FD_EXT, surf->dma_info.dmabuf_fd);
		LOGI("--yue EGL_DMA_BUF_PLANE0_PITCH_EXT is %x, surf->stride is %x, EGL_DMA_BUF_PLANE0_OFFSET_EXT is %x, surf->dma_info.dmabuf_offset is %x\n", EGL_DMA_BUF_PLANE0_PITCH_EXT, surf->stride, EGL_DMA_BUF_PLANE0_OFFSET_EXT, surf->dma_info.dmabuf_offset);
		if (gl_ctx.modifier) {
			LOGI("--yue, has modifier\n");
			attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
			attrs[i++] = gl_ctx.modifier & 0xffffffff;
			attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
			attrs[i++] = (gl_ctx.modifier & 0xffffffff00000000) >> 32;
		}
		attrs[i++] = EGL_NONE;

		for(i=0; i<17; i++)
			snprintf(tmp, 255, "%s 0x%x", tmp, attrs[i]);
		LOGI("eglCreateImageKHR attrs=(%s)\n", tmp);

		egl_img = gl_ops.eglCreateImageKHR(gl_ctx.eglDisplay,
				EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT,
				NULL, attrs);
		checkEglError("eglCreateImageKHR");
		if (egl_img == EGL_NO_IMAGE_KHR) {
			LOGE("Failed in eglCreateImageKHR.\n");
			return;
		}

		// SDL_GL_BindTexture(gl_ctx.surf_tex, NULL, NULL);
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl_ctx.surf_tex);
		checkGlError2("glBindTexture", gl_ctx.surf_tex);
		gl_ops.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, egl_img);
		checkGlError("glEGLImageTargetTexture2DOES");
		if (gl_ctx.egl_img != EGL_NO_IMAGE_KHR)
			gl_ops.eglDestroyImageKHR(gl_ctx.eglDisplay,
					gl_ctx.egl_img);

		/* In theory the created egl_img can be released after it is bound
		 * to texture.
		 * Now it is released next time so that it is controlled correctly
		 */
		gl_ctx.egl_img = egl_img;
	}

	LOGI("%s -2\n", __func__);
}

void Renderer::vdpy_surface_update()
{
	if (!initialized)
		return;

	if (gl_ctx.surf_tex)
		egl_render_copy(gl_ctx.surf_tex, NULL, true);

	eglSwapBuffers(gl_ctx.eglDisplay, gl_ctx.eglSurface);
}

void Renderer::vdpy_set_modifier(uint64_t modifier)
{
	gl_ctx.modifier = modifier;
}

#define checkGlError(op) { \
	LOGE("%s():%d   CALL %s()\n", __func__, __LINE__, op); \
	for (GLint error = glGetError(); error; error = glGetError()) { \
		LOGE("%s():%d   glError (0x%x) for %s()\n", __func__, __LINE__, error, op); \
	} \
}
#define checkGlError2(op, arg) { \
	LOGE("%s():%d   CALL %s() 0x%x\n", __func__, __LINE__, op, arg); \
	for (GLint error = glGetError(); error; error = glGetError()) { \
		LOGE("%s():%d   glError (0x%x) for %s()\n", __func__, __LINE__, error, op); \
	} \
}
#define checkEglError(op) { \
	LOGE("%s():%d   CALL %s()\n", __func__, __LINE__, op); \
	for (GLint error = eglGetError(); error!=EGL_SUCCESS; error = eglGetError()) { \
		LOGE("%s():%d   eglError (0x%x) for %s()\n", __func__, __LINE__, error, op); \
	} \
}

int Renderer::egl_render_copy(GLuint src_tex,
				   const SDL_Rect * dstrect  __attribute__((unused)), bool is_dmabuf)
{
	GLfloat vVertices[] = {-1.0f, 1.0f, 0.0f,  // Position 0
						   0.0f, 0.0f,		// TexCoord 0
						   -1.0f, -1.0f, 0.0f,  // Position 1
						   0.0f, 1.0f,		// TexCoord 1
						   1.0f, -1.0f, 0.0f,  // Position 2
						   1.0f, 1.0f,		// TexCoord 2
						   1.0f, 1.0f, 0.0f,  // Position 3
						   1.0f, 0.0f		 // TexCoord 3
	};
	GLushort indices[] = {0, 1, 2, 0, 2, 3};

	if (dstrect) {
		vVertices[0] = dstrect->x;
		vVertices[1] = dstrect->y;
		vVertices[5] = dstrect->x + dstrect->w;
		vVertices[6] = dstrect->y;
		vVertices[10] = dstrect->x + dstrect->w;
		vVertices[11] = dstrect->y + dstrect->h;
		vVertices[15] = dstrect->x;
		vVertices[16] = dstrect->y + dstrect->h;
		LOGI("%s dstrect={%d, %d, %d, %d}\n",
				 __func__, dstrect->x, dstrect->y, dstrect->w, dstrect->h);
	} else {
		LOGI("%s dstrect=NULL\n", __func__);
	}

	// Set the viewport
	glViewport(0, 0, gl_ctx.width, gl_ctx.height);
	checkGlError("glViewport");

	// Clear the color buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGlError("glClear");

	// Use the program object
	GLuint program;
	if (is_dmabuf)
		program = gl_ctx.programObjectExternal;
	else
		program = gl_ctx.programObject;
	glLinkProgram(program);
	glUseProgram(program);
	checkGlError("glUseProgram");

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	// Load the vertex position
	glVertexAttribPointer(0, 3, GL_FLOAT,
						  GL_FALSE, 5 * sizeof(GLfloat), vVertices);
	checkGlError("glVertexAttribPointer0");
	// Load the texture coordinate
	glVertexAttribPointer(1, 2, GL_FLOAT,
						  GL_FALSE, 5 * sizeof(GLfloat), &vVertices[3]);
	checkGlError("glVertexAttribPointer1");

	glEnableVertexAttribArray(0);
	checkGlError("glEnableVertexAttribArray0");
	glEnableVertexAttribArray(1);
	checkGlError("glEnableVertexAttribArray1");

	// Bind the base map
	glActiveTexture(GL_TEXTURE0);
	checkGlError("glActiveTexture");
	if (is_dmabuf)
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, src_tex);
	else
		glBindTexture(GL_TEXTURE_2D, src_tex);
	checkGlError2("glBindTexture", src_tex);

	// Set the base map sampler to texture unit to 0
	// glUniform1i(userData->baseMapLoc, 0);
	GLuint uniformlocation = glGetUniformLocation(program, "uTexture");
	checkGlError("glGetUniformLocation");
	glUniform1i(uniformlocation, 0);
	checkGlError("glUniform1i");

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
	checkGlError("glDrawElements");
	return 0;
}

int Renderer::egl_create_dma_tex(GLuint *texid)
{
	LOGI("%s -1\n", __func__);
	glGenTextures(1, texid);
	checkGlError2("glGenTextures", *texid);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, *texid);
	checkGlError2("glBindTexture", *texid);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	checkGlError("glTexParameteri");
	LOGI("%s -2\n", __func__);
	return 0;
}

GLuint Renderer::esLoadShader ( GLenum type, const char *shaderSrc )
{
	GLuint shader;
	GLint compiled;

	// Create the shader object
	shader = glCreateShader ( type );
	checkGlError("glCreateShader");
	if ( shader == 0 )
	{
		LOGE("%s() failed to create shader!\n", __func__);
		return 0;
	}

	// Load the shader source
	glShaderSource ( shader, 1, &shaderSrc, NULL );
	checkGlError("glShaderSource");

	// Compile the shader
	glCompileShader ( shader );
	checkGlError("glCompileShader");

	// Check the compile status
	glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );
	checkGlError("glGetShaderiv");
	if ( !compiled )
	{
		GLint infoLen = 0;

		glGetShaderiv ( shader, GL_INFO_LOG_LENGTH, &infoLen );
		checkGlError("glGetShaderiv2");
		if ( infoLen > 1 )
		{
			char *infoLog = (char *) malloc ( sizeof ( char ) * infoLen );

			glGetShaderInfoLog ( shader, infoLen, NULL, infoLog );
			LOGE ( "Error compiling shader:\n%s\n", infoLog );

			free ( infoLog );
		}

		glDeleteShader ( shader );
		checkGlError("glDeleteShader");
		LOGE("%s() failed to compile shader!\n", __func__);
		return 0;
	}

	return shader;

}

GLuint Renderer::esLoadProgram ( const char *vertShaderSrc, const char *fragShaderSrc )
{
	GLuint vertexShader;
	GLuint fragmentShader;
	GLuint programObject;
	GLint linked;

	// Load the vertex/fragment shaders
	vertexShader = esLoadShader ( GL_VERTEX_SHADER, vertShaderSrc );
	if ( vertexShader == 0 )
	{
		LOGE("%s() failed to load vertex shader!\n", __func__);
		return 0;
	}

	fragmentShader = esLoadShader ( GL_FRAGMENT_SHADER, fragShaderSrc );
	if ( fragmentShader == 0 )
	{
		glDeleteShader ( vertexShader );
		checkGlError("glDeleteShader");
		LOGE("%s() failed to load fragment shader!\n", __func__);
		return 0;
	}

	// Create the program object
	programObject = glCreateProgram ( );
	checkGlError("glCreateProgram");
	if ( programObject == 0 )
	{
		LOGE("%s() failed to create program!\n", __func__);
		return 0;
	}

	glAttachShader ( programObject, vertexShader );
	checkGlError("glAttachShader");
	glAttachShader ( programObject, fragmentShader );
	checkGlError("glAttachShader2");

	// Link the program
	glLinkProgram ( programObject );
	checkGlError("glLinkProgram");

	// Check the link status
	glGetProgramiv ( programObject, GL_LINK_STATUS, &linked );
	checkGlError("glGetProgramiv");

	if ( !linked )
	{
		GLint infoLen = 0;

		glGetProgramiv ( programObject, GL_INFO_LOG_LENGTH, &infoLen );
		checkGlError("glGetProgramiv2");

		if ( infoLen > 1 )
		{
			char *infoLog = (char *) malloc ( sizeof ( char ) * infoLen );

			glGetProgramInfoLog ( programObject, infoLen, NULL, infoLog );
			checkGlError("glGetProgramInfoLog");
			LOGE ( "Error linking program:\n%s\n", infoLog );

			free ( infoLog );
		}

		glDeleteProgram ( programObject );
		checkGlError("glDeleteProgram");
		LOGE("%s() failed to link program!\n", __func__);
		return 0;
	}

	// Free up no longer needed shader resources
	glDeleteShader ( vertexShader );
	checkGlError("glDeleteShader");
	glDeleteShader ( fragmentShader );
	checkGlError("glDeleteShader2");

	return programObject;
}
