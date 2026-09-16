/*
$info$
tags: thunklibs|GL
desc: Uses glXGetProcAddress instead of dlsym
$end_info$
*/

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1

#include "glcorearb.h"

#include <GL/glx.h>
#include <GL/glxext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <xcb/xcb.h>

#include "common/Host.h"
#include "common/X11Manager.h"

template<>
struct host_layout<_XDisplay*> {
  _XDisplay* data;
  _XDisplay* guest_display;

  host_layout(guest_layout<_XDisplay*>&);

  ~host_layout();
};


static X11Manager x11_manager;

static void* (*GuestMalloc)(guest_size_t) = nullptr;

// Diagnostic X error handler. libX11's default handler prints and calls
// exit(1) on BadDrawable etc., which prevents seeing what came before and
// what request triggered it. Print extra context and let execution continue
// so we can characterize the call chain. Opt-in via FEX_LIBGL_DEBUG=1.
static int LoggingXErrorHandler(Display* d, XErrorEvent* ev) {
  fprintf(stderr, "[fex-libGL] X Error: code=%u request=%u.%u resource=0x%lx serial=%lu\n",
          (unsigned)ev->error_code, (unsigned)ev->request_code,
          (unsigned)ev->minor_code, (unsigned long)ev->resourceid,
          (unsigned long)ev->serial);
  return 0; // do not abort
}
static bool FexLibGLDebug() {
  static const bool enabled = (getenv("FEX_LIBGL_DEBUG") != nullptr);
  return enabled;
}
static void InstallLoggingXErrorHandler() {
  static bool installed = false;
  if (!installed && FexLibGLDebug()) {
    using XSetErrorHandler_t = int (*(*)(int (*)(Display*, XErrorEvent*)))(Display*, XErrorEvent*);
    auto setter = reinterpret_cast<XSetErrorHandler_t>(dlsym(X11Manager::GetLibX11(), "XSetErrorHandler"));
    if (setter) {
      setter(LoggingXErrorHandler);
    }
    installed = true;
  }
}

host_layout<_XDisplay*>::host_layout(guest_layout<_XDisplay*>& guest)
  : guest_display(guest.force_get_host_pointer()) {
  InstallLoggingXErrorHandler();
  data = x11_manager.GuestToHostDisplay(guest_display);
}

host_layout<_XDisplay*>::~host_layout() {
  // This used to XFlush the host connection unconditionally — a write(2) to
  // the X socket per Display-taking GL call, defeating Xlib request batching
  // entirely. It is not needed for rendering: glXSwapBuffers issues its own
  // flush as part of the swap protocol, and non-swap GLX requests that a guest
  // could observe cross-connection get pushed out by the server round trips
  // libX11 already performs for reply-carrying requests. Kept behind a triage
  // env: if a title's window updates lag or GLX state appears stale
  // cross-connection, set FEX_X11_FLUSH_EVERY_CALL=1 to confirm this elision
  // is the cause, then give the specific entry point its own flush.
  static const bool FlushEveryCall = [] {
    const char* e = getenv("FEX_X11_FLUSH_EVERY_CALL");
    return e && *e == '1';
  }();
  if (data && FlushEveryCall) {
    x11_manager.HostXFlush(data);
  }
}

// Functions returning _XDisplay* should be handled explicitly via ptr_passthrough
guest_layout<_XDisplay*> to_guest(host_layout<_XDisplay*>) = delete;



static void fexfn_impl_libGL_GL_SetGuestMalloc(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &GuestMalloc);
}

static void fexfn_impl_libGL_GL_SetGuestXGetVisualInfo(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  // Build into a temporary and publish with a release store. Other threads
  // acquire-load this (MapToGuestVisualInfo); the trampoline body and its
  // GuestcallInfo must be visible before the pointer that reaches them is.
  decltype(x11_manager.GuestXGetVisualInfo) Fn {};
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &Fn);
  __atomic_store_n(&x11_manager.GuestXGetVisualInfo, Fn, __ATOMIC_RELEASE);
}

static void fexfn_impl_libGL_GL_SetGuestXSync(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXSync);
}

static void fexfn_impl_libGL_GL_SetGuestXDisplayString(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXDisplayString);
}

#include "thunkgen_host_libGL.inl"

// FEX_FRAMELOG=<path>: per-frame log of the host-side glXSwapBuffers cadence,
// written as a MangoHud-shaped CSV ("fps,frametime,elapsed" header, one row per
// swap, frametime in ms, elapsed in s since the first swap) so Scripts and
// ~/fex-scripts/scene_stats.py read it unchanged. Exists because MangoHud's GL
// layer lives in the guest's process image and cannot see the thunked host
// swaps, so every GL Linux-lane title (RimWorld, Dex, ...) had no fps column;
// the Vulkan thunk does not need this (MangoHud's Vulkan layer is host-side).
// One clock_gettime and one fprintf per frame; the FILE* is line-buffered by
// the kernel page cache, not by us, so a SIGKILLed session keeps its rows.
// Off (no work at all beyond one branch) unless the variable is set.
namespace {
struct FrameLog {
  FILE* File {};
  bool Tried {};
  struct timespec Prev {};
  struct timespec First {};
  std::mutex Mutex;

  void Swap() {
    if (!Tried) {
      Tried = true;
      const char* Path = getenv("FEX_FRAMELOG");
      if (Path && *Path) {
        File = fopen(Path, "a");
        if (File) {
          fprintf(File, "fps,frametime,elapsed\n");
          fflush(File);
        }
      }
    }
    if (!File) {
      return;
    }
    struct timespec Now {};
    clock_gettime(CLOCK_MONOTONIC, &Now);
    if (Prev.tv_sec == 0 && Prev.tv_nsec == 0) {
      Prev = Now;
      First = Now;
      return;
    }
    const double FrameMs = (Now.tv_sec - Prev.tv_sec) * 1e3 + (Now.tv_nsec - Prev.tv_nsec) / 1e6;
    const double Elapsed = (Now.tv_sec - First.tv_sec) + (Now.tv_nsec - First.tv_nsec) / 1e9;
    Prev = Now;
    fprintf(File, "%.2f,%.4f,%.4f\n", FrameMs > 0 ? 1000.0 / FrameMs : 0.0, FrameMs, Elapsed);
    fflush(File);
  }
};
FrameLog frame_log;
} // namespace

void fexfn_impl_libGL_glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
  fexldr_ptr_libGL_glXSwapBuffers(dpy, drawable);
  static const bool Enabled = [] {
    const char* Path = getenv("FEX_FRAMELOG");
    return Path && *Path;
  }();
  if (Enabled) {
    std::lock_guard lk {frame_log.Mutex};
    frame_log.Swap();
  }
}


auto fexfn_impl_libGL_glXGetProcAddress(const GLubyte* name) -> void (*)() {
  using VoidFn = void (*)();
  std::string_view name_sv {reinterpret_cast<const char*>(name)};
  if (name_sv == "glCompileShaderIncludeARB") {
    return (VoidFn)fexfn_impl_libGL_glCompileShaderIncludeARB;
  } else if (name_sv == "glCreateShaderProgramv") {
    return (VoidFn)fexfn_impl_libGL_glCreateShaderProgramv;
  } else if (name_sv == "glGetBufferPointerv") {
    return (VoidFn)fexfn_impl_libGL_glGetBufferPointerv;
  } else if (name_sv == "glGetBufferPointervARB") {
    return (VoidFn)fexfn_impl_libGL_glGetBufferPointervARB;
  } else if (name_sv == "glGetNamedBufferPointerv") {
    return (VoidFn)fexfn_impl_libGL_glGetNamedBufferPointerv;
  } else if (name_sv == "glGetNamedBufferPointervEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetNamedBufferPointervEXT;
  } else if (name_sv == "glGetPointerv") {
    return (VoidFn)fexfn_impl_libGL_glGetPointerv;
  } else if (name_sv == "glGetPointervEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetPointervEXT;
  } else if (name_sv == "glGetPointeri_vEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetPointeri_vEXT;
  } else if (name_sv == "glGetPointerIndexedvEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetPointerIndexedvEXT;
  } else if (name_sv == "glGetVariantPointervEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetVariantPointervEXT;
  } else if (name_sv == "glGetVertexAttribPointervARB") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexAttribPointervARB;
  } else if (name_sv == "glGetVertexAttribPointerv") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexAttribPointerv;
  } else if (name_sv == "glGetVertexAttribPointervNV") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexAttribPointervNV;
  } else if (name_sv == "glGetVertexArrayPointeri_vEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexArrayPointeri_vEXT;
  } else if (name_sv == "glGetVertexArrayPointervEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexArrayPointervEXT;
  } else if (name_sv == "glShaderSource") {
    return (VoidFn)fexfn_impl_libGL_glShaderSource;
  } else if (name_sv == "glShaderSourceARB") {
    return (VoidFn)fexfn_impl_libGL_glShaderSourceARB;
    // Pointer-array widening impls. These must be listed here for the same
    // reason as glShaderSource: a title that resolves them through
    // glXGetProcAddress (Unity does) would otherwise miss the custom impl.
  } else if (name_sv == "glMultiDrawElements") {
    return (VoidFn)fexfn_impl_libGL_glMultiDrawElements;
  } else if (name_sv == "glMultiDrawElementsEXT") {
    return (VoidFn)fexfn_impl_libGL_glMultiDrawElementsEXT;
  } else if (name_sv == "glMultiDrawElementsBaseVertex") {
    return (VoidFn)fexfn_impl_libGL_glMultiDrawElementsBaseVertex;
  } else if (name_sv == "glTransformFeedbackVaryings") {
    return (VoidFn)fexfn_impl_libGL_glTransformFeedbackVaryings;
  } else if (name_sv == "glTransformFeedbackVaryingsEXT") {
    return (VoidFn)fexfn_impl_libGL_glTransformFeedbackVaryingsEXT;
  } else if (name_sv == "glXChooseFBConfig") {
    return (VoidFn)fexfn_impl_libGL_glXChooseFBConfig;
  } else if (name_sv == "glXChooseFBConfigSGIX") {
    return (VoidFn)fexfn_impl_libGL_glXChooseFBConfigSGIX;
  } else if (name_sv == "glXSwapBuffers") {
    return (VoidFn)fexfn_impl_libGL_glXSwapBuffers;
  } else if (name_sv == "glXGetCurrentDisplay") {
    return (VoidFn)fexfn_impl_libGL_glXGetCurrentDisplay;
  } else if (name_sv == "glXGetCurrentDisplayEXT") {
    return (VoidFn)fexfn_impl_libGL_glXGetCurrentDisplayEXT;
  } else if (name_sv == "glXGetFBConfigs") {
    return (VoidFn)fexfn_impl_libGL_glXGetFBConfigs;
  } else if (name_sv == "glXGetFBConfigFromVisualSGIX") {
    return (VoidFn)fexfn_impl_libGL_glXGetFBConfigFromVisualSGIX;
  } else if (name_sv == "glXGetVisualFromFBConfigSGIX") {
    return (VoidFn)fexfn_impl_libGL_glXGetVisualFromFBConfigSGIX;
  } else if (name_sv == "glXChooseVisual") {
    return (VoidFn)fexfn_impl_libGL_glXChooseVisual;
    // The XID-taking GLX entry points MUST be listed here, not just annotated
    // custom_host_impl.
    //
    // A name resolved through glXGetProcAddress does NOT reach its
    // fexfn_impl_* wrapper. libGL_Guest.cpp:68 links the *returned host
    // address* to the generic HostPtrInvokers entry for that name, so a later
    // guest call lands in GuestWrapperForHostFunction<Sig>::Call (Host.h:913),
    // which repacks the arguments and then branches straight to whatever
    // address we returned here. Return the raw host symbol and the custom impl
    // is bypassed entirely — the guest_layout conversions still happen (that is
    // why this worked at all), but everything the impl adds around the call
    // does not. Confirmed by gdb: glXMakeCurrent in libGLX.so.0 called from
    // GuestWrapperForHostFunction<int (_XDisplay*, unsigned long,
    // __GLXcontextRec*), ...>::Call, never from fexfn_impl_libGL_glXMakeCurrent.
    //
    // For these four that missing extra is GuestSyncForHostDisplay: the
    // Window/Pixmap they name was minted on the guest connection and may still
    // sit in the guest Xlib request buffer. Without the sync the host
    // connection hits BadDrawable on GLXGetDrawableAttributes (Grimrock
    // bootstrap, serials ~28/30) whenever the per-call sync in
    // GuestToHostDisplay is not covering for it — which is the DEFAULT since
    // the 2026-08-13 first-only flip (per-call is opt-in
    // FEX_X11_SYNC_EVERY_CALL=1).
  } else if (name_sv == "glXMakeCurrent") {
    return (VoidFn)fexfn_impl_libGL_glXMakeCurrent;
  } else if (name_sv == "glXMakeContextCurrent") {
    return (VoidFn)fexfn_impl_libGL_glXMakeContextCurrent;
  } else if (name_sv == "glXCreateWindow") {
    return (VoidFn)fexfn_impl_libGL_glXCreateWindow;
  } else if (name_sv == "glXCreatePixmap") {
    return (VoidFn)fexfn_impl_libGL_glXCreatePixmap;
  } else if (name_sv == "glXCreateContext") {
    return (VoidFn)fexfn_impl_libGL_glXCreateContext;
  } else if (name_sv == "glXCreateGLXPixmap") {
    return (VoidFn)fexfn_impl_libGL_glXCreateGLXPixmap;
  } else if (name_sv == "glXCreateGLXPixmapMESA") {
    return (VoidFn)fexfn_impl_libGL_glXCreateGLXPixmapMESA;
  } else if (name_sv == "glXGetConfig") {
    return (VoidFn)fexfn_impl_libGL_glXGetConfig;
  } else if (name_sv == "glXGetVisualFromFBConfig") {
    return (VoidFn)fexfn_impl_libGL_glXGetVisualFromFBConfig;
  }
  // FEX_LIBGL_DEBUG=1: report every name that resolves to null. A title whose
  // GL init silently fails usually does so because one entry point it needs was
  // never thunked, and it rarely says which — Psychonauts printed "Missing
  // required OpenGL extensions" and eON prints only "failed to initialise".
  // This turns that into a list of names to add.
  auto Result = (VoidFn)glXGetProcAddress((const GLubyte*)name);
  if (!Result && FexLibGLDebug()) {
    fprintf(stderr, "[fex-libGL] glXGetProcAddress MISS: %s\n", name_sv.data());
    fflush(stderr);
  }
  return Result;
}

// TODO: unsigned int *glXEnumerateVideoDevicesNV (Display *dpy, int screen, int *nelements);


void fexfn_impl_libGL_glCompileShaderIncludeARB(GLuint a_0, GLsizei Count, guest_layout<const GLchar* const*> a_2, const GLint* a_3) {
  auto sources = a_2.force_get_host_pointer();
  return fexldr_ptr_libGL_glCompileShaderIncludeARB(a_0, Count, sources, a_3);
}

GLuint fexfn_impl_libGL_glCreateShaderProgramv(GLuint a_0, GLsizei count, guest_layout<const GLchar* const*> a_2) {
  auto sources = a_2.force_get_host_pointer();
  return fexldr_ptr_libGL_glCreateShaderProgramv(a_0, count, sources);
}


void fexfn_impl_libGL_glGetBufferPointerv(GLenum a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetBufferPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetBufferPointervARB(GLenum a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetBufferPointervARB(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetNamedBufferPointerv(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetNamedBufferPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetNamedBufferPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetNamedBufferPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointerv(GLenum a_0, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetPointerv(a_0, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointervEXT(GLenum a_0, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetPointervEXT(a_0, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointeri_vEXT(GLenum a_0, GLuint a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetPointeri_vEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointerIndexedvEXT(GLenum a_0, GLuint a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetPointerIndexedvEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVariantPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVariantPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointervARB(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointervARB(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointerv(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointervNV(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointervNV(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexArrayPointeri_vEXT(GLuint a_0, GLuint a_1, GLenum a_2, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexArrayPointeri_vEXT(a_0, a_1, a_2, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexArrayPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexArrayPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glShaderSource(GLuint a_0, GLsizei count, guest_layout<const GLchar* const*> a_2, const GLint* a_3) {
  auto sources = a_2.force_get_host_pointer();
  return fexldr_ptr_libGL_glShaderSource(a_0, count, sources, a_3);
}

// glMultiDrawElements / glTransformFeedbackVaryings family.
//
// All of these take an array of pointers whose elements are guest-width on a
// 32-bit guest (4 bytes) and host-width here (8 bytes), so the array has to be
// rebuilt element by element rather than passed through. Identical in shape to
// glShaderSource above; the 64-bit path is the same straight passthrough these
// functions had before, so behaviour there is unchanged.
//
// alloca matches glShaderSource's existing approach. The counts are draw-call
// batch sizes and varying counts (tens, not thousands), and they come from the
// guest's own render loop rather than from untrusted input.
void fexfn_impl_libGL_glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, guest_layout<const void* const*> a_3,
                                          GLsizei drawcount) {
  auto indices = a_3.force_get_host_pointer();
  return fexldr_ptr_libGL_glMultiDrawElements(mode, count, type, indices, drawcount);
}

void fexfn_impl_libGL_glMultiDrawElementsEXT(GLenum mode, const GLsizei* count, GLenum type, guest_layout<const void* const*> a_3,
                                             GLsizei drawcount) {
  auto indices = a_3.force_get_host_pointer();
  return fexldr_ptr_libGL_glMultiDrawElementsEXT(mode, count, type, indices, drawcount);
}

void fexfn_impl_libGL_glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, guest_layout<const void* const*> a_3,
                                                    GLsizei drawcount, const GLint* basevertex) {
  auto indices = a_3.force_get_host_pointer();
  return fexldr_ptr_libGL_glMultiDrawElementsBaseVertex(mode, count, type, indices, drawcount, basevertex);
}

void fexfn_impl_libGL_glTransformFeedbackVaryings(GLuint program, GLsizei count, guest_layout<const GLchar* const*> a_2, GLenum bufferMode) {
  auto varyings = a_2.force_get_host_pointer();
  return fexldr_ptr_libGL_glTransformFeedbackVaryings(program, count, varyings, bufferMode);
}

void fexfn_impl_libGL_glTransformFeedbackVaryingsEXT(GLuint program, GLsizei count, guest_layout<const GLchar* const*> a_2, GLenum bufferMode) {
  auto varyings = a_2.force_get_host_pointer();
  return fexldr_ptr_libGL_glTransformFeedbackVaryingsEXT(program, count, varyings, bufferMode);
}

// ---------------------------------------------------------------------------
// Buffer object mapping
// ---------------------------------------------------------------------------
//
// glMapBuffer hands back a pointer into the driver's mapping. That pointer is a
// host VA (0x3fff'xxxx'xxxx here) and cannot be represented in a 32-bit guest
// slot, so on 32-bit the guest gets a staging buffer in guest memory instead:
//   map:   host-map, then copy host -> staging ALWAYS (see MapBufferToGuest
//          for why this cannot be conditional on the access flags)
//   unmap: copy staging -> host if the access allows writing, then host-unmap
//
// Excluding these instead (as this thunk briefly did) is not an option: a title
// that needs GL_ARB_vertex_buffer_object simply refuses to start.
//
// Staging buffers are cached per target and grown as needed rather than freed.
// Only GuestMalloc is registered - there is no guest free() - so allocating per
// map would leak the guest's 4 GiB heap within minutes of gameplay. A game uses
// a bounded set of targets, so the cache is bounded too.
//
// GL_MAP_FLUSH_EXPLICIT_BIT needs the flush itself to be intercepted, and this
// is why: glFlushMappedBufferRange is the point at which the driver takes the
// mapped range's current contents. Under the staging scheme the guest's writes
// are in the staging buffer, and nothing has copied them into the driver's
// mapping yet — the copy used to happen only at unmap, strictly *after* every
// flush. So the driver latched the range as it stood at map time, i.e. the
// seeded (previous) contents, and for a FLUSH_EXPLICIT mapping the unmap copy
// that followed was not required to be uploaded at all: GL only promises to
// upload sub-ranges that were explicitly flushed.
//
// That is one frame's geometry surviving into the next — the same visible
// failure the unconditional seeding above was written for, which seeding could
// only change from "random garbage" into "last frame's contents".
//
// So the four flush entry points get custom impls on 32-bit that copy the
// flushed sub-range staging -> host mapping before forwarding. The full
// copy-back at unmap stays: it is what serves mappings *without*
// FLUSH_EXPLICIT, and for a flushed mapping it rewrites bytes that already
// match. Copying back only the flushed sub-ranges is a bandwidth optimisation
// that can come later; unlike this, it is not a correctness matter.

void fexfn_impl_libGL_glShaderSourceARB(GLuint a_0, GLsizei count, guest_layout<const GLcharARB**> a_2, const GLint* a_3) {
  auto sources = a_2.force_get_host_pointer();
  return fexldr_ptr_libGL_glShaderSourceARB(a_0, count, sources, a_3);
}

// Relocate data to guest heap so it can be called with XFree.
// The memory at the given host location will be de-allocated.
template<typename T>
guest_layout<T*> RelocateArrayToGuestHeap(T* Data, int NumItems) {
  if (!Data) {
    return guest_layout<T*> {.data = 0};
  }

  if (NumItems <= 0) {
    return guest_layout<T*> {.data = 0};
  }

  guest_layout<T*> GuestData;
  GuestData.data = reinterpret_cast<uintptr_t>(GuestMalloc(sizeof(guest_layout<T>) * NumItems));
  if (!GuestData.data) {
    // Guest heap exhausted. Without this check the loop below writes the
    // relocated array through a null guest pointer.
    return guest_layout<T*> {.data = 0};
  }
  for (int Index = 0; Index < NumItems; ++Index) {
    GuestData.get_pointer()[Index] = to_guest(to_host_layout(Data[Index]));
  }
  x11_manager.HostXFree(Data);
  return GuestData;
}


// Maps to a host-side XVisualInfo, which must be XFree'ed by the caller.
static XVisualInfo* LookupHostVisualInfo(Display* HostDisplay, guest_layout<XVisualInfo*> GuestInfo) {
  if (!GuestInfo.data) {
    return nullptr;
  }

  int num_matches;
  auto HostInfo = host_layout<XVisualInfo> {*GuestInfo.get_pointer()}.data;
  auto ret = x11_manager.HostXGetVisualInfo(HostDisplay, uint64_t {VisualScreenMask | VisualIDMask}, &HostInfo, &num_matches);
  if (num_matches != 1) {
    fprintf(stderr, "ERROR: Did not find unique host XVisualInfo\n");
    std::abort();
  }
  return ret;
}

// Maps to a guest-side XVisualInfo and destroys the host argument.
static guest_layout<XVisualInfo*> MapToGuestVisualInfo(Display* HostDisplay, XVisualInfo* HostInfo) {
  if (!HostInfo) {
    return guest_layout<XVisualInfo*> {.data = 0};
  }

  // This buffer must come from the *guest* heap.
  //
  // 41d9771a1 switched it to host std::malloc() on the reasoning that the
  // guest's XFree() would route back through the thunked XFree -> HostXFree ->
  // host free(). That is not what happens: libGL-guest.so has a DT_NEEDED on
  // the guest's own libX11.so.6, so XFree() is resolved inside the guest and
  // calls the *guest* allocator's free() on a host-heap pointer. Guest glibc
  // then walks a chunk header that was never its own and aborts with
  //   double free or corruption (out)
  // right after glXCreateContext. Whether it aborts at all depends only on
  // what the host pointer happens to alias, so it looked intermittent and got
  // misfiled as guest/host thunk interface drift.
  //
  // The guest-callback path that made GuestMalloc unusable back in May was
  // fixed afterwards (f34dbdb9d, 62ea24ce4, b21ee0205, 58973e69e), and
  // RelocateArrayToGuestHeap has been using GuestMalloc successfully since.
  // Use it here too - a single-element relocation is exactly what it does.
  //
  // ...but a byte-for-byte relocation is not enough, because XVisualInfo has a
  // `Visual* visual` member that points into the *host* Xlib's connection
  // state. Relocating the struct converts the layout and truncates that
  // member, and the guest then hands it to its own libX11: XCreateColormap
  // dereferences visual->visualid and segfaults. Dex died exactly there once
  // FBConfig selection started working.
  //
  // A Visual belongs to the connection that produced it, so the only correct
  // answer is one minted by the *guest's* Xlib. Re-query it there by screen +
  // visualid, which is the mirror of what LookupHostVisualInfo already does in
  // the other direction. GuestXGetVisualInfo has been registered for this
  // since the X11Manager was written but had no caller until now.
  //
  // The result is guest-allocated, so the guest's XFree owns it - which is
  // also what the caller of glXGetVisualFromFBConfig expects.
  // Acquire-load the callback pointer. It is published by a different thread
  // (whichever one ran the guest lib's OnInit) and publishing it also writes
  // trampoline bytes and a GuestcallInfo the callee dereferences. ppc64le is
  // weakly ordered, so without this a reader can observe a non-null pointer
  // while the memory behind it is not yet visible, and branch into garbage.
  auto* GuestGetVisualInfo = __atomic_load_n(&x11_manager.GuestXGetVisualInfo, __ATOMIC_ACQUIRE);
  auto* Malloc = __atomic_load_n(&GuestMalloc, __ATOMIC_ACQUIRE);

  if (GuestGetVisualInfo && Malloc) {
    auto GuestDisplay = x11_manager.HostToGuestDisplay(HostDisplay);
    if (GuestDisplay.data) {
      // Both the template and the out-count must live in guest memory: the
      // callback runs as guest code and cannot write to a host address.
      //
      // There is no guest free() registered here - only GuestMalloc - so a
      // per-call allocation would leak guest heap on every call, and a 32-bit
      // guest only has 4GiB of it. One buffer per thread, allocated once and
      // reused, keeps this bounded. It is thread_local because the callback
      // writes through it and concurrent callers must not share.
      constexpr size_t TemplateSize = sizeof(guest_layout<XVisualInfo>);
      static thread_local uint8_t* Scratch = nullptr;
      if (!Scratch) {
        Scratch = static_cast<uint8_t*>(Malloc(TemplateSize + sizeof(int)));
      }
      if (Scratch) {
        auto* Template = reinterpret_cast<guest_layout<XVisualInfo>*>(Scratch);
        auto* NumItems = reinterpret_cast<int*>(Scratch + TemplateSize);
        // Zero `visual` in the query template. Only screen and visualid are
        // consulted (see the mask passed below), but to_guest converts every
        // member — including the host Visual* at 0x3fff'xxxx'xxxx, which does
        // not fit the guest struct's 32-bit slot. That narrowing is what the
        // truncation guard in Host.h aborts on, and it fired here on Dex
        // before the re-query could even run.
        XVisualInfo Query = *HostInfo;
        Query.visual = nullptr;
        *Template = to_guest(to_host_layout(Query));
        *NumItems = 0;

        auto* Ret = GuestGetVisualInfo(reinterpret_cast<void*>(static_cast<uintptr_t>(GuestDisplay.data)),
                                       static_cast<guest_long>(VisualScreenMask | VisualIDMask), Template, NumItems);
        if (FexLibGLDebug()) {
          fprintf(stderr, "[fex-libGL] MapToGuestVisualInfo: re-query %s (visualid=0x%lx screen=%d n=%d)\n", (Ret && *NumItems >= 1) ? "HIT" : "MISS",
                  (unsigned long)HostInfo->visualid, HostInfo->screen, *NumItems);
          fflush(stderr);
        }
        if (Ret && *NumItems >= 1) {
          x11_manager.HostXFree(HostInfo);
          return guest_layout<XVisualInfo*> {.data = static_cast<decltype(guest_layout<XVisualInfo*>::data)>(reinterpret_cast<uintptr_t>(Ret))};
        }
        // Fall through to the relocating path on a miss rather than failing
        // the call: a caller that only reads depth/class still works, and a
        // hard failure here would regress titles that never touch `visual`.
      }
    }
  }


  return RelocateArrayToGuestHeap(HostInfo, 1);
}

guest_layout<GLXFBConfig*> fexfn_impl_libGL_glXChooseFBConfig(Display* Display, int Screen, const int* Attributes, int* NumItems) {
  auto ret = fexldr_ptr_libGL_glXChooseFBConfig(Display, Screen, Attributes, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

guest_layout<GLXFBConfigSGIX*> fexfn_impl_libGL_glXChooseFBConfigSGIX(Display* Display, int Screen, int* Attributes, int* NumItems) {
  auto ret = fexldr_ptr_libGL_glXChooseFBConfigSGIX(Display, Screen, Attributes, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplay() {
  auto ret = fexldr_ptr_libGL_glXGetCurrentDisplay();
  return x11_manager.HostToGuestDisplay(ret);
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplayEXT() {
  auto ret = fexldr_ptr_libGL_glXGetCurrentDisplayEXT();
  return x11_manager.HostToGuestDisplay(ret);
}

guest_layout<GLXFBConfig*> fexfn_impl_libGL_glXGetFBConfigs(Display* Display, int Screen, int* NumItems) {
  auto ret = fexldr_ptr_libGL_glXGetFBConfigs(Display, Screen, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

GLXFBConfigSGIX fexfn_impl_libGL_glXGetFBConfigFromVisualSGIX(Display* Display, guest_layout<XVisualInfo*> Info) {
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXGetFBConfigFromVisualSGIX(Display, HostInfo);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfigSGIX(Display* Display, GLXFBConfigSGIX Config) {
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXGetVisualFromFBConfigSGIX(Display, Config));
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXChooseVisual(Display* Display, int Screen, int* Attributes) {
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXChooseVisual(Display, Screen, Attributes));
}

void fexfn_impl_libGL_glXDestroyContext(Display* Display, GLXContext Context) {
  fexldr_ptr_libGL_glXDestroyContext(Display, Context);
}

GLXContext fexfn_impl_libGL_glXCreateContext(Display* Display, guest_layout<XVisualInfo*> Info, GLXContext ShareList, Bool Direct) {
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  if (FexLibGLDebug()) {
    fprintf(stderr, "[fex-libGL] glXCreateContext: display=%p visualid=0x%lx screen=%d direct=%d\n",
            Display, (unsigned long)(HostInfo ? HostInfo->visualid : 0), HostInfo ? HostInfo->screen : -1, Direct);
  }
  auto ret = fexldr_ptr_libGL_glXCreateContext(Display, HostInfo, ShareList, Direct);
  if (FexLibGLDebug()) {
    fprintf(stderr, "[fex-libGL] glXCreateContext: returned ctx=%p\n", (void*)ret);
  }
  x11_manager.HostXFree(HostInfo);
  return ret;
}

Bool fexfn_impl_libGL_glXMakeCurrent(Display* Display, GLXDrawable Drawable, GLXContext Context) {
  // XID args may name a guest-created drawable the host connection has not
  // seen yet (see X11Manager::GuestSyncForHostDisplay). Rare call; cheap here.
  x11_manager.GuestSyncForHostDisplay(Display);
  if (FexLibGLDebug()) {
    fprintf(stderr, "[fex-libGL] glXMakeCurrent: display=%p drawable=0x%lx context=%p\n",
            Display, (unsigned long)Drawable, (void*)Context);
  }
  auto ret = fexldr_ptr_libGL_glXMakeCurrent(Display, Drawable, Context);
  if (FexLibGLDebug()) {
    fprintf(stderr, "[fex-libGL] glXMakeCurrent: returned %d\n", ret);
  }
  return ret;
}

Bool fexfn_impl_libGL_glXMakeContextCurrent(Display* Display, GLXDrawable Draw, GLXDrawable Read, GLXContext Context) {
  // XID args may name a guest-created drawable the host connection has not
  // seen yet (see X11Manager::GuestSyncForHostDisplay). Rare call; cheap here.
  x11_manager.GuestSyncForHostDisplay(Display);
  if (FexLibGLDebug()) {
    fprintf(stderr, "[fex-libGL] glXMakeContextCurrent: display=%p draw=0x%lx read=0x%lx context=%p\n",
            Display, (unsigned long)Draw, (unsigned long)Read, (void*)Context);
  }
  auto ret = fexldr_ptr_libGL_glXMakeContextCurrent(Display, Draw, Read, Context);
  if (FexLibGLDebug()) {
    fprintf(stderr, "[fex-libGL] glXMakeContextCurrent: returned %d\n", ret);
  }
  return ret;
}

GLXWindow fexfn_impl_libGL_glXCreateWindow(Display* Display, GLXFBConfig Config, Window Win, const int* AttribList) {
  // `Win` was created on the guest connection; sync it visible to the host
  // connection first. This is the path SDL2/GLX-1.3 titles take INSTEAD of
  // glXMakeCurrent-first (Grimrock: BadDrawable on GLXGetDrawableAttributes,
  // "failed to create drawable", before any MakeCurrent).
  x11_manager.GuestSyncForHostDisplay(Display);
  if (FexLibGLDebug()) {
    fprintf(stderr, "[fex-libGL] glXCreateWindow: display=%p win=0x%lx\n", Display, (unsigned long)Win);
  }
  return fexldr_ptr_libGL_glXCreateWindow(Display, Config, Win, AttribList);
}

GLXPixmap fexfn_impl_libGL_glXCreatePixmap(Display* Display, GLXFBConfig Config, Pixmap Pixmap, const int* AttribList) {
  x11_manager.GuestSyncForHostDisplay(Display);
  return fexldr_ptr_libGL_glXCreatePixmap(Display, Config, Pixmap, AttribList);
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmap(Display* Display, guest_layout<XVisualInfo*> Info, Pixmap Pixmap) {
  x11_manager.GuestSyncForHostDisplay(Display);
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXCreateGLXPixmap(Display, HostInfo, Pixmap);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmapMESA(Display* Display, guest_layout<XVisualInfo*> Info, Pixmap Pixmap, Colormap Colormap) {
  x11_manager.GuestSyncForHostDisplay(Display);
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXCreateGLXPixmapMESA(Display, HostInfo, Pixmap, Colormap);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

int fexfn_impl_libGL_glXGetConfig(Display* Display, guest_layout<XVisualInfo*> Info, int Attribute, int* Value) {
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXGetConfig(Display, HostInfo, Attribute, Value);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfig(Display* Display, GLXFBConfig Config) {
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXGetVisualFromFBConfig(Display, Config));
}


EXPORTS(libGL)
