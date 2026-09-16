/*
$info$
tags: thunklibs|xshmfence
$end_info$
*/

#include <stdio.h>

#include <X11/xshmfence.h>

#include "common/Host.h"
#include <dlfcn.h>


#include "thunkgen_host_libxshmfence.inl"

EXPORTS(libxshmfence)

// See the custom_host_impl comment on xshmfence_unmap_shm in
// libxshmfence_interface.cpp: on a 32-bit guest, retire the token before the
// mapping goes away so a stale guest-held token resolves to null (see
// OpaqueHandleRegistry::ForToken) instead of a freed/reused host pointer.
// On 64-bit there is no token registry - struct xshmfence* is a plain
// passthrough pointer - so this is identical to the pre-existing
// (non-custom) generated call.
static void fexfn_impl_libxshmfence_xshmfence_unmap_shm(struct xshmfence* f) {
  fexldr_ptr_libxshmfence_xshmfence_unmap_shm(f);
}
