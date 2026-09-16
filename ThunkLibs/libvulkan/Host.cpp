/*
$info$
tags: thunklibs|Vulkan
$end_info$
*/

#define VK_USE_64_BIT_PTR_DEFINES 0

#define VK_USE_PLATFORM_XLIB_XRANDR_EXT
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <FEXCore/Utils/TypeDefines.h>
#include <vulkan/vulkan.h>

#include "common/Host.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdarg>
#include <cstring>
#include <map>
#include <sys/mman.h>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
// The guest's Vulkan loader configuration must never reach the host loader.
//
// These variables name ICD/layer *manifests* and the shared objects they
// point at. Under a thunk those always describe guest-architecture files, so
// letting the host loader read them makes it try to load x86 objects into a
// PPC64LE process. It cannot, and reports the failure as though no usable
// driver existed at all.
//
// This was not hypothetical. Steam's pressure-vessel exports
//   VK_DRIVER_FILES=.../radeon_icd.i686.json:.../amd_icd32.json:...
// (i686 only, because those are the ICD manifests present in the FEX RootFS),
// the host loader found nothing loadable in that list, and every
// vkCreateInstance from inside the container returned -9
// VK_ERROR_INCOMPATIBLE_DRIVER — the Steam client's "CVulkanTopology: failed
// create vulkan instance: -9", and its GPU process aborting on startup.
// Outside the container, where these variables are unset, the very same thunk
// enumerated every GPU correctly.
//
// The host loader's own default search path is the right answer here: driver
// selection on the host side is precisely what the thunk exists to delegate.
// FEX_HOST_<VAR> is honoured first so a host ICD/layer set can still be aimed
// deliberately (e.g. to pick one of several host drivers for testing).
//
// Guest semantics are unaffected: the guest's environment block was
// materialised into guest memory at exec time, long before this host library
// is dlopen()ed, so the guest still sees its own values and passes them to
// any children it spawns.
__attribute__((constructor)) void SanitizeHostVulkanEnvironment() {
  for (const char* Var : {
         "VK_ICD_FILENAMES",
         "VK_DRIVER_FILES",
         "VK_ADD_DRIVER_FILES",
         "VK_LAYER_PATH",
         "VK_ADD_LAYER_PATH",
         "VK_IMPLICIT_LAYER_PATH",
         "VK_ADD_IMPLICIT_LAYER_PATH",
         "VK_INSTANCE_LAYERS",
       }) {
    const auto Override = std::string("FEX_HOST_") + Var;
    if (const char* Value = getenv(Override.c_str())) {
      setenv(Var, Value, 1);
    } else {
      unsetenv(Var);
    }
  }
}
} // namespace


#include "thunkgen_host_libvulkan.inl"

static void ThunkLog(const char* Fmt, ...) __attribute__((format(printf, 1, 2)));
static void FreePNextChain(const void* Struct);

// FEX_VK_ARRAYTRACE=1 logs the count/array queries and the create-info repacks
// that this file implements by hand. Cheap enough to leave in: one cached
// lookup, no work when unset.
static bool VkArrayTrace() {
  static const bool Enabled = getenv("FEX_VK_ARRAYTRACE") != nullptr;
  return Enabled;
}

#include <common/X11Manager.h>


static bool SetupInstance {};
static std::mutex SetupMutex {};

#define LDR_PTR(fn) fexldr_ptr_libvulkan_##fn

static void DoSetupWithInstance(VkInstance instance) {
  std::unique_lock lk {SetupMutex};

  // Needed since the Guest-endpoint calls without a function pointer
  // TODO: Support use of multiple instances
  (void*&)LDR_PTR(vkGetDeviceProcAddr) = (void*)LDR_PTR(vkGetInstanceProcAddr)(instance, "vkGetDeviceProcAddr");
  if (LDR_PTR(vkGetDeviceProcAddr) == nullptr) {
    std::abort();
  }

  // Query pointers for non-EXT functions customized below
  (void*&)LDR_PTR(vkCreateDevice) = (void*)LDR_PTR(vkGetInstanceProcAddr)(instance, "vkCreateDevice");

  // Physical-device queries from device extensions: the loader does not export
  // these, so their dlsym pointers are null and the custom impls below would
  // call through zero.
  (void*&)LDR_PTR(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR) =
    (void*)LDR_PTR(vkGetInstanceProcAddr)(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
  (void*&)LDR_PTR(vkGetPhysicalDeviceVideoFormatPropertiesKHR) =
    (void*)LDR_PTR(vkGetInstanceProcAddr)(instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");

  // Only do this lookup once.
  // NOTE: If vkGetInstanceProcAddr was called with a null instance, only a few function pointers will be filled with non-null values, so we do repeat the lookup in that case
  if (instance) {
    SetupInstance = true;
  }
}

#define FEXFN_IMPL(fn) fexfn_impl_libvulkan_##fn

// Cached proc-addr resolution for the custom impls below (the allocator-
// nulling wrappers and friends). They used to re-resolve their driver pointer
// through vkGet{Device,Instance}ProcAddr on EVERY call to stay multi-device-
// tolerant; radv's GDPA is a hash walk (~100-200ns) and the call rate is
// resource-churn rate, which open-world streaming keeps permanently nonzero.
// Each expansion site instead keeps one {owner, PFN} pair: the unique lambda
// type per expansion gives every wrapper its own statics. Owner mismatch
// re-resolves under the lock, which preserves multi-device correctness.
//
// Invariants / deliberate choices:
// - std::mutex, NOT a lock-free {atomic owner, atomic PFN} pair: Vk handles
//   are freed and reused, so a torn read could pair a stale owner with a new
//   PFN (ABA) and pass the equality check. The uncontended lock is cheaper
//   than the GDPA hash walk these sites used to pay, and none of them are
//   draw-rate.
// - Accepted staleness: destroying an owner and creating a new one at the
//   SAME handle value returns the old resolution (same class of staleness as
//   the vkCreateDevice-time preloads above, which cache per-device pointers
//   for the life of the process). Per-entry-point pointers for one ICD do not
//   differ per device object in practice.
// - A null resolution is NOT cached (matches the old per-call behavior for
//   absent extensions: resolve again next call, crash only if actually
//   invoked through null).
// - The fexldr_ptr_* slot for the wrapped function is no longer written; it
//   keeps its dlsym/init-time value. No code outside these wrappers reads the
//   affected slots (verified 2026-08-13).
#define LDR_PTR_CACHED_IMPL(fn, owner, OwnerType, gpa)                    \
  [](OwnerType Owner_) {                                                  \
    static std::mutex CacheMutex;                                         \
    static OwnerType CachedOwner = VK_NULL_HANDLE;                        \
    static decltype(LDR_PTR(fn)) CachedFn = nullptr;                      \
    std::lock_guard Lock {CacheMutex};                                    \
    if (CachedOwner != Owner_ || !CachedFn) {                             \
      CachedFn = reinterpret_cast<decltype(LDR_PTR(fn))>(LDR_PTR(gpa)(Owner_, #fn)); \
      CachedOwner = Owner_;                                               \
    }                                                                     \
    return CachedFn;                                                      \
  }(owner)
#define LDR_DEVICE_PTR_CACHED(fn, dev) LDR_PTR_CACHED_IMPL(fn, dev, VkDevice, vkGetDeviceProcAddr)
#define LDR_INSTANCE_PTR_CACHED(fn, inst) LDR_PTR_CACHED_IMPL(fn, inst, VkInstance, vkGetInstanceProcAddr)

static X11Manager x11_manager;

static void fexfn_impl_libvulkan_Vulkan_SetGuestXGetVisualInfo(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXGetVisualInfo);
}

static void fexfn_impl_libvulkan_Vulkan_SetGuestXSync(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXSync);
}

static void fexfn_impl_libvulkan_Vulkan_SetGuestXDisplayString(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXDisplayString);
}

void fex_custom_repack_entry(host_layout<VkXcbSurfaceCreateInfoKHR>& to, const guest_layout<VkXcbSurfaceCreateInfoKHR>& from) {
  // TODO: xcb_aux_sync?
  to.data.connection = x11_manager.GuestToHostConnection(const_cast<xcb_connection_t*>(from.data.connection.force_get_host_pointer()));
}

bool fex_custom_repack_exit(guest_layout<VkXcbSurfaceCreateInfoKHR>&, const host_layout<VkXcbSurfaceCreateInfoKHR>&) {
  // TODO: xcb_sync?
  // Input-only struct; see the note on fex_custom_repack_exit(VkInstanceCreateInfo).
  return true;
}

void fex_custom_repack_entry(host_layout<VkXlibSurfaceCreateInfoKHR>& to, const guest_layout<VkXlibSurfaceCreateInfoKHR>& from) {
  to.data.dpy = x11_manager.GuestToHostDisplay(const_cast<Display*>(from.data.dpy.force_get_host_pointer()));
}

bool fex_custom_repack_exit(guest_layout<VkXlibSurfaceCreateInfoKHR>&, const host_layout<VkXlibSurfaceCreateInfoKHR>& from) {
  x11_manager.HostXFlush(from.data.dpy);
  // Input-only struct; see the note on fex_custom_repack_exit(VkInstanceCreateInfo).
  return true;
}

// Both display impls guard against a null fexldr pointer: the loader does not
// export these instance-extension entry points, so the pointer is only filled
// by the requery in vkGetInstanceProcAddr. An application that fetches them
// through vkGetDeviceProcAddr instead reaches the impl with the slot still
// null, and should get an error, not a call through zero.
static VkResult fexfn_impl_libvulkan_vkAcquireXlibDisplayEXT(VkPhysicalDevice a_0, guest_layout<Display*> a_1, VkDisplayKHR a_2) {
  if (!fexldr_ptr_libvulkan_vkAcquireXlibDisplayEXT) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  auto host_display = x11_manager.GuestToHostDisplay(a_1.force_get_host_pointer());
  auto ret = fexldr_ptr_libvulkan_vkAcquireXlibDisplayEXT(a_0, host_display, a_2);
  x11_manager.HostXFlush(host_display);
  return ret;
}

static VkResult fexfn_impl_libvulkan_vkGetRandROutputDisplayEXT(VkPhysicalDevice a_0, guest_layout<Display*> a_1, RROutput a_2, VkDisplayKHR* a_3) {
  if (!fexldr_ptr_libvulkan_vkGetRandROutputDisplayEXT) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  auto host_display = x11_manager.GuestToHostDisplay(a_1.force_get_host_pointer());
  auto ret = fexldr_ptr_libvulkan_vkGetRandROutputDisplayEXT(a_0, host_display, a_2, a_3);
  x11_manager.HostXFlush(host_display);
  return ret;
}

static VkBool32 fexfn_impl_libvulkan_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice a_0, uint32_t a_1,
                                                                                  guest_layout<xcb_connection_t*> a_2, xcb_visualid_t a_3) {
  auto host_connection = x11_manager.GuestToHostConnection(a_2.force_get_host_pointer());
  return fexldr_ptr_libvulkan_vkGetPhysicalDeviceXcbPresentationSupportKHR(a_0, a_1, host_connection, a_3);
}

static VkBool32 fexfn_impl_libvulkan_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice a_0, uint32_t a_1,
                                                                                   guest_layout<Display*> a_2, VisualID a_3) {
  auto host_display = x11_manager.GuestToHostDisplay(a_2.force_get_host_pointer());
  auto ret = fexldr_ptr_libvulkan_vkGetPhysicalDeviceXlibPresentationSupportKHR(a_0, a_1, host_display, a_3);
  x11_manager.HostXFlush(host_display);
  return ret;
}

// Functions with callbacks are overridden to ignore the guest-side callbacks

static VkResult
FEXFN_IMPL(vkCreateShaderModule)(VkDevice a_0, const VkShaderModuleCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkShaderModule* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateShaderModule, a_0)(a_0, a_1, nullptr, a_3);
}

static VkBool32
DummyVkDebugReportCallback(VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT, uint64_t, size_t, int32_t, const char*, const char*, void*) {
  return VK_FALSE;
}

static VkResult FEXFN_IMPL(vkCreateInstance)(const VkInstanceCreateInfo* a_0, const VkAllocationCallbacks* a_1, guest_layout<VkInstance*> a_2) {
  if (VkArrayTrace()) {
    fprintf(stderr, "FEXVKTRACE: vkCreateInstance impl entered info=%p out=%p ldr=%p\n", (const void*)a_0, (void*)a_2.get_pointer(),
            (void*)LDR_PTR(vkCreateInstance));
    fflush(stderr);
  }
  const VkInstanceCreateInfo* vk_struct_base = a_0;
  for (const VkBaseInStructure* vk_struct = reinterpret_cast<const VkBaseInStructure*>(vk_struct_base); vk_struct->pNext;
       vk_struct = vk_struct->pNext) {
    // Override guest callbacks used for VK_EXT_debug_report AND
    // VK_EXT_debug_utils.  Both struct types embed a pfn*Callback that is
    // a guest VA — if native libvulkan invokes them, it runs guest code as
    // host code and SEGVs.  The standalone vkCreateDebugUtilsMessengerEXT
    // path replaces pfnUserCallback with DummyVkDebugUtilsMessengerCallback,
    // but the pNext-of-VkInstanceCreateInfo path was bypassing that
    // protection entirely — strip the messenger struct as well.
    const auto sType = reinterpret_cast<const VkBaseInStructure*>(vk_struct->pNext)->sType;
    if (sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT ||
        sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) {
      // Overwrite the pNext pointer, ignoring its const-qualifier
      const_cast<VkBaseInStructure*>(vk_struct)->pNext = vk_struct->pNext->pNext;

      // If we copied over a nullptr for pNext then early exit
      if (!vk_struct->pNext) {
        break;
      }
    }
  }

  VkInstance out;
  auto ret = LDR_PTR(vkCreateInstance)(vk_struct_base, nullptr, &out);
  *a_2.get_pointer() = to_guest(to_host_layout(out));
  return ret;
}


static VkResult FEXFN_IMPL(vkCreateDevice)(VkPhysicalDevice a_0, const VkDeviceCreateInfo* a_1, const VkAllocationCallbacks* a_2,
                                           guest_layout<VkDevice*> a_3) {
  // Add VK_EXT_map_memory_placed (and the VK_KHR_map_memory2 it builds on) to
  // whatever the guest asked for. The guest will not request them - DXVK has no
  // reason to - but without them vkMapMemory cannot return an address a 32-bit
  // guest can hold. Same injection wine performs for wow64.

  VkDevice out;
  auto ret = LDR_PTR(vkCreateDevice)(a_0, a_1, nullptr, &out);
  if (VkArrayTrace()) {
    fprintf(stderr, "FEXVKTRACE: vkCreateDevice ret=%d device=%p out_guest=%p\n", ret, (void*)out, (void*)a_3.get_pointer());
    fflush(stderr);
  }
  *a_3.get_pointer() = to_guest(to_host_layout(out));

  // Reload device-specific function pointers used in custom implementations.
  // This is only done in advance for functions that don't take a VkDevice
  // argument. Since this breaks multi-device scenarios, other functions reload
  // the function pointer on-demand.
  // NOTE: Running KHR-GLES31.core.compute_shader.simple-compute-shared_context with zink may trigger related issues
  // TODO: Support multi-device scenarios everywhere
  // No functions affected on 64-bit

  return ret;
}


static VkResult FEXFN_IMPL(vkAllocateMemory)(VkDevice a_0, const VkMemoryAllocateInfo* a_1, const VkAllocationCallbacks* a_2, VkDeviceMemory* a_3) {
  auto Ret = LDR_DEVICE_PTR_CACHED(vkAllocateMemory, a_0)(a_0, a_1, nullptr, a_3);
  return Ret;
}

static void FEXFN_IMPL(vkFreeMemory)(VkDevice a_0, VkDeviceMemory a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkFreeMemory, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateDebugReportCallbackEXT)(VkInstance a_0, guest_layout<const VkDebugReportCallbackCreateInfoEXT*> a_1,
                                                           const VkAllocationCallbacks* a_2, VkDebugReportCallbackEXT* a_3) {
  auto overridden_callback = host_layout<VkDebugReportCallbackCreateInfoEXT> {*a_1.get_pointer()}.data;
  overridden_callback.pfnCallback = DummyVkDebugReportCallback;
  return LDR_INSTANCE_PTR_CACHED(vkCreateDebugReportCallbackEXT, a_0)(a_0, &overridden_callback, nullptr, a_3);
}

static void FEXFN_IMPL(vkDestroyDebugReportCallbackEXT)(VkInstance a_0, VkDebugReportCallbackEXT a_1, const VkAllocationCallbacks* a_2) {
  LDR_INSTANCE_PTR_CACHED(vkDestroyDebugReportCallbackEXT, a_0)(a_0, a_1, nullptr);
}

extern "C" VkBool32 DummyVkDebugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                                       const VkDebugUtilsMessengerCallbackDataEXT*, void*) {
  return VK_FALSE;
}

static VkResult FEXFN_IMPL(vkCreateDebugUtilsMessengerEXT)(VkInstance_T* a_0, guest_layout<const VkDebugUtilsMessengerCreateInfoEXT*> a_1,
                                                           const VkAllocationCallbacks* a_2, VkDebugUtilsMessengerEXT* a_3) {
  auto overridden_callback = host_layout<VkDebugUtilsMessengerCreateInfoEXT> {*a_1.get_pointer()}.data;
  overridden_callback.pfnUserCallback = DummyVkDebugUtilsMessengerCallback;
  return LDR_INSTANCE_PTR_CACHED(vkCreateDebugUtilsMessengerEXT, a_0)(a_0, &overridden_callback, nullptr, a_3);
}

// VkAllocationCallbacks embeds five guest function pointers (pfnAllocation,
// pfnReallocation, pfnFree, pfnInternalAllocation, pfnInternalFree).  Passing
// the struct through to native libvulkan causes it to interpret those guest
// VAs as host code pointers and SEGV the moment it tries to allocate.  Each
// FEXFN_IMPL wrapper below forces nullptr for pAllocator so the native loader
// uses its default allocator.  Device-level entrypoints look up their proc-
// addr lazily via vkGetDeviceProcAddr to remain multi-device-tolerant, matching
// the existing vkCreateShaderModule / vkAllocateMemory pattern.

static VkResult FEXFN_IMPL(vkCreateBuffer)(VkDevice a_0, const VkBufferCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkBuffer* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateBuffer, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyBuffer)(VkDevice a_0, VkBuffer a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyBuffer, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateBufferView)(VkDevice a_0, const VkBufferViewCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkBufferView* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateBufferView, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyBufferView)(VkDevice a_0, VkBufferView a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyBufferView, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateImage)(VkDevice a_0, const VkImageCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkImage* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateImage, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyImage)(VkDevice a_0, VkImage a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyImage, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateImageView)(VkDevice a_0, const VkImageViewCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkImageView* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateImageView, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyImageView)(VkDevice a_0, VkImageView a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyImageView, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreatePipelineCache)(VkDevice a_0, const VkPipelineCacheCreateInfo* a_1, const VkAllocationCallbacks* a_2,
                                                  VkPipelineCache* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreatePipelineCache, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyPipelineCache)(VkDevice a_0, VkPipelineCache a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyPipelineCache, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateGraphicsPipelines)(VkDevice a_0, VkPipelineCache a_1, uint32_t a_2, const VkGraphicsPipelineCreateInfo* a_3,
                                                     const VkAllocationCallbacks* a_4, VkPipeline* a_5) {
  if (VkArrayTrace()) {
    fprintf(stderr, "FEXVKTRACE: vkCreateGraphicsPipelines count=%u info=%p", a_2, (const void*)a_3);
    if (a_3) {
      fprintf(stderr, " stages=%u pStages=%p vi=%p ia=%p vp=%p rs=%p ms=%p ds=%p cb=%p dyn=%p layout=%p pNext=%p flags=%x",
              a_3->stageCount, (const void*)a_3->pStages, (const void*)a_3->pVertexInputState, (const void*)a_3->pInputAssemblyState,
              (const void*)a_3->pViewportState, (const void*)a_3->pRasterizationState, (const void*)a_3->pMultisampleState,
              (const void*)a_3->pDepthStencilState, (const void*)a_3->pColorBlendState, (const void*)a_3->pDynamicState,
              (const void*)a_3->layout, (const void*)a_3->pNext, a_3->flags);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
  }
  return LDR_DEVICE_PTR_CACHED(vkCreateGraphicsPipelines, a_0)(a_0, a_1, a_2, a_3, nullptr, a_5);
}

static VkResult FEXFN_IMPL(vkCreateComputePipelines)(VkDevice a_0, VkPipelineCache a_1, uint32_t a_2, const VkComputePipelineCreateInfo* a_3,
                                                    const VkAllocationCallbacks* a_4, VkPipeline* a_5) {
  return LDR_DEVICE_PTR_CACHED(vkCreateComputePipelines, a_0)(a_0, a_1, a_2, a_3, nullptr, a_5);
}

static void FEXFN_IMPL(vkDestroyPipeline)(VkDevice a_0, VkPipeline a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyPipeline, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreatePipelineLayout)(VkDevice a_0, const VkPipelineLayoutCreateInfo* a_1, const VkAllocationCallbacks* a_2,
                                                  VkPipelineLayout* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreatePipelineLayout, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyPipelineLayout)(VkDevice a_0, VkPipelineLayout a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyPipelineLayout, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateSampler)(VkDevice a_0, const VkSamplerCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkSampler* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateSampler, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroySampler)(VkDevice a_0, VkSampler a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroySampler, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateDescriptorSetLayout)(VkDevice a_0, const VkDescriptorSetLayoutCreateInfo* a_1,
                                                       const VkAllocationCallbacks* a_2, VkDescriptorSetLayout* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateDescriptorSetLayout, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyDescriptorSetLayout)(VkDevice a_0, VkDescriptorSetLayout a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyDescriptorSetLayout, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateDescriptorPool)(VkDevice a_0, const VkDescriptorPoolCreateInfo* a_1, const VkAllocationCallbacks* a_2,
                                                  VkDescriptorPool* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateDescriptorPool, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyDescriptorPool)(VkDevice a_0, VkDescriptorPool a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyDescriptorPool, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateSemaphore)(VkDevice a_0, const VkSemaphoreCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkSemaphore* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateSemaphore, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroySemaphore)(VkDevice a_0, VkSemaphore a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroySemaphore, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateFence)(VkDevice a_0, const VkFenceCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkFence* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateFence, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyFence)(VkDevice a_0, VkFence a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyFence, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateEvent)(VkDevice a_0, const VkEventCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkEvent* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateEvent, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyEvent)(VkDevice a_0, VkEvent a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyEvent, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateQueryPool)(VkDevice a_0, const VkQueryPoolCreateInfo* a_1, const VkAllocationCallbacks* a_2, VkQueryPool* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateQueryPool, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyQueryPool)(VkDevice a_0, VkQueryPool a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyQueryPool, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateFramebuffer)(VkDevice a_0, const VkFramebufferCreateInfo* a_1, const VkAllocationCallbacks* a_2,
                                               VkFramebuffer* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateFramebuffer, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyFramebuffer)(VkDevice a_0, VkFramebuffer a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyFramebuffer, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateRenderPass)(VkDevice a_0, const VkRenderPassCreateInfo* a_1, const VkAllocationCallbacks* a_2,
                                              VkRenderPass* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateRenderPass, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyRenderPass)(VkDevice a_0, VkRenderPass a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyRenderPass, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateRenderPass2)(VkDevice a_0, const VkRenderPassCreateInfo2* a_1, const VkAllocationCallbacks* a_2,
                                               VkRenderPass* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateRenderPass2, a_0)(a_0, a_1, nullptr, a_3);
}

static VkResult FEXFN_IMPL(vkCreateRenderPass2KHR)(VkDevice a_0, const VkRenderPassCreateInfo2* a_1, const VkAllocationCallbacks* a_2,
                                                  VkRenderPass* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateRenderPass2KHR, a_0)(a_0, a_1, nullptr, a_3);
}

static VkResult FEXFN_IMPL(vkCreateCommandPool)(VkDevice a_0, const VkCommandPoolCreateInfo* a_1, const VkAllocationCallbacks* a_2,
                                               VkCommandPool* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateCommandPool, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyCommandPool)(VkDevice a_0, VkCommandPool a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyCommandPool, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateSwapchainKHR)(VkDevice a_0, const VkSwapchainCreateInfoKHR* a_1, const VkAllocationCallbacks* a_2,
                                                VkSwapchainKHR* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateSwapchainKHR, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroySwapchainKHR)(VkDevice a_0, VkSwapchainKHR a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroySwapchainKHR, a_0)(a_0, a_1, nullptr);
}

// Follow-up wrappers covering instance/device shutdown plus surface creation
// and the Vulkan 1.1/1.2/1.3 + EXT/KHR descriptor-update-template /
// private-data-slot / sampler-ycbcr-conversion / validation-cache paths.
// Same nullptr-pAllocator rationale as the block above.

static void FEXFN_IMPL(vkDestroyInstance)(VkInstance a_0, const VkAllocationCallbacks* a_1) {
  LDR_INSTANCE_PTR_CACHED(vkDestroyInstance, a_0)(a_0, nullptr);
}

static void FEXFN_IMPL(vkDestroyDevice)(VkDevice a_0, const VkAllocationCallbacks* a_1) {
  LDR_DEVICE_PTR_CACHED(vkDestroyDevice, a_0)(a_0, nullptr);
}

static void FEXFN_IMPL(vkDestroyShaderModule)(VkDevice a_0, VkShaderModule a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyShaderModule, a_0)(a_0, a_1, nullptr);
}

static void FEXFN_IMPL(vkDestroySurfaceKHR)(VkInstance a_0, VkSurfaceKHR a_1, const VkAllocationCallbacks* a_2) {
  LDR_INSTANCE_PTR_CACHED(vkDestroySurfaceKHR, a_0)(a_0, a_1, nullptr);
}

// WSI surface creators (vkCreateXlib/Xcb/WaylandSurfaceKHR).
//
// These were previously thunkgen-default because their Vk*SurfaceCreateInfoKHR
// embeds a Display*/xcb_connection_t* that must be translated via x11_manager
// (through the fex_custom_repack_entry hooks above).  But the auto-generated
// unpacker forwarded the guest pAllocator VERBATIM to the native driver.
// VkAllocationCallbacks is an opaque_type embedding five GUEST function
// pointers; if a guest supplies a custom allocator the native driver interprets
// those guest VAs as host code and SEGVs -- the same hazard class as the
// allocator-nulling family below.  (The WSI path rarely allocates through
// pAllocator today, which is why this was latent, but "rarely" is not "never":
// e.g. layered/driver-internal surface bookkeeping can honor it.)
//
// Convert them to custom_host_impl so pAllocator can be forced to nullptr.  The
// Display*/xcb_connection_t* translation is unaffected: the parameter is left
// non-passthrough, so the generated unpacker still wraps pCreateInfo in a
// repack_wrapper -- applying the custom entry-repack (dpy/connection + pNext)
// before this impl runs and the custom exit-repack (HostXFlush) after it
// returns -- and hands us the already-translated native host pointer.  The only
// deviation from the auto-generated path is substituting nullptr for the guest
// pAllocator, exactly like vkCreateDisplayPlaneSurfaceKHR / vkCreateHeadless-
// SurfaceEXT.  Defined unconditionally so 32-bit and 64-bit thunks both work.
static VkResult FEXFN_IMPL(vkCreateXlibSurfaceKHR)(VkInstance a_0, const VkXlibSurfaceCreateInfoKHR* a_1,
                                                   const VkAllocationCallbacks* a_2, VkSurfaceKHR* a_3) {
  return LDR_INSTANCE_PTR_CACHED(vkCreateXlibSurfaceKHR, a_0)(a_0, a_1, nullptr, a_3);
}

static VkResult FEXFN_IMPL(vkCreateXcbSurfaceKHR)(VkInstance a_0, const VkXcbSurfaceCreateInfoKHR* a_1,
                                                  const VkAllocationCallbacks* a_2, VkSurfaceKHR* a_3) {
  return LDR_INSTANCE_PTR_CACHED(vkCreateXcbSurfaceKHR, a_0)(a_0, a_1, nullptr, a_3);
}

static VkResult FEXFN_IMPL(vkCreateWaylandSurfaceKHR)(VkInstance a_0, const VkWaylandSurfaceCreateInfoKHR* a_1,
                                                      const VkAllocationCallbacks* a_2, VkSurfaceKHR* a_3) {
  return LDR_INSTANCE_PTR_CACHED(vkCreateWaylandSurfaceKHR, a_0)(a_0, a_1, nullptr, a_3);
}

static VkResult FEXFN_IMPL(vkCreateDescriptorUpdateTemplate)(VkDevice a_0, const VkDescriptorUpdateTemplateCreateInfo* a_1,
                                                             const VkAllocationCallbacks* a_2, VkDescriptorUpdateTemplate* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateDescriptorUpdateTemplate, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyDescriptorUpdateTemplate)(VkDevice a_0, VkDescriptorUpdateTemplate a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyDescriptorUpdateTemplate, a_0)(a_0, a_1, nullptr);
}

static VkResult FEXFN_IMPL(vkCreateDescriptorUpdateTemplateKHR)(VkDevice a_0, const VkDescriptorUpdateTemplateCreateInfo* a_1,
                                                                const VkAllocationCallbacks* a_2, VkDescriptorUpdateTemplate* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateDescriptorUpdateTemplateKHR, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyDescriptorUpdateTemplateKHR)(VkDevice a_0, VkDescriptorUpdateTemplate a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyDescriptorUpdateTemplateKHR, a_0)(a_0, a_1, nullptr);
}

static void FEXFN_IMPL(vkDestroyDebugUtilsMessengerEXT)(VkInstance a_0, VkDebugUtilsMessengerEXT a_1, const VkAllocationCallbacks* a_2) {
  LDR_INSTANCE_PTR_CACHED(vkDestroyDebugUtilsMessengerEXT, a_0)(a_0, a_1, nullptr);
}

// Architecture-independent: these only drop the allocation callbacks (FEX does
// not forward guest allocators) and refresh the proc address. They sat inside
// the 64-bit-only block below purely because their registrations did; the
// 32-bit thunk now registers them, so the definitions have to be visible too.
// vkCreateDisplayModeKHR takes VkPhysicalDevice; no owning VkInstance is in
// scope to refresh the proc-addr from, so reuse the pre-loaded dlsym pointer.
static VkResult FEXFN_IMPL(vkCreateDisplayModeKHR)(VkPhysicalDevice a_0, VkDisplayKHR a_1, const VkDisplayModeCreateInfoKHR* a_2,
                                                   const VkAllocationCallbacks* a_3, VkDisplayModeKHR* a_4) {
  return LDR_PTR(vkCreateDisplayModeKHR)(a_0, a_1, a_2, nullptr, a_4);
}
static VkResult FEXFN_IMPL(vkCreateDisplayPlaneSurfaceKHR)(VkInstance a_0, const VkDisplaySurfaceCreateInfoKHR* a_1,
                                                           const VkAllocationCallbacks* a_2, VkSurfaceKHR* a_3) {
  return LDR_INSTANCE_PTR_CACHED(vkCreateDisplayPlaneSurfaceKHR, a_0)(a_0, a_1, nullptr, a_3);
}
static VkResult FEXFN_IMPL(vkCreateHeadlessSurfaceEXT)(VkInstance a_0, const VkHeadlessSurfaceCreateInfoEXT* a_1,
                                                       const VkAllocationCallbacks* a_2, VkSurfaceKHR* a_3) {
  return LDR_INSTANCE_PTR_CACHED(vkCreateHeadlessSurfaceEXT, a_0)(a_0, a_1, nullptr, a_3);
}
static VkResult FEXFN_IMPL(vkCreatePrivateDataSlotEXT)(VkDevice a_0, const VkPrivateDataSlotCreateInfo* a_1,
                                                       const VkAllocationCallbacks* a_2, VkPrivateDataSlot* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreatePrivateDataSlotEXT, a_0)(a_0, a_1, nullptr, a_3);
}
static VkResult FEXFN_IMPL(vkCreateSamplerYcbcrConversionKHR)(VkDevice a_0, const VkSamplerYcbcrConversionCreateInfo* a_1,
                                                              const VkAllocationCallbacks* a_2, VkSamplerYcbcrConversion* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateSamplerYcbcrConversionKHR, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyPrivateDataSlotEXT)(VkDevice a_0, VkPrivateDataSlot a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyPrivateDataSlotEXT, a_0)(a_0, a_1, nullptr);
}
static void FEXFN_IMPL(vkDestroySamplerYcbcrConversionKHR)(VkDevice a_0, VkSamplerYcbcrConversion a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroySamplerYcbcrConversionKHR, a_0)(a_0, a_1, nullptr);
}
static void FEXFN_IMPL(vkDestroyValidationCacheEXT)(VkDevice a_0, VkValidationCacheEXT a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyValidationCacheEXT, a_0)(a_0, a_1, nullptr);
}




static VkResult FEXFN_IMPL(vkCreatePrivateDataSlot)(VkDevice a_0, const VkPrivateDataSlotCreateInfo* a_1, const VkAllocationCallbacks* a_2,
                                                    VkPrivateDataSlot* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreatePrivateDataSlot, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroyPrivateDataSlot)(VkDevice a_0, VkPrivateDataSlot a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroyPrivateDataSlot, a_0)(a_0, a_1, nullptr);
}


static VkResult FEXFN_IMPL(vkCreateSamplerYcbcrConversion)(VkDevice a_0, const VkSamplerYcbcrConversionCreateInfo* a_1,
                                                           const VkAllocationCallbacks* a_2, VkSamplerYcbcrConversion* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateSamplerYcbcrConversion, a_0)(a_0, a_1, nullptr, a_3);
}
static void FEXFN_IMPL(vkDestroySamplerYcbcrConversion)(VkDevice a_0, VkSamplerYcbcrConversion a_1, const VkAllocationCallbacks* a_2) {
  LDR_DEVICE_PTR_CACHED(vkDestroySamplerYcbcrConversion, a_0)(a_0, a_1, nullptr);
}


static VkResult FEXFN_IMPL(vkCreateValidationCacheEXT)(VkDevice a_0, const VkValidationCacheCreateInfoEXT* a_1,
                                                       const VkAllocationCallbacks* a_2, VkValidationCacheEXT* a_3) {
  return LDR_DEVICE_PTR_CACHED(vkCreateValidationCacheEXT, a_0)(a_0, a_1, nullptr, a_3);
}

// CoreIsolation presenter ground truth (docs/CORE_ISOLATION_PLAN.md): record
// which guest thread performs presents, then pass through. The notify symbol
// lives in the FEX binary with default visibility; under an FEX build
// without it (or a foreign host loader) dlsym resolves null once and this
// stays a plain passthrough. 64-bit only — the 32-bit thunk keeps its fully
// generated path so this hook cannot disturb the 32-bit repack machinery.
static VkResult FEXFN_IMPL(vkQueuePresentKHR)(VkQueue queue, const VkPresentInfoKHR* present_info) {
  using NotifyFn = void (*)();
  static const NotifyFn Notify = reinterpret_cast<NotifyFn>(dlsym(RTLD_DEFAULT, "FEX_NotifyGuestPresent"));
  if (Notify) {
    Notify();
  }
  return LDR_PTR(vkQueuePresentKHR)(queue, present_info);
}


// Single-source list of custom_host_impl functions whose ONLY safe caller is
// their fexfn_impl_* wrapper because the wrapper DEFUSES a guest function
// pointer the application always supplies (a debug-callback pointer inside the
// create-info struct).  A name resolved via vkGetInstanceProcAddr /
// vkGetDeviceProcAddr never reaches its fexfn_impl_* wrapper unless it is
// returned from LookupCustomVulkanFunction below: MakeGuestCallable (Guest.cpp)
// links the returned host address to a signature-generic invoker
// (GetCallerForHostFunction -> CallHostFunction<fexthunks_invoke_callback<Sig>>),
// which lands on the host at GuestWrapperForHostFunction<Sig>::Call and branches
// straight to whatever host address procaddr returned.  If that address is the
// raw driver entry (the default LDR_PTR return), the guest's pfn*Callback is
// passed through verbatim; native libvulkan then invokes a guest VA as host
// code and SEGVs the first time a debug message fires.  These two entry points
// are extension functions resolved exclusively via vkGetInstanceProcAddr, so
// the bypass is the common path, not the exception.  Same hazard class the
// vkCreateInstance impl already handles for the pNext-embedded callback.
//
// INVARIANT: every custom_host_impl that defuses or repacks a guest pointer the
// generic GuestWrapperForHostFunction path would pass verbatim MUST be listed
// here (or in the explicit branches below) or it is unreachable via GIPA/GDPA.
// This cannot be asserted at compile time from this translation unit (the set
// of custom_host_impl configs lives in libvulkan_interface.cpp / thunkgen and
// is not visible here), so it is enforced by review against that file.
#define FEX_VULKAN_CALLBACK_DEFUSING_IMPLS(X) \
  X(vkCreateDebugReportCallbackEXT)           \
  X(vkCreateDebugUtilsMessengerEXT)

// Second, larger member of the same hazard class as the callback-defusing list
// above: custom_host_impl functions whose fexfn_impl_* wrapper exists SOLELY to
// force pAllocator = nullptr before forwarding to native libvulkan.
//
// VkAllocationCallbacks is an opaque_type embedding five GUEST function pointers
// (pfnAllocation, pfnReallocation, pfnFree, pfnInternalAllocation,
// pfnInternalFree).  If the guest supplies a custom allocator and the struct is
// passed through verbatim, the native driver interprets those guest VAs as host
// code and SEGVs the instant it allocates.  The wrappers below defuse that by
// substituting nullptr (native default allocator).
//
// As with the callback-defusing list, a name resolved via
// vkGetInstanceProcAddr / vkGetDeviceProcAddr NEVER reaches its fexfn_impl_*
// wrapper unless LookupCustomVulkanFunction returns it (MakeGuestCallable links
// the returned host address to the signature-generic
// GuestWrapperForHostFunction<Sig>::Call, which branches straight to whatever
// host address procaddr returned -- the raw driver entry by default).  Since
// volk / layers / most engines resolve every entry point via GIPA/GDPA, that
// bypass is the COMMON path.  The bug is latent only because most applications
// pass pAllocator = nullptr, making both paths agree; any app with a custom
// allocator crashes.  Routing every wrapper through this table closes that.
//
// INVARIANT: every allocator-nulling custom_host_impl MUST appear here or it is
// unreachable via GIPA/GDPA.  This is the single source of truth for the family
// -- the plain-name branches in LookupCustomVulkanFunction below intentionally
// no longer duplicate any allocator-nulling entry.
//
// A generator-side auto-population was evaluated and rejected for the same
// reason documented in libGL_Host.cpp (FEX_LIBGL_RELOCATING_IMPLS): the thunk
// generator sees only the syntactic `custom_host_impl` flag, not the semantic
// reason a wrapper exists, so it cannot distinguish an allocator-nulling /
// pointer-defusing wrapper (which MUST be procaddr-routed) from a pure
// array-repacking wrapper or a custom_guest_entrypoint resolver such as
// vkGetInstanceProcAddr itself (which must NOT be).  The impl-body
// `#ifndef IS_32BIT_THUNK` guards further live in this .cpp, invisible to the
// generator's interface-only parse.  This X-macro coupling gives the same
// "cannot add the impl without registering it" guarantee locally.
//
// NOTE: the debug-callback creators are deliberately absent here -- they null
// pAllocator too, but are already covered by FEX_VULKAN_CALLBACK_DEFUSING_IMPLS
// above; listing them twice would be redundant (first match wins regardless).

// Wrappers defined unconditionally (both 32-bit and 64-bit thunk builds).
#define FEX_VULKAN_ALLOCATOR_NULLING_IMPLS(X) \
  X(vkCreateShaderModule)                     \
  X(vkCreateInstance)                         \
  X(vkCreateDevice)                           \
  X(vkAllocateMemory)                         \
  X(vkFreeMemory)                             \
  X(vkCreateBuffer)                           \
  X(vkDestroyBuffer)                          \
  X(vkCreateBufferView)                       \
  X(vkDestroyBufferView)                      \
  X(vkCreateImage)                            \
  X(vkDestroyImage)                           \
  X(vkCreateImageView)                        \
  X(vkDestroyImageView)                       \
  X(vkCreatePipelineCache)                    \
  X(vkDestroyPipelineCache)                   \
  X(vkCreateGraphicsPipelines)                \
  X(vkDestroyPipeline)                        \
  X(vkCreatePipelineLayout)                   \
  X(vkDestroyPipelineLayout)                  \
  X(vkCreateSampler)                          \
  X(vkDestroySampler)                         \
  X(vkCreateDescriptorSetLayout)              \
  X(vkDestroyDescriptorSetLayout)             \
  X(vkCreateDescriptorPool)                   \
  X(vkDestroyDescriptorPool)                  \
  X(vkCreateSemaphore)                        \
  X(vkDestroySemaphore)                       \
  X(vkCreateFence)                            \
  X(vkDestroyFence)                           \
  X(vkCreateFramebuffer)                      \
  X(vkDestroyFramebuffer)                     \
  X(vkCreateRenderPass)                       \
  X(vkDestroyRenderPass)                      \
  X(vkCreateRenderPass2)                      \
  X(vkCreateRenderPass2KHR)                   \
  X(vkCreateCommandPool)                      \
  X(vkDestroyCommandPool)                     \
  X(vkCreateSwapchainKHR)                     \
  X(vkDestroySwapchainKHR)                    \
  X(vkDestroyInstance)                        \
  X(vkDestroyDevice)                          \
  X(vkDestroyShaderModule)                    \
  X(vkDestroySurfaceKHR)                      \
  X(vkCreateDescriptorUpdateTemplate)         \
  X(vkDestroyDescriptorUpdateTemplate)        \
  X(vkCreateDescriptorUpdateTemplateKHR)      \
  X(vkDestroyDescriptorUpdateTemplateKHR)     \
  X(vkDestroyDebugReportCallbackEXT)          \
  X(vkDestroyDebugUtilsMessengerEXT)

// Wrappers whose fexfn_impl_* definitions are compiled only in the 64-bit thunk
// (their impl bodies sit inside `#ifndef IS_32BIT_THUNK` blocks in this file).
// Referencing them from a 32-bit build would be an undefined-symbol error, so
// they are expanded only under the same guard below.
#define FEX_VULKAN_ALLOCATOR_NULLING_IMPLS_64BIT(X) \
  X(vkCreateComputePipelines)                       \
  X(vkCreateEvent)                                  \
  X(vkDestroyEvent)                                 \
  X(vkCreateQueryPool)                              \
  X(vkDestroyQueryPool)                             \
  X(vkCreateDisplayPlaneSurfaceKHR)                 \
  X(vkCreateDisplayModeKHR)                         \
  X(vkCreateHeadlessSurfaceEXT)                     \
  X(vkCreatePrivateDataSlot)                        \
  X(vkDestroyPrivateDataSlot)                       \
  X(vkCreatePrivateDataSlotEXT)                     \
  X(vkDestroyPrivateDataSlotEXT)                    \
  X(vkCreateSamplerYcbcrConversion)                 \
  X(vkDestroySamplerYcbcrConversion)                \
  X(vkCreateSamplerYcbcrConversionKHR)              \
  X(vkDestroySamplerYcbcrConversionKHR)             \
  X(vkCreateValidationCacheEXT)                     \
  X(vkDestroyValidationCacheEXT)

// WSI surface creators.  Same procaddr-routing requirement as the
// allocator-nulling family: their custom_host_impl wrappers (which force
// pAllocator = nullptr while translating the embedded Display*/xcb_connection_t*
// via x11_manager) are only reached on the packed-thunk path.  A name resolved
// through vkGetInstanceProcAddr / vkGetDeviceProcAddr -- the path volk / layers /
// most engines actually take -- branches straight to the raw driver entry unless
// this table returns the wrapper, re-opening the exact pAllocator hole the
// wrapper closes.  Defined unconditionally (the impl bodies are not bitness-
// guarded), so this expands outside the IS_32BIT_THUNK guard below.
#define FEX_VULKAN_WSI_SURFACE_IMPLS(X) \
  X(vkCreateXlibSurfaceKHR)             \
  X(vkCreateXcbSurfaceKHR)              \
  X(vkCreateWaylandSurfaceKHR)

static PFN_vkVoidFunction LookupCustomVulkanFunction(const char* a_1) {
  using namespace std::string_view_literals;

#define FEX_VULKAN_PROCADDR_CASE(name)                      \
  if (a_1 == std::string_view {#name}) {                    \
    return (PFN_vkVoidFunction)fexfn_impl_libvulkan_##name; \
  }
  FEX_VULKAN_CALLBACK_DEFUSING_IMPLS(FEX_VULKAN_PROCADDR_CASE)
  FEX_VULKAN_ALLOCATOR_NULLING_IMPLS(FEX_VULKAN_PROCADDR_CASE)
  FEX_VULKAN_WSI_SURFACE_IMPLS(FEX_VULKAN_PROCADDR_CASE)
  FEX_VULKAN_ALLOCATOR_NULLING_IMPLS_64BIT(FEX_VULKAN_PROCADDR_CASE)
#undef FEX_VULKAN_PROCADDR_CASE

  if (a_1 == "vkAcquireXlibDisplayEXT"sv) {
    return (PFN_vkVoidFunction)fexfn_impl_libvulkan_vkAcquireXlibDisplayEXT;
  } else if (a_1 == "vkGetRandROutputDisplayEXT"sv) {
    return (PFN_vkVoidFunction)fexfn_impl_libvulkan_vkGetRandROutputDisplayEXT;
  } else if (a_1 == "vkGetPhysicalDeviceXcbPresentationSupportKHR"sv) {
    return (PFN_vkVoidFunction)fexfn_impl_libvulkan_vkGetPhysicalDeviceXcbPresentationSupportKHR;
  } else if (a_1 == "vkGetPhysicalDeviceXlibPresentationSupportKHR"sv) {
    return (PFN_vkVoidFunction)fexfn_impl_libvulkan_vkGetPhysicalDeviceXlibPresentationSupportKHR;
  }
  return nullptr;
}

static PFN_vkVoidFunction FEXFN_IMPL(vkGetDeviceProcAddr)(VkDevice a_0, const char* a_1) {
  // Just return the host facing function pointer
  // The guest will handle mapping if this exists

  // Check for functions with custom implementations first
  if (auto ptr = LookupCustomVulkanFunction(a_1)) {
    return ptr;
  }

  return LDR_PTR(vkGetDeviceProcAddr)(a_0, a_1);
}

static PFN_vkVoidFunction FEXFN_IMPL(vkGetInstanceProcAddr)(VkInstance a_0, const char* a_1) {
  // Just return the host facing function pointer
  // The guest will handle mapping if it exists

  if (!SetupInstance && a_0) {
    DoSetupWithInstance(a_0);
  }

  // Check for functions with custom implementations first
  if (auto ptr = LookupCustomVulkanFunction(a_1)) {
    // If this function belongs to an instance extension, requery its address.
    // This ensures fexldr_ptr_* is valid if the application creates a minimal
    // VkInstance with no extensions before creating its actual instance.
    using namespace std::string_view_literals;
    if (a_1 == "vkGetRandROutputDisplayEXT"sv && !LDR_PTR(vkGetRandROutputDisplayEXT)) {
      (void*&)LDR_PTR(vkGetRandROutputDisplayEXT) = (void*)LDR_PTR(vkGetInstanceProcAddr)(a_0, "vkGetRandROutputDisplayEXT");
    }
    if (a_1 == "vkAcquireXlibDisplayEXT"sv && !LDR_PTR(vkAcquireXlibDisplayEXT)) {
      (void*&)LDR_PTR(vkAcquireXlibDisplayEXT) = (void*)LDR_PTR(vkGetInstanceProcAddr)(a_0, "vkAcquireXlibDisplayEXT");
    }
    const char* XcbPresent = "vkGetPhysicalDeviceXcbPresentationSupportKHR";
    if (a_1 == std::string_view {XcbPresent} && !LDR_PTR(vkGetPhysicalDeviceXcbPresentationSupportKHR)) {
      (void*&)LDR_PTR(vkGetPhysicalDeviceXcbPresentationSupportKHR) = (void*)LDR_PTR(vkGetInstanceProcAddr)(a_0, XcbPresent);
    }
    const char* XlibPresent = "vkGetPhysicalDeviceXlibPresentationSupportKHR";
    if (a_1 == std::string_view {XlibPresent} && !LDR_PTR(vkGetPhysicalDeviceXlibPresentationSupportKHR)) {
      (void*&)LDR_PTR(vkGetPhysicalDeviceXlibPresentationSupportKHR) = (void*)LDR_PTR(vkGetInstanceProcAddr)(a_0, XlibPresent);
    }

    return ptr;
  }

  return LDR_PTR(vkGetInstanceProcAddr)(a_0, a_1);
}


EXPORTS(libvulkan)
