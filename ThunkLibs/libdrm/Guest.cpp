/*
$info$
tags: thunklibs|drm
$end_info$
*/

#include <xf86drm.h>

#include <stdio.h>
#include <cstdlib>
#include <cstring>

#include "common/Guest.h"
#include <stdarg.h>

#include "thunkgen_guest_libdrm.inl"

extern "C" {

void FEX_malloc_free_on_host(void* Ptr) {
  struct {
    void* p;
  } args;
  args.p = Ptr;
  fexthunks_libdrm_FEX_free_on_host(&args);
}

size_t FEX_malloc_usable_size(void* Ptr) {
  struct {
    void* p;
    size_t rv;
  } args;
  args.p = Ptr;
  fexthunks_libdrm_FEX_usable_size(&args);
  return args.rv;
}

void drmMsg(const char* format, ...) {
  va_list ap;
  if (1) {
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
  }
}


// The caller-frees string family: the host hands back its own heap pointer.
// The guest's free() must own the result (that is the libdrm API contract for
// these), so copy it onto the guest heap and release the host allocation.
static char* RelocateToGuestHeap(char* ret) {
  if (ret) {
    // Usable size
    size_t Usable = FEX_malloc_usable_size(ret);

    // This will be a bit wasteful but this is an unsized pointer
    void* NewPtr = malloc(Usable);
    memcpy(NewPtr, ret, Usable);

    FEX_malloc_free_on_host(ret);
    ret = (char*)NewPtr;
  }

  return ret;
}

char* drmGetDeviceNameFromFd(int a_0) {
  return RelocateToGuestHeap(fexfn_pack_drmGetDeviceNameFromFd(a_0));
}

char* drmGetDeviceNameFromFd2(int a_0) {
  return RelocateToGuestHeap(fexfn_pack_drmGetDeviceNameFromFd2(a_0));
}

char* drmGetPrimaryDeviceNameFromFd(int a_0) {
  return RelocateToGuestHeap(fexfn_pack_drmGetPrimaryDeviceNameFromFd(a_0));
}

char* drmGetRenderDeviceNameFromFd(int a_0) {
  return RelocateToGuestHeap(fexfn_pack_drmGetRenderDeviceNameFromFd(a_0));
}

char* drmGetFormatModifierName(uint64_t a_0) {
  return RelocateToGuestHeap(fexfn_pack_drmGetFormatModifierName(a_0));
}

}

LOAD_LIB(libdrm)
