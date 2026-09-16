#include <common/GeneratorInterface.h>

#include <xf86drm.h>

template<auto>
struct fex_gen_config {
  unsigned version = 2;
};

template<typename>
struct fex_gen_type {};

// Function, parameter index, parameter type [optional]
template<auto, int, typename = void>
struct fex_gen_param {};

// On a 64-bit guest, pointers are the same width on both sides of the thunk and guest
// and host share the same virtual address space. That makes every one of the types
// below "bit compatible" as-is: no member needs repacking, and pointers found inside
// these structs can be dereferenced by the host directly without any translation.
// None of this holds on a 32-bit guest (4-byte guest pointers vs 8-byte host pointers,
// so the very shape of these structs differs) -- see the 32-bit-specific handling
// further down for each of these types.
template<>
struct fex_gen_type<drmDevice> : fexgen::assume_compatible_data_layout {};

// Anonymous sub-structs
template<>
struct fex_gen_type<drmStatsT> : fexgen::assume_compatible_data_layout {};

// TODO: Convert vtable
template<>
struct fex_gen_type<drmServerInfo> : fexgen::assume_compatible_data_layout {};
template<>
struct fex_gen_type<drmEventContext> : fexgen::assume_compatible_data_layout {};

// 64-bit only: the guest-side wrappers for the caller-frees string returns
// (drmGetDeviceNameFromFd & co) copy the host allocation onto the guest heap
// via its malloc_usable_size and then release it host-side. On 32-bit the
// host pointer cannot even reach the guest, so the relocation happens in the
// custom host impls instead and this pair is not needed.
size_t FEX_usable_size(void*);
void FEX_free_on_host(void*);

template<>
struct fex_gen_config<FEX_usable_size> : fexgen::custom_host_impl, fexgen::custom_guest_entrypoint {};
template<>
struct fex_gen_config<FEX_free_on_host> : fexgen::custom_host_impl, fexgen::custom_guest_entrypoint {};
// drmIoctl's payload crosses as-is (void*, shared address space). On a 32-bit
// guest this is sound for the ioctls Mesa actually issues directly -- the
// driver-specific amdgpu/radeon/GEM/syncobj/prime ioctls are fixed-width and
// 64-bit-aligned by kernel-ABI design, so i386 and x86_64 layouts agree. The
// legacy core ioctls whose structs genuinely differ (VERSION, GET_UNIQUE,
// MAP...) are reached through the libdrm wrappers thunked below, which the
// host rebuilds natively.
template<>
struct fex_gen_config<drmIoctl> {};
// The libdrm-internal hash table is a host-heap object: drmGetHashTable
// returns a raw host pointer and drmHashEntry carries a function pointer plus
// a tag-table pointer. A 32-bit guest slot cannot hold either, so both stay
// 64-bit only (nothing in the lib32 Mesa stack imports them).
template<>
struct fex_gen_config<drmGetHashTable> {};
template<>
struct fex_gen_config<drmGetEntry> {};
template<>
struct fex_gen_config<drmAvailable> {};
template<>
struct fex_gen_config<drmOpen> {};
template<>
struct fex_gen_config<drmOpenWithType> {};
template<>
struct fex_gen_config<drmOpenControl> {};
template<>
struct fex_gen_config<drmOpenRender> {};
template<>
struct fex_gen_config<drmClose> {};
template<>
struct fex_gen_config<drmGetVersion> {};
template<>
struct fex_gen_config<drmGetLibVersion> {};
template<>
struct fex_gen_config<drmGetCap> {};
template<>
struct fex_gen_config<drmFreeVersion> {};
template<>
struct fex_gen_config<drmGetMagic> {};
template<>
struct fex_gen_config<drmGetBusid> {};
template<>
struct fex_gen_config<drmGetInterruptFromBusID> {};
template<>
struct fex_gen_config<drmGetMap> {};
template<>
struct fex_gen_config<drmGetClient> {};
template<>
struct fex_gen_config<drmGetStats> {};
template<>
struct fex_gen_config<drmSetInterfaceVersion> {};
template<>
struct fex_gen_config<drmCommandNone> {};
template<>
struct fex_gen_config<drmCommandRead> {};
template<>
struct fex_gen_config<drmCommandWrite> {};
template<>
struct fex_gen_config<drmCommandWriteRead> {};
template<>
struct fex_gen_config<drmFreeBusid> {};
template<>
struct fex_gen_config<drmSetBusid> {};
template<>
struct fex_gen_config<drmAuthMagic> {};
template<>
struct fex_gen_config<drmAddMap> {};
template<>
struct fex_gen_config<drmRmMap> {};
template<>
struct fex_gen_config<drmAddContextPrivateMapping> {};
template<>
struct fex_gen_config<drmAddBufs> {};
template<>
struct fex_gen_config<drmMarkBufs> {};
template<>
struct fex_gen_config<drmCreateContext> {};
template<>
struct fex_gen_config<drmSetContextFlags> {};
template<>
struct fex_gen_config<drmGetContextFlags> {};
template<>
struct fex_gen_config<drmAddContextTag> {};
template<>
struct fex_gen_config<drmDelContextTag> {};
// drmGetContextTag's void** out-parameter would have the host write an 8-byte
// pointer through a 4-byte guest slot; drmGetReservedContextList returns a
// host drmMalloc'd array its Free partner must see again. All DRI1
// master/X-server API, unreachable from the lib32 Mesa stack. Excluded on
// 32-bit (drmAddContextTag/drmDelContextTag above stay: they only store a
// value).
template<>
struct fex_gen_config<drmGetContextTag> {};
template<>
struct fex_gen_config<drmGetReservedContextList> {};
template<>
struct fex_gen_config<drmFreeReservedContextList> {};
template<>
struct fex_gen_config<drmSwitchToContext> {};
template<>
struct fex_gen_config<drmDestroyContext> {};
template<>
struct fex_gen_config<drmCreateDrawable> {};
template<>
struct fex_gen_config<drmDestroyDrawable> {};
template<>
struct fex_gen_config<drmUpdateDrawableInfo> {};
template<>
struct fex_gen_config<drmCtlInstHandler> {};
template<>
struct fex_gen_config<drmCtlUninstHandler> {};
template<>
struct fex_gen_config<drmSetClientCap> {};
template<>
struct fex_gen_config<drmCrtcGetSequence> {};
template<>
struct fex_gen_config<drmCrtcQueueSequence> {};
// The DRI1 mapping surface cannot cross the 32-bit boundary: drmMap writes a
// host mapping address through a void** out-parameter (8 bytes into a 4-byte
// guest slot, and the VA does not fit 32 bits anyway), and
// drmGetBufInfo/drmMapBufs return host-heap structs whose `list` members
// point at further host structs carrying mapped-buffer addresses. Nothing in
// the lib32 Mesa DRI3 stack imports any of these. Excluded on 32-bit; a
// missing symbol makes the one guest that wants it fail visibly at load
// instead of corrupting silently.
template<>
struct fex_gen_config<drmMap> {};
template<>
struct fex_gen_config<drmUnmap> {};
template<>
struct fex_gen_config<drmGetBufInfo> {};
template<>
struct fex_gen_config<drmMapBufs> {};
template<>
struct fex_gen_config<drmUnmapBufs> {};
template<>
struct fex_gen_config<drmDMA> {};
template<>
struct fex_gen_config<drmFreeBufs> {};
template<>
struct fex_gen_config<drmGetLock> {};
template<>
struct fex_gen_config<drmUnlock> {};
template<>
struct fex_gen_config<drmFinish> {};
template<>
struct fex_gen_config<drmGetContextPrivateMapping> {};
template<>
struct fex_gen_config<drmScatterGatherAlloc> {};
template<>
struct fex_gen_config<drmScatterGatherFree> {};
template<>
struct fex_gen_config<drmWaitVBlank> {};

template<>
struct fex_gen_config<drmSetServerInfo> {};
template<>
struct fex_gen_config<drmError> {};
// drmMalloc/drmHashCreate/drmRandomCreate/drmSLCreate all hand the guest a
// raw host-heap pointer (as void*), which does not fit a 32-bit guest slot;
// the rest of each family consumes those handles. Utility API for DDX
// drivers, unused by the lib32 Mesa stack. Excluded on 32-bit; if a consumer
// ever appears, the fix is an opaque-handle token registry as in
// libxshmfence/libGL, not a data-layout annotation.
template<>
struct fex_gen_config<drmMalloc> {};
template<>
struct fex_gen_config<drmFree> {};
template<>
struct fex_gen_config<drmHashCreate> {};
template<>
struct fex_gen_config<drmHashDestroy> {};
template<>
struct fex_gen_config<drmHashLookup> {};
template<>
struct fex_gen_config<drmHashInsert> {};
template<>
struct fex_gen_config<drmHashDelete> {};
template<>
struct fex_gen_config<drmHashFirst> {};
template<>
struct fex_gen_config<drmHashNext> {};
template<>
struct fex_gen_config<drmRandomCreate> {};
template<>
struct fex_gen_config<drmRandomDestroy> {};
template<>
struct fex_gen_config<drmRandom> {};
template<>
struct fex_gen_config<drmRandomDouble> {};
template<>
struct fex_gen_config<drmSLCreate> {};
template<>
struct fex_gen_config<drmSLDestroy> {};
template<>
struct fex_gen_config<drmSLLookup> {};
template<>
struct fex_gen_config<drmSLInsert> {};
template<>
struct fex_gen_config<drmSLDelete> {};
template<>
struct fex_gen_config<drmSLNext> {};
template<>
struct fex_gen_config<drmSLFirst> {};
template<>
struct fex_gen_config<drmSLDump> {};
template<>
struct fex_gen_config<drmSLLookupNeighbors> {};
template<>
struct fex_gen_config<drmOpenOnce> {};
template<>
struct fex_gen_config<drmOpenOnceWithType> {};
template<>
struct fex_gen_config<drmCloseOnce> {};
template<>
struct fex_gen_config<drmSetMaster> {};
template<>
struct fex_gen_config<drmDropMaster> {};
template<>
struct fex_gen_config<drmIsMaster> {};
template<>
struct fex_gen_config<drmHandleEvent> {};
// The caller-frees string family. On 64-bit the guest wrapper copies the host
// allocation onto the guest heap (FEX_usable_size dance in Guest.cpp) so the
// guest's free() owns the result. On 32-bit the custom host impl relocates the
// string onto the guest heap directly -- the host pointer would not even fit
// the packed return slot -- and the guest wrapper is a plain pack call.
template<>
struct fex_gen_config<drmGetDeviceNameFromFd> : fexgen::custom_guest_entrypoint {};
template<>
struct fex_gen_config<drmGetDeviceNameFromFd2> : fexgen::custom_guest_entrypoint {};

template<>
struct fex_gen_config<drmGetNodeTypeFromFd> {};
template<>
struct fex_gen_config<drmPrimeHandleToFD> {};
template<>
struct fex_gen_config<drmPrimeFDToHandle> {};
// Modern GEM handle close; lib32/lib64 Mesa gallium binds it at load (BIND_NOW).
template<>
struct fex_gen_config<drmCloseBufferHandle> {};
template<>
struct fex_gen_config<drmGetPrimaryDeviceNameFromFd> : fexgen::custom_guest_entrypoint {};
template<>
struct fex_gen_config<drmGetRenderDeviceNameFromFd> : fexgen::custom_guest_entrypoint {};
// Returns a malloc'd string the caller frees with free(); same relocation
// pattern as drmGetDeviceNameFromFd. Mesa gallium binds it at load.
template<>
struct fex_gen_config<drmGetFormatModifierName> : fexgen::custom_guest_entrypoint {};

template<>
struct fex_gen_config<drmGetDevice> {};
template<>
struct fex_gen_config<drmFreeDevice> {};
template<>
struct fex_gen_config<drmGetDevices> {};
template<>
struct fex_gen_config<drmFreeDevices> {};
template<>
struct fex_gen_config<drmGetDevice2> {};
template<>
struct fex_gen_config<drmGetDevices2> {};
// EGL and radv resolve device-by-dev_t through this at load.
template<>
struct fex_gen_config<drmGetDeviceFromDevId> {};
template<>
struct fex_gen_config<drmDevicesEqual> {};
template<>
struct fex_gen_config<drmSyncobjCreate> {};
template<>
struct fex_gen_config<drmSyncobjDestroy> {};
template<>
struct fex_gen_config<drmSyncobjHandleToFD> {};
template<>
struct fex_gen_config<drmSyncobjFDToHandle> {};
template<>
struct fex_gen_config<drmSyncobjImportSyncFile> {};
template<>
struct fex_gen_config<drmSyncobjExportSyncFile> {};
template<>
struct fex_gen_config<drmSyncobjWait> {};
template<>
struct fex_gen_config<drmSyncobjReset> {};
template<>
struct fex_gen_config<drmSyncobjSignal> {};
template<>
struct fex_gen_config<drmSyncobjTimelineSignal> {};
template<>
struct fex_gen_config<drmSyncobjTimelineWait> {};
template<>
struct fex_gen_config<drmSyncobjQuery> {};
template<>
struct fex_gen_config<drmSyncobjQuery2> {};
template<>
struct fex_gen_config<drmSyncobjTransfer> {};
