/**************************************************************************
 * 
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/


/**
 * This is an EGL driver that wraps GLX. This gives the benefit of being
 * completely agnostic of the direct rendering implementation.
 *
 * Authors: Alan Hourihane <alanh@tungstengraphics.com>
 */

/*
 * TODO: 
 *
 * test eglBind/ReleaseTexImage
 */


#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "dlfcn.h"
#include <X11/Xlib.h>
#include <GL/gl.h>
#include "glxclient.h"

#include "eglconfig.h"
#include "eglcontext.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "eglglobals.h"
#include "egllog.h"
#include "eglsurface.h"

#include <GL/gl.h>

#define CALLOC_STRUCT(T)   (struct T *) calloc(1, sizeof(struct T))

static const EGLint all_apis = (EGL_OPENGL_ES_BIT
                                | EGL_OPENGL_ES2_BIT
                                | EGL_OPENVG_BIT
                                /* | EGL_OPENGL_BIT */); /* can't do */

struct visual_attribs
{
   /* X visual attribs */
   int id;
   int klass;
   int depth;
   int redMask, greenMask, blueMask;
   int colormapSize;
   int bitsPerRGB;

   /* GL visual attribs */
   int supportsGL;
   int transparentType;
   int transparentRedValue;
   int transparentGreenValue;
   int transparentBlueValue;
   int transparentAlphaValue;
   int transparentIndexValue;
   int bufferSize;
   int level;
   int render_type;
   int doubleBuffer;
   int stereo;
   int auxBuffers;
   int redSize, greenSize, blueSize, alphaSize;
   int depthSize;
   int stencilSize;
   int accumRedSize, accumGreenSize, accumBlueSize, accumAlphaSize;
   int numSamples, numMultisample;
   int visualCaveat;
};

/** subclass of _EGLDriver */
struct GLX_egl_driver
{
   _EGLDriver Base;   /**< base class */
};


/** driver data of _EGLDisplay */
struct GLX_egl_display
{
   Display *dpy;
   XVisualInfo *visuals;
   GLXFBConfig *fbconfigs;

   int glx_maj, glx_min;
};


/** subclass of _EGLContext */
struct GLX_egl_context
{
   _EGLContext Base;   /**< base class */

   GLXContext context;
};


/** subclass of _EGLSurface */
struct GLX_egl_surface
{
   _EGLSurface Base;   /**< base class */

   GLXDrawable drawable;
};


/** subclass of _EGLConfig */
struct GLX_egl_config
{
   _EGLConfig Base;   /**< base class */
   int index;
};

/** cast wrapper */
static struct GLX_egl_driver *
GLX_egl_driver(_EGLDriver *drv)
{
   return (struct GLX_egl_driver *) drv;
}

static struct GLX_egl_display *
GLX_egl_display(_EGLDisplay *dpy)
{
   return (struct GLX_egl_display *) dpy->DriverData;
}

static struct GLX_egl_context *
GLX_egl_context(_EGLContext *ctx)
{
   return (struct GLX_egl_context *) ctx;
}

static struct GLX_egl_surface *
GLX_egl_surface(_EGLSurface *surf)
{
   return (struct GLX_egl_surface *) surf;
}

static int
GLX_egl_config_index(_EGLConfig *conf)
{
   return ((struct GLX_egl_config *) conf)->index;
}

static GLboolean
get_visual_attribs(Display *dpy, XVisualInfo *vInfo,
                   struct visual_attribs *attribs)
{
   const char *ext = glXQueryExtensionsString(dpy, vInfo->screen);
   int rgba;

   memset(attribs, 0, sizeof(struct visual_attribs));

   attribs->id = vInfo->visualid;
#if defined(__cplusplus) || defined(c_plusplus)
   attribs->klass = vInfo->c_class;
#else
   attribs->klass = vInfo->class;
#endif
   attribs->depth = vInfo->depth;
   attribs->redMask = vInfo->red_mask;
   attribs->greenMask = vInfo->green_mask;
   attribs->blueMask = vInfo->blue_mask;
   attribs->colormapSize = vInfo->colormap_size;
   attribs->bitsPerRGB = vInfo->bits_per_rgb;

   if (glXGetConfig(dpy, vInfo, GLX_USE_GL, &attribs->supportsGL) != 0 ||
       !attribs->supportsGL)
      return GL_FALSE;
   glXGetConfig(dpy, vInfo, GLX_BUFFER_SIZE, &attribs->bufferSize);
   glXGetConfig(dpy, vInfo, GLX_LEVEL, &attribs->level);
   glXGetConfig(dpy, vInfo, GLX_RGBA, &rgba);
   if (!rgba)
      return GL_FALSE;
   attribs->render_type = GLX_RGBA_BIT;
   
   glXGetConfig(dpy, vInfo, GLX_DOUBLEBUFFER, &attribs->doubleBuffer);
   if (!attribs->doubleBuffer)
      return GL_FALSE;

   glXGetConfig(dpy, vInfo, GLX_STEREO, &attribs->stereo);
   glXGetConfig(dpy, vInfo, GLX_AUX_BUFFERS, &attribs->auxBuffers);
   glXGetConfig(dpy, vInfo, GLX_RED_SIZE, &attribs->redSize);
   glXGetConfig(dpy, vInfo, GLX_GREEN_SIZE, &attribs->greenSize);
   glXGetConfig(dpy, vInfo, GLX_BLUE_SIZE, &attribs->blueSize);
   glXGetConfig(dpy, vInfo, GLX_ALPHA_SIZE, &attribs->alphaSize);
   glXGetConfig(dpy, vInfo, GLX_DEPTH_SIZE, &attribs->depthSize);
   glXGetConfig(dpy, vInfo, GLX_STENCIL_SIZE, &attribs->stencilSize);
   glXGetConfig(dpy, vInfo, GLX_ACCUM_RED_SIZE, &attribs->accumRedSize);
   glXGetConfig(dpy, vInfo, GLX_ACCUM_GREEN_SIZE, &attribs->accumGreenSize);
   glXGetConfig(dpy, vInfo, GLX_ACCUM_BLUE_SIZE, &attribs->accumBlueSize);
   glXGetConfig(dpy, vInfo, GLX_ACCUM_ALPHA_SIZE, &attribs->accumAlphaSize);

   /* get transparent pixel stuff */
   glXGetConfig(dpy, vInfo,GLX_TRANSPARENT_TYPE, &attribs->transparentType);
   if (attribs->transparentType == GLX_TRANSPARENT_RGB) {
     glXGetConfig(dpy, vInfo, GLX_TRANSPARENT_RED_VALUE, &attribs->transparentRedValue);
     glXGetConfig(dpy, vInfo, GLX_TRANSPARENT_GREEN_VALUE, &attribs->transparentGreenValue);
     glXGetConfig(dpy, vInfo, GLX_TRANSPARENT_BLUE_VALUE, &attribs->transparentBlueValue);
     glXGetConfig(dpy, vInfo, GLX_TRANSPARENT_ALPHA_VALUE, &attribs->transparentAlphaValue);
   }
   else if (attribs->transparentType == GLX_TRANSPARENT_INDEX) {
     glXGetConfig(dpy, vInfo, GLX_TRANSPARENT_INDEX_VALUE, &attribs->transparentIndexValue);
   }

   /* multisample attribs */
#ifdef GLX_ARB_multisample
   if (ext && strstr(ext, "GLX_ARB_multisample")) {
      glXGetConfig(dpy, vInfo, GLX_SAMPLE_BUFFERS_ARB, &attribs->numMultisample);
      glXGetConfig(dpy, vInfo, GLX_SAMPLES_ARB, &attribs->numSamples);
   }
#endif
   else {
      attribs->numSamples = 0;
      attribs->numMultisample = 0;
   }

#if defined(GLX_EXT_visual_rating)
   if (ext && strstr(ext, "GLX_EXT_visual_rating")) {
      glXGetConfig(dpy, vInfo, GLX_VISUAL_CAVEAT_EXT, &attribs->visualCaveat);
   }
   else {
      attribs->visualCaveat = GLX_NONE_EXT;
   }
#else
   attribs->visualCaveat = 0;
#endif

   return GL_TRUE;
}

#ifdef GLX_VERSION_1_3

static int
glx_token_to_visual_class(int visual_type)
{
   switch (visual_type) {
   case GLX_TRUE_COLOR:
      return TrueColor;
   case GLX_DIRECT_COLOR:
      return DirectColor;
   case GLX_PSEUDO_COLOR:
      return PseudoColor;
   case GLX_STATIC_COLOR:
      return StaticColor;
   case GLX_GRAY_SCALE:
      return GrayScale;
   case GLX_STATIC_GRAY:
      return StaticGray;
   case GLX_NONE:
   default:
      return None;
   }
}

static int
get_fbconfig_attribs(Display *dpy, GLXFBConfig fbconfig,
		     struct visual_attribs *attribs)
{
   int visual_type;
   int fbconfig_id;

   memset(attribs, 0, sizeof(struct visual_attribs));

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_FBCONFIG_ID, &fbconfig_id);

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_VISUAL_ID, &attribs->id);

#if 0
   attribs->depth = vInfo->depth;
   attribs->redMask = vInfo->red_mask;
   attribs->greenMask = vInfo->green_mask;
   attribs->blueMask = vInfo->blue_mask;
   attribs->colormapSize = vInfo->colormap_size;
   attribs->bitsPerRGB = vInfo->bits_per_rgb;
#endif

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_X_VISUAL_TYPE, &visual_type);
   attribs->klass = glx_token_to_visual_class(visual_type);

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_BUFFER_SIZE, &attribs->bufferSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_LEVEL, &attribs->level);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_RENDER_TYPE, &attribs->render_type);
   if (!(attribs->render_type & GLX_RGBA_BIT))
      return 0;

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_DOUBLEBUFFER, &attribs->doubleBuffer);
   if (!attribs->doubleBuffer)
      return 0;

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_STEREO, &attribs->stereo);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_AUX_BUFFERS, &attribs->auxBuffers);

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_RED_SIZE, &attribs->redSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_GREEN_SIZE, &attribs->greenSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_BLUE_SIZE, &attribs->blueSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_ALPHA_SIZE, &attribs->alphaSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_DEPTH_SIZE, &attribs->depthSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_STENCIL_SIZE, &attribs->stencilSize);

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_ACCUM_RED_SIZE, &attribs->accumRedSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_ACCUM_GREEN_SIZE, &attribs->accumGreenSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_ACCUM_BLUE_SIZE, &attribs->accumBlueSize);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_ACCUM_ALPHA_SIZE, &attribs->accumAlphaSize);

   /* get transparent pixel stuff */
   glXGetFBConfigAttrib(dpy, fbconfig,GLX_TRANSPARENT_TYPE, &attribs->transparentType);
   if (attribs->transparentType == GLX_TRANSPARENT_RGB) {
     glXGetFBConfigAttrib(dpy, fbconfig, GLX_TRANSPARENT_RED_VALUE, &attribs->transparentRedValue);
     glXGetFBConfigAttrib(dpy, fbconfig, GLX_TRANSPARENT_GREEN_VALUE, &attribs->transparentGreenValue);
     glXGetFBConfigAttrib(dpy, fbconfig, GLX_TRANSPARENT_BLUE_VALUE, &attribs->transparentBlueValue);
     glXGetFBConfigAttrib(dpy, fbconfig, GLX_TRANSPARENT_ALPHA_VALUE, &attribs->transparentAlphaValue);
   }
   else if (attribs->transparentType == GLX_TRANSPARENT_INDEX) {
     glXGetFBConfigAttrib(dpy, fbconfig, GLX_TRANSPARENT_INDEX_VALUE, &attribs->transparentIndexValue);
   }

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_SAMPLE_BUFFERS, &attribs->numMultisample);
   glXGetFBConfigAttrib(dpy, fbconfig, GLX_SAMPLES, &attribs->numSamples);

   glXGetFBConfigAttrib(dpy, fbconfig, GLX_CONFIG_CAVEAT, &attribs->visualCaveat);

   if (attribs->id == 0) {
      attribs->id = fbconfig_id;
      return EGL_PBUFFER_BIT | EGL_PIXMAP_BIT;
   }

   return EGL_WINDOW_BIT;
}

#endif

static EGLBoolean
create_configs(_EGLDisplay *disp, struct GLX_egl_display *GLX_dpy)
{
   XVisualInfo theTemplate;
   int numVisuals;
   long mask;
   int i;
   struct visual_attribs attribs;

   GLX_dpy->fbconfigs = NULL;

#ifdef GLX_VERSION_1_3
   /* get list of all fbconfigs on this screen */
   GLX_dpy->fbconfigs = glXGetFBConfigs(GLX_dpy->dpy, DefaultScreen(GLX_dpy->dpy), &numVisuals);

   if (numVisuals == 0) {
      GLX_dpy->fbconfigs = NULL;
      goto xvisual;
   }

   for (i = 0; i < numVisuals; i++) {
      struct GLX_egl_config *config;
      int bit;

      bit = get_fbconfig_attribs(GLX_dpy->dpy, GLX_dpy->fbconfigs[i], &attribs);
      if (!bit)
         continue;

      config = CALLOC_STRUCT(GLX_egl_config);

      config->index = i;
      _eglInitConfig(&config->Base, (i+1));
      SET_CONFIG_ATTRIB(&config->Base, EGL_NATIVE_VISUAL_ID, attribs.id);
      SET_CONFIG_ATTRIB(&config->Base, EGL_BUFFER_SIZE, attribs.bufferSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_RED_SIZE, attribs.redSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_GREEN_SIZE, attribs.greenSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_BLUE_SIZE, attribs.blueSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_ALPHA_SIZE, attribs.alphaSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_DEPTH_SIZE, attribs.depthSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_STENCIL_SIZE, attribs.stencilSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_SAMPLES, attribs.numSamples);
      SET_CONFIG_ATTRIB(&config->Base, EGL_SAMPLE_BUFFERS, attribs.numMultisample);
      SET_CONFIG_ATTRIB(&config->Base, EGL_CONFORMANT, all_apis);
      SET_CONFIG_ATTRIB(&config->Base, EGL_RENDERABLE_TYPE, all_apis);
      SET_CONFIG_ATTRIB(&config->Base, EGL_SURFACE_TYPE, bit);

      /* XXX possibly other things to init... */

      _eglAddConfig(disp, &config->Base);
   }

   goto end;
#endif

xvisual:
   /* get list of all visuals on this screen */
   theTemplate.screen = DefaultScreen(GLX_dpy->dpy);
   mask = VisualScreenMask;
   GLX_dpy->visuals = XGetVisualInfo(GLX_dpy->dpy, mask, &theTemplate, &numVisuals);

   for (i = 0; i < numVisuals; i++) {
      struct GLX_egl_config *config;

      if (!get_visual_attribs(GLX_dpy->dpy, &GLX_dpy->visuals[i], &attribs))
	 continue;

      config = CALLOC_STRUCT(GLX_egl_config);

      config->index = i;
      _eglInitConfig(&config->Base, (i+1));
      SET_CONFIG_ATTRIB(&config->Base, EGL_NATIVE_VISUAL_ID, attribs.id);
      SET_CONFIG_ATTRIB(&config->Base, EGL_BUFFER_SIZE, attribs.bufferSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_RED_SIZE, attribs.redSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_GREEN_SIZE, attribs.greenSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_BLUE_SIZE, attribs.blueSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_ALPHA_SIZE, attribs.alphaSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_DEPTH_SIZE, attribs.depthSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_STENCIL_SIZE, attribs.stencilSize);
      SET_CONFIG_ATTRIB(&config->Base, EGL_SAMPLES, attribs.numSamples);
      SET_CONFIG_ATTRIB(&config->Base, EGL_SAMPLE_BUFFERS, attribs.numMultisample);
      SET_CONFIG_ATTRIB(&config->Base, EGL_CONFORMANT, all_apis);
      SET_CONFIG_ATTRIB(&config->Base, EGL_RENDERABLE_TYPE, all_apis);
      SET_CONFIG_ATTRIB(&config->Base, EGL_SURFACE_TYPE,
                        (EGL_WINDOW_BIT /*| EGL_PBUFFER_BIT | EGL_PIXMAP_BIT*/));

      /* XXX possibly other things to init... */

      _eglAddConfig(disp, &config->Base);
   }

end:
   return EGL_TRUE;
}

/**
 * Called via eglInitialize(), GLX_drv->API.Initialize().
 */
static EGLBoolean
GLX_eglInitialize(_EGLDriver *drv, _EGLDisplay *disp,
                   EGLint *major, EGLint *minor)
{
   struct GLX_egl_display *GLX_dpy;

   GLX_dpy = CALLOC_STRUCT(GLX_egl_display);
   if (!GLX_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   GLX_dpy->dpy = (Display *) disp->NativeDisplay;
   if (!GLX_dpy->dpy) {
      GLX_dpy->dpy = XOpenDisplay(NULL);
      if (!GLX_dpy->dpy) {
         _eglLog(_EGL_WARNING, "GLX: XOpenDisplay failed");
         free(GLX_dpy);
         return EGL_FALSE;
      }
   }

   disp->DriverData = (void *) GLX_dpy;
   disp->ClientAPIsMask = all_apis;

   glXQueryVersion(GLX_dpy->dpy, &GLX_dpy->glx_maj, &GLX_dpy->glx_min);

   /* we're supporting EGL 1.4 */
   *major = 1;
   *minor = 4;

   create_configs(disp, GLX_dpy);

   return EGL_TRUE;
}

/**
 * Called via eglTerminate(), drv->API.Terminate().
 */
static EGLBoolean
GLX_eglTerminate(_EGLDriver *drv, _EGLDisplay *disp)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);

   _eglReleaseDisplayResources(drv, disp);
   _eglCleanupDisplay(disp);

   if (GLX_dpy->visuals)
      XFree(GLX_dpy->visuals);
   if (GLX_dpy->fbconfigs)
      XFree(GLX_dpy->fbconfigs);

   if (!disp->NativeDisplay)
      XCloseDisplay(GLX_dpy->dpy);
   free(GLX_dpy);

   disp->DriverData = NULL;

   return EGL_TRUE;
}


/**
 * Called via eglCreateContext(), drv->API.CreateContext().
 */
static _EGLContext *
GLX_eglCreateContext(_EGLDriver *drv, _EGLDisplay *disp, _EGLConfig *conf,
                      _EGLContext *share_list, const EGLint *attrib_list)
{
   struct GLX_egl_context *GLX_ctx = CALLOC_STRUCT(GLX_egl_context);
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   struct GLX_egl_context *GLX_ctx_shared = GLX_egl_context(share_list);

   if (!GLX_ctx) {
      _eglError(EGL_BAD_ALLOC, "eglCreateContext");
      return NULL;
   }

   if (!_eglInitContext(drv, &GLX_ctx->Base, conf, attrib_list)) {
      free(GLX_ctx);
      return NULL;
   }

#ifdef GLX_VERSION_1_3
   if (GLX_dpy->fbconfigs)
      GLX_ctx->context =
         glXCreateNewContext(GLX_dpy->dpy,
                             GLX_dpy->fbconfigs[GLX_egl_config_index(conf)],
                             GLX_RGBA_TYPE,
                             GLX_ctx_shared ? GLX_ctx_shared->context : NULL,
                             GL_TRUE);
   else
#endif
      GLX_ctx->context =
         glXCreateContext(GLX_dpy->dpy,
                          &GLX_dpy->visuals[GLX_egl_config_index(conf)],
                          GLX_ctx_shared ? GLX_ctx_shared->context : NULL,
                          GL_TRUE);
   if (!GLX_ctx->context) {
      free(GLX_ctx);
      return NULL;
   }

#if 1
   /* (maybe?) need to have a direct rendering context */
   if (!glXIsDirect(GLX_dpy->dpy, GLX_ctx->context)) {
      glXDestroyContext(GLX_dpy->dpy, GLX_ctx->context);
      free(GLX_ctx);
      return NULL;
   }
#endif

   return &GLX_ctx->Base;
}


/**
 * Called via eglMakeCurrent(), drv->API.MakeCurrent().
 */
static EGLBoolean
GLX_eglMakeCurrent(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *dsurf,
                   _EGLSurface *rsurf, _EGLContext *ctx)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   struct GLX_egl_surface *GLX_dsurf = GLX_egl_surface(dsurf);
   struct GLX_egl_surface *GLX_rsurf = GLX_egl_surface(rsurf);
   struct GLX_egl_context *GLX_ctx = GLX_egl_context(ctx);
   GLXDrawable ddraw, rdraw;
   GLXContext cctx;

   if (!_eglMakeCurrent(drv, disp, dsurf, rsurf, ctx))
      return EGL_FALSE;

   ddraw = (GLX_dsurf) ? GLX_dsurf->drawable : None;
   rdraw = (GLX_rsurf) ? GLX_rsurf->drawable : None;
   cctx = (GLX_ctx) ? GLX_ctx->context : NULL;

#ifdef GLX_VERSION_1_3
   if (glXMakeContextCurrent(GLX_dpy->dpy, ddraw, rdraw, cctx))
      return EGL_TRUE;
#endif

   if (ddraw == rdraw && glXMakeCurrent(GLX_dpy->dpy, ddraw, cctx))
      return EGL_TRUE;

   return EGL_FALSE;
}

/** Get size of given window */
static Status
get_drawable_size(Display *dpy, Drawable d, uint *width, uint *height)
{
   Window root;
   Status stat;
   int xpos, ypos;
   unsigned int w, h, bw, depth;
   stat = XGetGeometry(dpy, d, &root, &xpos, &ypos, &w, &h, &bw, &depth);
   *width = w;
   *height = h;
   return stat;
}

/**
 * Called via eglCreateWindowSurface(), drv->API.CreateWindowSurface().
 */
static _EGLSurface *
GLX_eglCreateWindowSurface(_EGLDriver *drv, _EGLDisplay *disp, _EGLConfig *conf,
                            NativeWindowType window, const EGLint *attrib_list)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   struct GLX_egl_surface *GLX_surf;
   uint width, height;

   GLX_surf = CALLOC_STRUCT(GLX_egl_surface);
   if (!GLX_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreateWindowSurface");
      return NULL;
   }

   if (!_eglInitSurface(drv, &GLX_surf->Base, EGL_WINDOW_BIT,
                        conf, attrib_list)) {
      free(GLX_surf);
      return NULL;
   }

   GLX_surf->drawable = window;
   get_drawable_size(GLX_dpy->dpy, window, &width, &height);
   GLX_surf->Base.Width = width;
   GLX_surf->Base.Height = height;

   return &GLX_surf->Base;
}

#ifdef GLX_VERSION_1_3
static _EGLSurface *
GLX_eglCreatePixmapSurface(_EGLDriver *drv, _EGLDisplay *disp, _EGLConfig *conf,
			   NativePixmapType pixmap, const EGLint *attrib_list)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   struct GLX_egl_surface *GLX_surf;
   int i;

   /* GLX must >= 1.3 */
   if (!(GLX_dpy->glx_maj == 1 && GLX_dpy->glx_min >= 3))
      return NULL;

   GLX_surf = CALLOC_STRUCT(GLX_egl_surface);
   if (!GLX_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreatePixmapSurface");
      return NULL;
   }

   if (!_eglInitSurface(drv, &GLX_surf->Base, EGL_PIXMAP_BIT,
                        conf, attrib_list)) {
      free(GLX_surf);
      return NULL;
   }

   for (i = 0; attrib_list && attrib_list[i] != EGL_NONE; i++) {
      switch (attrib_list[i]) {
         /* no attribs at this time */
      default:
         _eglError(EGL_BAD_ATTRIBUTE, "eglCreatePixmapSurface");
         free(GLX_surf);
         return NULL;
      }
   }

   GLX_surf->drawable =
      glXCreatePixmap(GLX_dpy->dpy,
                      GLX_dpy->fbconfigs[GLX_egl_config_index(conf)],
                      pixmap, NULL);
   if (!GLX_surf->drawable) {
      free(GLX_surf);
      return NULL;
   }

   return &GLX_surf->Base;
}

static _EGLSurface *
GLX_eglCreatePbufferSurface(_EGLDriver *drv, _EGLDisplay *disp,
                            _EGLConfig *conf, const EGLint *attrib_list)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   struct GLX_egl_surface *GLX_surf;
   int attribs[5];
   int i = 0, j = 0;

   /* GLX must >= 1.3 */
   if (!(GLX_dpy->glx_maj == 1 && GLX_dpy->glx_min >= 3))
      return NULL;

   GLX_surf = CALLOC_STRUCT(GLX_egl_surface);
   if (!GLX_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreatePbufferSurface");
      return NULL;
   }

   if (!_eglInitSurface(drv, &GLX_surf->Base, EGL_PBUFFER_BIT,
                        conf, attrib_list)) {
      free(GLX_surf);
      return NULL;
   }

   while(attrib_list[i] != EGL_NONE) {
      switch (attrib_list[i]) {
         case EGL_WIDTH:
	    attribs[j++] = GLX_PBUFFER_WIDTH;
	    attribs[j++] = attrib_list[i+1];
	    break;
	 case EGL_HEIGHT:
	    attribs[j++] = GLX_PBUFFER_HEIGHT;
	    attribs[j++] = attrib_list[i+1];
	    break;
      }
      i++;
   }
   attribs[j++] = 0;

   GLX_surf->drawable =
      glXCreatePbuffer(GLX_dpy->dpy,
                       GLX_dpy->fbconfigs[GLX_egl_config_index(conf)],
                       attribs);
   if (!GLX_surf->drawable) {
      free(GLX_surf);
      return NULL;
   }

   return &GLX_surf->Base;
}
#endif

static EGLBoolean
GLX_eglDestroySurface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   if (!_eglIsSurfaceBound(surf)) {
      struct GLX_egl_surface *GLX_surf = GLX_egl_surface(surf);
      switch (surf->Type) {
      case EGL_PBUFFER_BIT:
         glXDestroyPbuffer(GLX_dpy->dpy, GLX_surf->drawable);
         break;
      case EGL_PIXMAP_BIT:
         glXDestroyPixmap(GLX_dpy->dpy, GLX_surf->drawable);
         break;
      default:
         break;
      }
      free(surf);
   }

   return EGL_TRUE;
}


static EGLBoolean
GLX_eglBindTexImage(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf,
                     EGLint buffer)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   struct GLX_egl_surface *GLX_surf = GLX_egl_surface(surf);

   /* buffer ?? */
   glXBindTexImageEXT(GLX_dpy->dpy, GLX_surf->drawable,
                      GLX_FRONT_LEFT_EXT, NULL);

   return EGL_TRUE;
}


static EGLBoolean
GLX_eglReleaseTexImage(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf,
                        EGLint buffer)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   struct GLX_egl_surface *GLX_surf = GLX_egl_surface(surf);

   /* buffer ?? */
   glXReleaseTexImageEXT(GLX_dpy->dpy, GLX_surf->drawable,
                         GLX_FRONT_LEFT_EXT);

   return EGL_TRUE;
}


static EGLBoolean
GLX_eglSwapBuffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   struct GLX_egl_display *GLX_dpy = GLX_egl_display(disp);
   struct GLX_egl_surface *GLX_surf = GLX_egl_surface(draw);

   _eglLog(_EGL_DEBUG, "GLX: EGL SwapBuffers 0x%x",draw);

   glXSwapBuffers(GLX_dpy->dpy, GLX_surf->drawable);

   return EGL_TRUE;
}

/*
 * Called from eglGetProcAddress() via drv->API.GetProcAddress().
 */
static _EGLProc
GLX_eglGetProcAddress(const char *procname)
{
   /* This is a bit of a hack to get at the gallium/Mesa state tracker
    * function st_get_proc_address().  This will probably change at
    * some point.
    */
   _EGLProc (*get_proc_addr)(const char *procname);
   _EGLProc proc_addr;
   get_proc_addr = dlsym(NULL, "st_get_proc_address");
   if (get_proc_addr)
      return get_proc_addr(procname);

   proc_addr = glXGetProcAddress((const GLubyte *)procname);
   if (proc_addr)
      return proc_addr;

   return (_EGLProc)dlsym(NULL, procname);
}


static void
GLX_Unload(_EGLDriver *drv)
{
   struct GLX_egl_driver *GLX_drv = GLX_egl_driver(drv);
   free(GLX_drv);
}


/**
 * This is the main entrypoint into the driver, called by libEGL.
 * Create a new _EGLDriver object and init its dispatch table.
 */
_EGLDriver *
_eglMain(const char *args)
{
   struct GLX_egl_driver *GLX_drv = CALLOC_STRUCT(GLX_egl_driver);

   if (!GLX_drv)
      return NULL;

   _eglInitDriverFallbacks(&GLX_drv->Base);
   GLX_drv->Base.API.Initialize = GLX_eglInitialize;
   GLX_drv->Base.API.Terminate = GLX_eglTerminate;
   GLX_drv->Base.API.CreateContext = GLX_eglCreateContext;
   GLX_drv->Base.API.MakeCurrent = GLX_eglMakeCurrent;
   GLX_drv->Base.API.CreateWindowSurface = GLX_eglCreateWindowSurface;
#ifdef GLX_VERSION_1_3
   GLX_drv->Base.API.CreatePixmapSurface = GLX_eglCreatePixmapSurface;
   GLX_drv->Base.API.CreatePbufferSurface = GLX_eglCreatePbufferSurface;
#endif
   GLX_drv->Base.API.DestroySurface = GLX_eglDestroySurface;
   GLX_drv->Base.API.BindTexImage = GLX_eglBindTexImage;
   GLX_drv->Base.API.ReleaseTexImage = GLX_eglReleaseTexImage;
   GLX_drv->Base.API.SwapBuffers = GLX_eglSwapBuffers;
   GLX_drv->Base.API.GetProcAddress = GLX_eglGetProcAddress;

   GLX_drv->Base.Name = "GLX";
   GLX_drv->Base.Unload = GLX_Unload;

   return &GLX_drv->Base;
}
