/**
 * Public EGL API entrypoints
 *
 * Generally, we use the EGLDisplay parameter as a key to lookup the
 * appropriate device driver handle, then jump though the driver's
 * dispatch table to handle the function.
 *
 * That allows us the option of supporting multiple, simultaneous,
 * heterogeneous hardware devices in the future.
 *
 * The EGLDisplay, EGLConfig, EGLContext and EGLSurface types are
 * opaque handles implemented with 32-bit unsigned integers.
 * It's up to the driver function or fallback function to look up the
 * handle and get an object.
 * By using opaque handles, we leave open the possibility of having
 * indirect rendering in the future, like GLX.
 *
 *
 * Notes on naming conventions:
 *
 * eglFooBar    - public EGL function
 * EGL_FOO_BAR  - public EGL token
 * EGLDatatype  - public EGL datatype
 *
 * _eglFooBar   - private EGL function
 * _EGLDatatype - private EGL datatype, typedef'd struct
 * _egl_struct  - private EGL struct, non-typedef'd
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eglcontext.h"
#include "egldisplay.h"
#include "egltypedefs.h"
#include "eglglobals.h"
#include "egldriver.h"
#include "eglsurface.h"
#include "eglconfig.h"
#include "eglscreen.h"
#include "eglmode.h"
#include "eglimage.h"


/**
 * This is typically the first EGL function that an application calls.
 * We initialize our global vars and create a private _EGLDisplay object.
 */
EGLDisplay EGLAPIENTRY
eglGetDisplay(NativeDisplayType nativeDisplay)
{
   _EGLDisplay *dpy;
   dpy = _eglFindDisplay(nativeDisplay);
   if (!dpy) {
      dpy = _eglNewDisplay(nativeDisplay);
      if (dpy)
         _eglLinkDisplay(dpy);
   }
   return _eglGetDisplayHandle(dpy);
}


/**
 * This is typically the second EGL function that an application calls.
 * Here we load/initialize the actual hardware driver.
 */
EGLBoolean EGLAPIENTRY
eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLDriver *drv;
   EGLint major_int, minor_int;

   if (!disp)
      return _eglError(EGL_BAD_DISPLAY, __FUNCTION__);

   drv = disp->Driver;
   if (!drv) {
      drv = _eglOpenDriver(disp);
      if (!drv)
         return _eglError(EGL_NOT_INITIALIZED, __FUNCTION__);

      /* Initialize the particular display now */
      if (!drv->API.Initialize(drv, disp, &major_int, &minor_int)) {
         _eglCloseDriver(drv, disp);
         return _eglError(EGL_NOT_INITIALIZED, __FUNCTION__);
      }

      disp->APImajor = major_int;
      disp->APIminor = minor_int;
      snprintf(disp->Version, sizeof(disp->Version),
               "%d.%d (%s)", major_int, minor_int, drv->Name);

      /* limit to APIs supported by core */
      disp->ClientAPIsMask &= _EGL_API_ALL_BITS;

      disp->Driver = drv;
   } else {
      major_int = disp->APImajor;
      minor_int = disp->APIminor;
   }

   /* Update applications version of major and minor if not NULL */
   if ((major != NULL) && (minor != NULL)) {
      *major = major_int;
      *minor = minor_int;
   }

   return EGL_TRUE;
}


EGLBoolean EGLAPIENTRY
eglTerminate(EGLDisplay dpy)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLDriver *drv;

   if (!disp)
      return _eglError(EGL_BAD_DISPLAY, __FUNCTION__);

   drv = disp->Driver;
   if (drv) {
      drv->API.Terminate(drv, disp);
      _eglCloseDriver(drv, disp);
      disp->Driver = NULL;
   }

   return EGL_TRUE;
}


/**
 * A bunch of check functions and declare macros to simply error checking.
 */
static INLINE _EGLDriver *
_eglCheckDisplay(_EGLDisplay *disp, const char *msg)
{
   if (!disp) {
      _eglError(EGL_BAD_DISPLAY, msg);
      return NULL;
   }
   if (!disp->Driver) {
      _eglError(EGL_NOT_INITIALIZED, msg);
      return NULL;
   }
   return disp->Driver;
}


static INLINE _EGLDriver *
_eglCheckSurface(_EGLDisplay *disp, _EGLSurface *surf, const char *msg)
{
   _EGLDriver *drv = _eglCheckDisplay(disp, msg);
   if (!drv)
      return NULL;
   if (!surf) {
      _eglError(EGL_BAD_SURFACE, msg);
      return NULL;
   }
   return drv;
}


static INLINE _EGLDriver *
_eglCheckContext(_EGLDisplay *disp, _EGLContext *context, const char *msg)
{
   _EGLDriver *drv = _eglCheckDisplay(disp, msg);
   if (!drv)
      return NULL;
   if (!context) {
      _eglError(EGL_BAD_CONTEXT, msg);
      return NULL;
   }
   return drv;
}


static INLINE _EGLDriver *
_eglCheckConfig(_EGLDisplay *disp, _EGLConfig *conf, const char *msg)
{
   _EGLDriver *drv = _eglCheckDisplay(disp, msg);
   if (!drv)
      return NULL;
   if (!conf) {
      _eglError(EGL_BAD_CONFIG, msg);
      return NULL;
   }
   return drv;
}


#define _EGL_DECLARE_DD(dpy)                                   \
   _EGLDisplay *disp = _eglLookupDisplay(dpy);                 \
   _EGLDriver *drv;                                            \
   do {                                                        \
      drv = _eglCheckDisplay(disp, __FUNCTION__);              \
      if (!drv)                                                \
         return EGL_FALSE;                                     \
   } while (0)


#define _EGL_DECLARE_DD_AND_SURFACE(dpy, surface)              \
   _EGLDisplay *disp = _eglLookupDisplay(dpy);                 \
   _EGLSurface *surf = _eglLookupSurface((surface), disp);     \
   _EGLDriver *drv;                                            \
   do {                                                        \
      drv = _eglCheckSurface(disp, surf, __FUNCTION__);        \
      if (!drv)                                                \
         return EGL_FALSE;                                     \
   } while (0)


#define _EGL_DECLARE_DD_AND_CONTEXT(dpy, ctx)                  \
   _EGLDisplay *disp = _eglLookupDisplay(dpy);                 \
   _EGLContext *context = _eglLookupContext((ctx), disp);      \
   _EGLDriver *drv;                                            \
   do {                                                        \
      drv = _eglCheckContext(disp, context, __FUNCTION__);     \
      if (!drv)                                                \
         return EGL_FALSE;                                     \
   } while (0)


#ifdef EGL_MESA_screen_surface


static INLINE _EGLDriver *
_eglCheckScreen(_EGLDisplay *disp, _EGLScreen *scrn, const char *msg)
{
   _EGLDriver *drv = _eglCheckDisplay(disp, msg);
   if (!drv)
      return NULL;
   if (!scrn) {
      _eglError(EGL_BAD_SCREEN_MESA, msg);
      return NULL;
   }
   return drv;
}


static INLINE _EGLDriver *
_eglCheckMode(_EGLDisplay *disp, _EGLMode *m, const char *msg)
{
   _EGLDriver *drv = _eglCheckDisplay(disp, msg);
   if (!drv)
      return NULL;
   if (!m) {
      _eglError(EGL_BAD_MODE_MESA, msg);
      return NULL;
   }
   return drv;
}


#define _EGL_DECLARE_DD_AND_SCREEN(dpy, screen)                \
   _EGLDisplay *disp = _eglLookupDisplay(dpy);                 \
   _EGLScreen *scrn = _eglLookupScreen((screen), disp);        \
   _EGLDriver *drv;                                            \
   do {                                                        \
      drv = _eglCheckScreen(disp, scrn, __FUNCTION__);         \
      if (!drv)                                                \
         return EGL_FALSE;                                     \
   } while (0)


#define _EGL_DECLARE_DD_AND_MODE(dpy, mode)                    \
   _EGLDisplay *disp = _eglLookupDisplay(dpy);                 \
   _EGLMode *m = _eglLookupMode((mode), disp);                 \
   _EGLDriver *drv;                                            \
   do {                                                        \
      drv = _eglCheckMode(disp, m, __FUNCTION__);              \
      if (!drv)                                                \
         return EGL_FALSE;                                     \
   } while (0)


#endif /* EGL_MESA_screen_surface */


const char * EGLAPIENTRY
eglQueryString(EGLDisplay dpy, EGLint name)
{
   _EGL_DECLARE_DD(dpy);
   return drv->API.QueryString(drv, disp, name);
}


EGLBoolean EGLAPIENTRY
eglGetConfigs(EGLDisplay dpy, EGLConfig *configs,
              EGLint config_size, EGLint *num_config)
{
   _EGL_DECLARE_DD(dpy);
   return drv->API.GetConfigs(drv, disp, configs, config_size, num_config);
}


EGLBoolean EGLAPIENTRY
eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                EGLint config_size, EGLint *num_config)
{
   _EGL_DECLARE_DD(dpy);
   return drv->API.ChooseConfig(drv, disp, attrib_list, configs,
                                config_size, num_config);
}


EGLBoolean EGLAPIENTRY
eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                   EGLint attribute, EGLint *value)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLConfig *conf = _eglLookupConfig(config, disp);
   _EGLDriver *drv;

   drv = _eglCheckConfig(disp, conf, __FUNCTION__);
   if (!drv)
      return EGL_FALSE;

   return drv->API.GetConfigAttrib(drv, disp, conf, attribute, value);
}


EGLContext EGLAPIENTRY
eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_list,
                 const EGLint *attrib_list)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLConfig *conf = _eglLookupConfig(config, disp);
   _EGLContext *share = _eglLookupContext(share_list, disp);
   _EGLDriver *drv;
   _EGLContext *context;

   drv = _eglCheckConfig(disp, conf, __FUNCTION__);
   if (!drv)
      return EGL_NO_CONTEXT;
   if (!share && share_list != EGL_NO_CONTEXT) {
      _eglError(EGL_BAD_CONTEXT, __FUNCTION__);
      return EGL_NO_CONTEXT;
   }

   context = drv->API.CreateContext(drv, disp, conf, share, attrib_list);
   if (context)
      return _eglLinkContext(context, disp);
   else
      return EGL_NO_CONTEXT;
}


EGLBoolean EGLAPIENTRY
eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
   _EGL_DECLARE_DD_AND_CONTEXT(dpy, ctx);
   _eglUnlinkContext(context);
   return drv->API.DestroyContext(drv, disp, context);
}


EGLBoolean EGLAPIENTRY
eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
               EGLContext ctx)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLContext *context = _eglLookupContext(ctx, disp);
   _EGLSurface *draw_surf = _eglLookupSurface(draw, disp);
   _EGLSurface *read_surf = _eglLookupSurface(read, disp);
   _EGLDriver *drv;

   drv = _eglCheckDisplay(disp, __FUNCTION__);
   if (!drv)
      return EGL_FALSE;
   if (!context && ctx != EGL_NO_CONTEXT)
      return _eglError(EGL_BAD_CONTEXT, __FUNCTION__);
   if ((!draw_surf && draw != EGL_NO_SURFACE) ||
       (!read_surf && read != EGL_NO_SURFACE))
      return _eglError(EGL_BAD_SURFACE, __FUNCTION__);

   return drv->API.MakeCurrent(drv, disp, draw_surf, read_surf, context);
}


EGLBoolean EGLAPIENTRY
eglQueryContext(EGLDisplay dpy, EGLContext ctx,
                EGLint attribute, EGLint *value)
{
   _EGL_DECLARE_DD_AND_CONTEXT(dpy, ctx);
   return drv->API.QueryContext(drv, disp, context, attribute, value);
}


EGLSurface EGLAPIENTRY
eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                       NativeWindowType window, const EGLint *attrib_list)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLConfig *conf = _eglLookupConfig(config, disp);
   _EGLDriver *drv;
   _EGLSurface *surf;

   drv = _eglCheckConfig(disp, conf, __FUNCTION__);
   if (!drv)
      return EGL_NO_SURFACE;

   surf = drv->API.CreateWindowSurface(drv, disp, conf, window, attrib_list);
   if (surf)
      return _eglLinkSurface(surf, disp);
   else
      return EGL_NO_SURFACE;
}


EGLSurface EGLAPIENTRY
eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config,
                       NativePixmapType pixmap, const EGLint *attrib_list)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLConfig *conf = _eglLookupConfig(config, disp);
   _EGLDriver *drv;
   _EGLSurface *surf;

   drv = _eglCheckConfig(disp, conf, __FUNCTION__);
   if (!drv)
      return EGL_NO_SURFACE;

   surf = drv->API.CreatePixmapSurface(drv, disp, conf, pixmap, attrib_list);
   if (surf)
      return _eglLinkSurface(surf, disp);
   else
      return EGL_NO_SURFACE;
}


EGLSurface EGLAPIENTRY
eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                        const EGLint *attrib_list)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLConfig *conf = _eglLookupConfig(config, disp);
   _EGLDriver *drv;
   _EGLSurface *surf;

   drv = _eglCheckConfig(disp, conf, __FUNCTION__);
   if (!drv)
      return EGL_NO_SURFACE;

   surf = drv->API.CreatePbufferSurface(drv, disp, conf, attrib_list);
   if (surf)
      return _eglLinkSurface(surf, disp);
   else
      return EGL_NO_SURFACE;
}


EGLBoolean EGLAPIENTRY
eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
   _EGL_DECLARE_DD_AND_SURFACE(dpy, surface);
   _eglUnlinkSurface(surf);
   return drv->API.DestroySurface(drv, disp, surf);
}

EGLBoolean EGLAPIENTRY
eglQuerySurface(EGLDisplay dpy, EGLSurface surface,
                EGLint attribute, EGLint *value)
{
   _EGL_DECLARE_DD_AND_SURFACE(dpy, surface);
   return drv->API.QuerySurface(drv, disp, surf, attribute, value);
}

EGLBoolean EGLAPIENTRY
eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface,
                 EGLint attribute, EGLint value)
{
   _EGL_DECLARE_DD_AND_SURFACE(dpy, surface);
   return drv->API.SurfaceAttrib(drv, disp, surf, attribute, value);
}


EGLBoolean EGLAPIENTRY
eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
   _EGL_DECLARE_DD_AND_SURFACE(dpy, surface);
   return drv->API.BindTexImage(drv, disp, surf, buffer);
}


EGLBoolean EGLAPIENTRY
eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
   _EGL_DECLARE_DD_AND_SURFACE(dpy, surface);
   return drv->API.ReleaseTexImage(drv, disp, surf, buffer);
}


EGLBoolean EGLAPIENTRY
eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
   _EGL_DECLARE_DD(dpy);
   return drv->API.SwapInterval(drv, disp, interval);
}


EGLBoolean EGLAPIENTRY
eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
   _EGL_DECLARE_DD_AND_SURFACE(dpy, surface);
   return drv->API.SwapBuffers(drv, disp, surf);
}


EGLBoolean EGLAPIENTRY
eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, NativePixmapType target)
{
   _EGL_DECLARE_DD_AND_SURFACE(dpy, surface);
   return drv->API.CopyBuffers(drv, disp, surf, target);
}


EGLBoolean EGLAPIENTRY
eglWaitGL(void)
{
   _EGLDisplay *disp = _eglGetCurrentDisplay();
   _EGLDriver *drv;

   if (!disp)
      return EGL_TRUE;

   /* a current display is always initialized */
   drv = disp->Driver;

   return drv->API.WaitGL(drv, disp);
}


EGLBoolean EGLAPIENTRY
eglWaitNative(EGLint engine)
{
   _EGLDisplay *disp = _eglGetCurrentDisplay();
   _EGLDriver *drv;

   if (!disp)
      return EGL_TRUE;

   /* a current display is always initialized */
   drv = disp->Driver;

   return drv->API.WaitNative(drv, disp, engine);
}


EGLDisplay EGLAPIENTRY
eglGetCurrentDisplay(void)
{
   _EGLDisplay *dpy = _eglGetCurrentDisplay();
   return _eglGetDisplayHandle(dpy);
}


EGLContext EGLAPIENTRY
eglGetCurrentContext(void)
{
   _EGLContext *ctx = _eglGetCurrentContext();
   return _eglGetContextHandle(ctx);
}


EGLSurface EGLAPIENTRY
eglGetCurrentSurface(EGLint readdraw)
{
   _EGLSurface *s = _eglGetCurrentSurface(readdraw);
   return _eglGetSurfaceHandle(s);
}


EGLint EGLAPIENTRY
eglGetError(void)
{
   _EGLThreadInfo *t = _eglGetCurrentThread();
   EGLint e = t->LastError;
   if (!_eglIsCurrentThreadDummy())
      t->LastError = EGL_SUCCESS;
   return e;
}


void (* EGLAPIENTRY eglGetProcAddress(const char *procname))()
{
   typedef void (*genericFunc)();
   struct name_function {
      const char *name;
      _EGLProc function;
   };
   static struct name_function egl_functions[] = {
      /* alphabetical order */
      { "eglBindTexImage", (_EGLProc) eglBindTexImage },
      { "eglChooseConfig", (_EGLProc) eglChooseConfig },
      { "eglCopyBuffers", (_EGLProc) eglCopyBuffers },
      { "eglCreateContext", (_EGLProc) eglCreateContext },
      { "eglCreatePbufferSurface", (_EGLProc) eglCreatePbufferSurface },
      { "eglCreatePixmapSurface", (_EGLProc) eglCreatePixmapSurface },
      { "eglCreateWindowSurface", (_EGLProc) eglCreateWindowSurface },
      { "eglDestroyContext", (_EGLProc) eglDestroyContext },
      { "eglDestroySurface", (_EGLProc) eglDestroySurface },
      { "eglGetConfigAttrib", (_EGLProc) eglGetConfigAttrib },
      { "eglGetConfigs", (_EGLProc) eglGetConfigs },
      { "eglGetCurrentContext", (_EGLProc) eglGetCurrentContext },
      { "eglGetCurrentDisplay", (_EGLProc) eglGetCurrentDisplay },
      { "eglGetCurrentSurface", (_EGLProc) eglGetCurrentSurface },
      { "eglGetDisplay", (_EGLProc) eglGetDisplay },
      { "eglGetError", (_EGLProc) eglGetError },
      { "eglGetProcAddress", (_EGLProc) eglGetProcAddress },
      { "eglInitialize", (_EGLProc) eglInitialize },
      { "eglMakeCurrent", (_EGLProc) eglMakeCurrent },
      { "eglQueryContext", (_EGLProc) eglQueryContext },
      { "eglQueryString", (_EGLProc) eglQueryString },
      { "eglQuerySurface", (_EGLProc) eglQuerySurface },
      { "eglReleaseTexImage", (_EGLProc) eglReleaseTexImage },
      { "eglSurfaceAttrib", (_EGLProc) eglSurfaceAttrib },
      { "eglSwapBuffers", (_EGLProc) eglSwapBuffers },
      { "eglSwapInterval", (_EGLProc) eglSwapInterval },
      { "eglTerminate", (_EGLProc) eglTerminate },
      { "eglWaitGL", (_EGLProc) eglWaitGL },
      { "eglWaitNative", (_EGLProc) eglWaitNative },
      /* Extensions */
#ifdef EGL_MESA_screen_surface
      { "eglChooseModeMESA", (_EGLProc) eglChooseModeMESA },
      { "eglGetModesMESA", (_EGLProc) eglGetModesMESA },
      { "eglGetModeAttribMESA", (_EGLProc) eglGetModeAttribMESA },
      { "eglCopyContextMESA", (_EGLProc) eglCopyContextMESA },
      { "eglGetScreensMESA", (_EGLProc) eglGetScreensMESA },
      { "eglCreateScreenSurfaceMESA", (_EGLProc) eglCreateScreenSurfaceMESA },
      { "eglShowScreenSurfaceMESA", (_EGLProc) eglShowScreenSurfaceMESA },
      { "eglScreenPositionMESA", (_EGLProc) eglScreenPositionMESA },
      { "eglQueryScreenMESA", (_EGLProc) eglQueryScreenMESA },
      { "eglQueryScreenSurfaceMESA", (_EGLProc) eglQueryScreenSurfaceMESA },
      { "eglQueryScreenModeMESA", (_EGLProc) eglQueryScreenModeMESA },
      { "eglQueryModeStringMESA", (_EGLProc) eglQueryModeStringMESA },
#endif /* EGL_MESA_screen_surface */
#ifdef EGL_VERSION_1_2
      { "eglBindAPI", (_EGLProc) eglBindAPI },
      { "eglCreatePbufferFromClientBuffer", (_EGLProc) eglCreatePbufferFromClientBuffer },
      { "eglQueryAPI", (_EGLProc) eglQueryAPI },
      { "eglReleaseThread", (_EGLProc) eglReleaseThread },
      { "eglWaitClient", (_EGLProc) eglWaitClient },
#endif /* EGL_VERSION_1_2 */
#ifdef EGL_KHR_image_base
      { "eglCreateImageKHR", (_EGLProc) eglCreateImageKHR },
      { "eglDestroyImageKHR", (_EGLProc) eglDestroyImageKHR },
#endif /* EGL_KHR_image_base */
      { NULL, NULL }
   };
   EGLint i;
   for (i = 0; egl_functions[i].name; i++) {
      if (strcmp(egl_functions[i].name, procname) == 0) {
         return (genericFunc) egl_functions[i].function;
      }
   }

   /* preload a driver if there isn't one */
   if (!_eglGlobal.NumDrivers)
      _eglPreloadDriver(NULL);

   /* now loop over drivers to query their procs */
   for (i = 0; i < _eglGlobal.NumDrivers; i++) {
      _EGLProc p = _eglGlobal.Drivers[i]->API.GetProcAddress(procname);
      if (p)
         return p;
   }

   return NULL;
}


/*
 * EGL_MESA_screen extension
 */

EGLBoolean EGLAPIENTRY
eglChooseModeMESA(EGLDisplay dpy, EGLScreenMESA screen,
                  const EGLint *attrib_list, EGLModeMESA *modes,
                  EGLint modes_size, EGLint *num_modes)
{
   _EGL_DECLARE_DD_AND_SCREEN(dpy, screen);
   return drv->API.ChooseModeMESA(drv, disp, scrn, attrib_list,
                                  modes, modes_size, num_modes);
}


EGLBoolean EGLAPIENTRY
eglGetModesMESA(EGLDisplay dpy, EGLScreenMESA screen, EGLModeMESA *modes,
                EGLint mode_size, EGLint *num_mode)
{
   _EGL_DECLARE_DD_AND_SCREEN(dpy, screen);
   return drv->API.GetModesMESA(drv, disp, scrn, modes, mode_size, num_mode);
}


EGLBoolean EGLAPIENTRY
eglGetModeAttribMESA(EGLDisplay dpy, EGLModeMESA mode,
                     EGLint attribute, EGLint *value)
{
   _EGL_DECLARE_DD_AND_MODE(dpy, mode);
   return drv->API.GetModeAttribMESA(drv, disp, m, attribute, value);
}


EGLBoolean EGLAPIENTRY
eglCopyContextMESA(EGLDisplay dpy, EGLContext source, EGLContext dest,
                   EGLint mask)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLContext *source_context = _eglLookupContext(source, disp);
   _EGLContext *dest_context = _eglLookupContext(dest, disp);
   _EGLDriver *drv;

   drv = _eglCheckContext(disp, source_context, __FUNCTION__);
   if (!drv || !dest_context) {
      if (drv)
         _eglError(EGL_BAD_CONTEXT, __FUNCTION__);
      return EGL_FALSE;
   }

   return drv->API.CopyContextMESA(drv, disp, source_context, dest_context,
                                   mask);
}


EGLBoolean
eglGetScreensMESA(EGLDisplay dpy, EGLScreenMESA *screens,
                  EGLint max_screens, EGLint *num_screens)
{
   _EGL_DECLARE_DD(dpy);
   return drv->API.GetScreensMESA(drv, disp, screens,
                                  max_screens, num_screens);
}


EGLSurface
eglCreateScreenSurfaceMESA(EGLDisplay dpy, EGLConfig config,
                           const EGLint *attrib_list)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLConfig *conf = _eglLookupConfig(config, disp);
   _EGLDriver *drv;
   _EGLSurface *surf;

   drv = _eglCheckConfig(disp, conf, __FUNCTION__);
   if (!drv)
      return EGL_NO_SURFACE;

   surf = drv->API.CreateScreenSurfaceMESA(drv, disp, conf, attrib_list);
   if (surf)
      return _eglLinkSurface(surf, disp);
   else
      return EGL_NO_SURFACE;
}


EGLBoolean
eglShowScreenSurfaceMESA(EGLDisplay dpy, EGLint screen,
                         EGLSurface surface, EGLModeMESA mode)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLScreen *scrn = _eglLookupScreen((EGLScreenMESA) screen, disp);
   _EGLSurface *surf = _eglLookupSurface(surface, disp);
   _EGLMode *m = _eglLookupMode(mode, disp);
   _EGLDriver *drv;

   drv = _eglCheckScreen(disp, scrn, __FUNCTION__);
   if (!drv)
      return EGL_FALSE;
   if (!surf && surface != EGL_NO_SURFACE)
      return _eglError(EGL_BAD_SURFACE, __FUNCTION__);
   if (!m && mode != EGL_NO_MODE_MESA)
      return _eglError(EGL_BAD_MODE_MESA, __FUNCTION__);

   return drv->API.ShowScreenSurfaceMESA(drv, disp, scrn, surf, m);
}


EGLBoolean
eglScreenPositionMESA(EGLDisplay dpy, EGLScreenMESA screen, EGLint x, EGLint y)
{
   _EGL_DECLARE_DD_AND_SCREEN(dpy, screen);
   return drv->API.ScreenPositionMESA(drv, disp, scrn, x, y);
}


EGLBoolean
eglQueryScreenMESA(EGLDisplay dpy, EGLScreenMESA screen,
                   EGLint attribute, EGLint *value)
{
   _EGL_DECLARE_DD_AND_SCREEN(dpy, screen);
   return drv->API.QueryScreenMESA(drv, disp, scrn, attribute, value);
}


EGLBoolean
eglQueryScreenSurfaceMESA(EGLDisplay dpy, EGLScreenMESA screen,
                          EGLSurface *surface)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLScreen *scrn = _eglLookupScreen((EGLScreenMESA) screen, disp);
   _EGLDriver *drv;
   _EGLSurface *surf;

   drv = _eglCheckScreen(disp, scrn, __FUNCTION__);
   if (!drv)
      return EGL_FALSE;

   if (drv->API.QueryScreenSurfaceMESA(drv, disp, scrn, &surf) != EGL_TRUE)
      surf = NULL;
   if (surface)
      *surface = _eglGetSurfaceHandle(surf);
   return (surf != NULL);
}


EGLBoolean
eglQueryScreenModeMESA(EGLDisplay dpy, EGLScreenMESA screen, EGLModeMESA *mode)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLScreen *scrn = _eglLookupScreen((EGLScreenMESA) screen, disp);
   _EGLDriver *drv;
   _EGLMode *m;

   drv = _eglCheckScreen(disp, scrn, __FUNCTION__);
   if (!drv)
      return EGL_FALSE;

   if (drv->API.QueryScreenModeMESA(drv, disp, scrn, &m) != EGL_TRUE)
      m = NULL;
   if (mode)
      *mode = m->Handle;

   return (m != NULL);
}


const char *
eglQueryModeStringMESA(EGLDisplay dpy, EGLModeMESA mode)
{
   _EGL_DECLARE_DD_AND_MODE(dpy, mode);
   return drv->API.QueryModeStringMESA(drv, disp, m);
}


/**
 ** EGL 1.2
 **/

#ifdef EGL_VERSION_1_2


/**
 * Specify the client API to use for subsequent calls including:
 *  eglCreateContext()
 *  eglGetCurrentContext()
 *  eglGetCurrentDisplay()
 *  eglGetCurrentSurface()
 *  eglMakeCurrent(when the ctx parameter is EGL NO CONTEXT)
 *  eglWaitClient()
 *  eglWaitNative()
 * See section 3.7 "Rendering Context" in the EGL specification for details.
 */
EGLBoolean
eglBindAPI(EGLenum api)
{
   _EGLThreadInfo *t = _eglGetCurrentThread();

   if (_eglIsCurrentThreadDummy())
      return _eglError(EGL_BAD_ALLOC, "eglBindAPI");

   if (!_eglIsApiValid(api))
      return _eglError(EGL_BAD_PARAMETER, "eglBindAPI");

   t->CurrentAPIIndex = _eglConvertApiToIndex(api);
   return EGL_TRUE;
}


/**
 * Return the last value set with eglBindAPI().
 */
EGLenum
eglQueryAPI(void)
{
   /* returns one of EGL_OPENGL_API, EGL_OPENGL_ES_API or EGL_OPENVG_API */
   _EGLThreadInfo *t = _eglGetCurrentThread();
   return _eglConvertApiFromIndex(t->CurrentAPIIndex);
}


EGLSurface
eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype,
                                 EGLClientBuffer buffer, EGLConfig config,
                                 const EGLint *attrib_list)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLConfig *conf = _eglLookupConfig(config, disp);
   _EGLDriver *drv;
   _EGLSurface *surf;

   drv = _eglCheckConfig(disp, conf, __FUNCTION__);
   if (!drv)
      return EGL_NO_SURFACE;

   surf = drv->API.CreatePbufferFromClientBuffer(drv, disp, buftype, buffer,
                                                 conf, attrib_list);
   if (surf)
      return _eglLinkSurface(surf, disp);
   else
      return EGL_NO_SURFACE;
}


EGLBoolean
eglReleaseThread(void)
{
   /* unbind current context */
   if (!_eglIsCurrentThreadDummy()) {
      _EGLDisplay *disp = _eglGetCurrentDisplay();
      _EGLDriver *drv;
      if (disp) {
         drv = disp->Driver;
         (void) drv->API.MakeCurrent(drv, disp, NULL, NULL, NULL);
      }
   }

   _eglDestroyCurrentThread();
   return EGL_TRUE;
}


EGLBoolean
eglWaitClient(void)
{
   _EGLDisplay *disp = _eglGetCurrentDisplay();
   _EGLDriver *drv;

   if (!disp)
      return EGL_TRUE;

   /* a current display is always initialized */
   drv = disp->Driver;

   return drv->API.WaitClient(drv, disp);
}


#endif /* EGL_VERSION_1_2 */


#ifdef EGL_KHR_image_base


EGLImageKHR
eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attr_list)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLContext *context = _eglLookupContext(ctx, disp);
   _EGLDriver *drv;
   _EGLImage *img;

   drv = _eglCheckDisplay(disp, __FUNCTION__);
   if (!drv)
      return EGL_NO_IMAGE_KHR;
   if (!context && ctx != EGL_NO_CONTEXT) {
      _eglError(EGL_BAD_CONTEXT, __FUNCTION__);
      return EGL_NO_IMAGE_KHR;
   }

   img = drv->API.CreateImageKHR(drv, disp, context, target, buffer, attr_list);
   if (img)
      return _eglLinkImage(img, disp);
   else
      return EGL_NO_IMAGE_KHR;
}


EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image)
{
   _EGLDisplay *disp = _eglLookupDisplay(dpy);
   _EGLImage *img = _eglLookupImage(image, disp);
   _EGLDriver *drv;

   drv = _eglCheckDisplay(disp, __FUNCTION__);
   if (!drv)
      return EGL_FALSE;
   if (!img)
      return _eglError(EGL_BAD_PARAMETER, __FUNCTION__);

   _eglUnlinkImage(img);
   return drv->API.DestroyImageKHR(drv, disp, img);
}


#endif /* EGL_KHR_image_base */
