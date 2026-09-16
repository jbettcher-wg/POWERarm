/*
$info$
tags: thunklibs|wayland-client
$end_info$
*/

#include <string_view>
#include <unordered_map>
#include <wayland-client.h>

#include <stdio.h>

#include "common/Host.h"
#include <dlfcn.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <map>
#include <span>
#include <string>

#include <ranges>

template<>
struct guest_layout<wl_argument> {
  using type = wl_argument;
  type data;

  guest_layout& operator=(const wl_argument from) {
    data = from;
    return *this;
  }
};

#include "thunkgen_host_libwayland-client.inl"

// Maps guest interface to host_interfaces
static std::unordered_map<guest_layout<const wl_interface>*, wl_interface*> guest_to_host_interface;

static wl_interface* get_proxy_interface(wl_proxy* proxy) {
  // wl_proxy is a private struct, but its first member is the wl_interface pointer
  return *reinterpret_cast<wl_interface**>(proxy);
}

static void assert_is_valid_host_interface(const wl_interface* interface) {
  // The 32-bit data layout of wl_interface differs from the 64-bit one due to
  // its pointer members. Our repacking code takes care of these differences.
  //
  // To ensure this indeed functions properly, a simple consistency check is
  // applied here: If any of the message counts are absurdly high, it means
  // data from pointer members leaked into other members.

  if ((uint32_t)interface->method_count >= 0x1000 || (uint32_t)interface->event_count >= 0x1000) {
    fprintf(stderr, "ERROR: Expected %p to be a host wl_interface, but it's not\n", interface);
    std::abort();
  }
}

const wl_interface* lookup_wl_interface(guest_layout<const wl_interface*> interface) {
  return interface.force_get_host_pointer();
}

static wl_proxy* fexfn_impl_libwayland_client_wl_proxy_create(wl_proxy* proxy, guest_layout<const wl_interface*> guest_interface_raw) {
  auto host_interface = lookup_wl_interface(guest_interface_raw);
  return fexldr_ptr_libwayland_client_wl_proxy_create(proxy, host_interface);
}

#define WL_CLOSURE_MAX_ARGS 20
static auto fex_wl_remap_argument_list(guest_layout<wl_argument*> args, const wl_message& message) {
  // Cast to host layout and return as std::span
  wl_argument* host_args = host_layout<wl_argument*> {args}.data;
  return std::span<wl_argument, WL_CLOSURE_MAX_ARGS> {host_args, WL_CLOSURE_MAX_ARGS};
}

extern "C" void fexfn_impl_libwayland_client_wl_proxy_marshal_array(wl_proxy* proxy, uint32_t opcode, guest_layout<wl_argument*> args) {
  auto host_args = fex_wl_remap_argument_list(args, get_proxy_interface(proxy)->methods[opcode]);
  fexldr_ptr_libwayland_client_wl_proxy_marshal_array(proxy, opcode, host_args.data());
}

static wl_proxy* fex_wl_proxy_marshal_array(wl_proxy* proxy, uint32_t opcode, guest_layout<wl_argument*> args,
                                            guest_layout<const wl_interface*> guest_interface,
                                            bool constructor, // Call the _constructor variant of the native wayland function
                                            std::optional<uint32_t> version, std::optional<uint32_t> flags) {
  auto interface = lookup_wl_interface(guest_interface);

  assert_is_valid_host_interface(get_proxy_interface(proxy));

  auto host_args = fex_wl_remap_argument_list(args, get_proxy_interface(proxy)->methods[opcode]);

  if (false) {
  } else if (!constructor && !version && !flags) {
    return nullptr;
  } else if (!constructor && version && flags) {
    // wl_proxy_marshal_array_flags is only available starting from Wayland 1.19.91
#if WAYLAND_VERSION_MAJOR * 10000 + WAYLAND_VERSION_MINOR * 100 + WAYLAND_VERSION_MICRO >= 11991
    return fexldr_ptr_libwayland_client_wl_proxy_marshal_array_flags(proxy, opcode, interface, version.value(), flags.value(), host_args.data());
#else
    fprintf(stderr, "Host Wayland version is too old to support FEX thunking\n");
    __builtin_trap();
#endif
  } else if (constructor && version && !flags) {
    return fexldr_ptr_libwayland_client_wl_proxy_marshal_array_constructor_versioned(proxy, opcode, host_args.data(), interface, version.value());
  } else if (constructor && !version && !flags) {
    return fexldr_ptr_libwayland_client_wl_proxy_marshal_array_constructor(proxy, opcode, host_args.data(), interface);
  } else {
    fprintf(stderr, "Invalid configuration\n");
    __builtin_trap();
  }
}

extern "C" wl_proxy* fexfn_impl_libwayland_client_wl_proxy_marshal_array_constructor_versioned(
  wl_proxy* proxy, uint32_t opcode, guest_layout<wl_argument*> args, guest_layout<const wl_interface*> interface, uint32_t version) {
  return fex_wl_proxy_marshal_array(proxy, opcode, args, interface, true, version, std::nullopt);
}

extern "C" wl_proxy* fexfn_impl_libwayland_client_wl_proxy_marshal_array_constructor(
  wl_proxy* proxy, uint32_t opcode, guest_layout<wl_argument*> args, guest_layout<const wl_interface*> interface) {
  return fex_wl_proxy_marshal_array(proxy, opcode, args, interface, true, std::nullopt, std::nullopt);
}

extern "C" wl_proxy* fexfn_impl_libwayland_client_wl_proxy_marshal_array_flags(wl_proxy* proxy, uint32_t opcode,
                                                                               guest_layout<const wl_interface*> interface, uint32_t version,
                                                                               uint32_t flags, guest_layout<wl_argument*> args) {
  return fex_wl_proxy_marshal_array(proxy, opcode, args, interface, false, version, flags);
}

// Variant of CallbackUnpack::CallGuestPtr that relocates a wl_array parameter
// for 32-bit guests. Relocating this parameter is required since it may
// reference inaccessible memory regions (presumably due to pointing to data
// on the host stack).
template<typename Result, typename... Args>
const auto CallGuestPtrWithWaylandArray = CallbackUnpack<Result(Args..., wl_array*)>::CallGuestPtr;

// See wayland-util.h for documentation on protocol message signatures
template<char>
struct ArgType;
template<>
struct ArgType<'s'> {
  using type = const char*;
};
template<>
struct ArgType<'u'> {
  using type = uint32_t;
};
template<>
struct ArgType<'i'> {
  using type = int32_t;
};
template<>
struct ArgType<'o'> {
  using type = wl_proxy*;
};
template<>
struct ArgType<'n'> {
  using type = wl_proxy*;
};
template<>
struct ArgType<'a'> {
  using type = wl_array*;
};
template<>
struct ArgType<'f'> {
  using type = wl_fixed_t;
};
template<>
struct ArgType<'h'> {
  using type = int32_t;
}; // fd?

template<char... Signature>
static void WaylandFinalizeHostTrampolineForGuestListener(void (*callback)()) {
  using cb = void(void*, wl_proxy*, typename ArgType<Signature>::type...);
  FinalizeHostTrampolineForGuestFunction((cb*)callback);
}

extern "C" int
fexfn_impl_libwayland_client_wl_proxy_add_listener(struct wl_proxy* proxy, guest_layout<void (**)(void)> callback_table_raw, void* data) {
  auto interface = get_proxy_interface(proxy);

  assert_is_valid_host_interface(interface);

  auto callback_table = callback_table_raw.force_get_host_pointer();

  for (int i = 0; i < interface->event_count; ++i) {
    auto signature_view = std::string_view {interface->events[i].signature};

    // A leading number indicates the minimum protocol version
    uint32_t since_version = 0;
    auto [ptr, res] = std::from_chars(signature_view.begin(), signature_view.end(), since_version, 10);
    auto signature = std::string {signature_view.substr(ptr - signature_view.begin())};

    // ? just indicates that the argument may be null, so it doesn't change the signature
    signature.erase(std::remove(signature.begin(), signature.end(), '?'), signature.end());

    auto callback = callback_table[i];

    if (signature == "") {
      // E.g. xdg_toplevel::close
      WaylandFinalizeHostTrampolineForGuestListener<>(callback);
    } else if (signature == "a") {
      // E.g. xdg_toplevel::wm_capabilities
      FEX::HLE::FinalizeHostTrampolineForGuestFunction((FEX::HLE::HostToGuestTrampolinePtr*)callback,
                                                       (void*)CallGuestPtrWithWaylandArray<void, void*, wl_proxy*>);
    } else if (signature == "f") {
      WaylandFinalizeHostTrampolineForGuestListener<'f'>(callback);
    } else if (signature == "hu") {
      // E.g. zwp_linux_dmabuf_feedback_v1::format_table
      WaylandFinalizeHostTrampolineForGuestListener<'h', 'u'>(callback);
    } else if (signature == "i") {
      // E.g. wl_output_listener::scale
      WaylandFinalizeHostTrampolineForGuestListener<'i'>(callback);
    } else if (signature == "if") {
      // E.g. wl_touch_listener::orientation
      WaylandFinalizeHostTrampolineForGuestListener<'i', 'f'>(callback);
    } else if (signature == "iff") {
      // E.g. wl_touch_listener::shape
      WaylandFinalizeHostTrampolineForGuestListener<'i', 'f', 'f'>(callback);
    } else if (signature == "ii") {
      // E.g. xdg_toplevel::configure_bounds
      WaylandFinalizeHostTrampolineForGuestListener<'i', 'i'>(callback);
    } else if (signature == "iu") {
      WaylandFinalizeHostTrampolineForGuestListener<'i', 'u'>(callback);
    } else if (signature == "iia") {
      // E.g. xdg_toplevel::configure
      FEX::HLE::FinalizeHostTrampolineForGuestFunction((FEX::HLE::HostToGuestTrampolinePtr*)callback,
                                                       (void*)CallGuestPtrWithWaylandArray<void, void*, wl_proxy*, int32_t, int32_t>);
    } else if (signature == "iiii") {
      WaylandFinalizeHostTrampolineForGuestListener<'i', 'i', 'i', 'i'>(callback);
    } else if (signature == "iiiiissi") {
      // E.g. wl_output_listener::geometry
      WaylandFinalizeHostTrampolineForGuestListener<'i', 'i', 'i', 'i', 'i', 's', 's', 'i'>(callback);
    } else if (signature == "n") {
      // E.g. wl_data_device_listener::data_offer
      WaylandFinalizeHostTrampolineForGuestListener<'n'>(callback);
    } else if (signature == "o") {
      // E.g. wl_data_device_listener::selection
      WaylandFinalizeHostTrampolineForGuestListener<'o'>(callback);
    } else if (signature == "u") {
      // E.g. wl_registry::global_remove
      WaylandFinalizeHostTrampolineForGuestListener<'u'>(callback);
    } else if (signature == "uff") {
      // E.g. wl_pointer_listener::motion
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'f', 'f'>(callback);
    } else if (signature == "uffff") {
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'f', 'f', 'f', 'f'>(callback);
    } else if (signature == "uhu") {
      // E.g. wl_keyboard_listener::keymap
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'h', 'u'>(callback);
    } else if (signature == "ui") {
      // E.g. wl_pointer_listener::axis_discrete
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'i'>(callback);
    } else if (signature == "uiff") {
      // E.g. wl_touch_listener::motion
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'i', 'f', 'f'>(callback);
    } else if (signature == "uiii") {
      // E.g. wl_output_listener::mode
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'i', 'i', 'i'>(callback);
    } else if (signature == "uiiii") {
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'i', 'i', 'i', 'i'>(callback);
    } else if (signature == "uo") {
      // E.g. wl_pointer_listener::leave
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'o'>(callback);
    } else if (signature == "uoa") {
      // E.g. wl_keyboard_listener::enter
      FEX::HLE::FinalizeHostTrampolineForGuestFunction((FEX::HLE::HostToGuestTrampolinePtr*)callback,
                                                       (void*)CallGuestPtrWithWaylandArray<void, void*, wl_proxy*, uint32_t, wl_surface*>);
    } else if (signature == "uoff") {
      // E.g. wl_pointer_listener::enter
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'o', 'f', 'f'>(callback);
    } else if (signature == "uoffo") {
      // E.g. wl_data_device_listener::enter
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'o', 'f', 'f', 'o'>(callback);
    } else if (signature == "uoo") {
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'o', 'o'>(callback);
    } else if (signature == "us") {
      WaylandFinalizeHostTrampolineForGuestListener<'u', 's'>(callback);
    } else if (signature == "uss") {
      WaylandFinalizeHostTrampolineForGuestListener<'u', 's', 's'>(callback);
    } else if (signature == "usu") {
      // E.g. wl_registry::global
      WaylandFinalizeHostTrampolineForGuestListener<'u', 's', 'u'>(callback);
    } else if (signature == "uu") {
      // E.g. wl_pointer_listener::axis_stop
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'u'>(callback);
    } else if (signature == "uuf") {
      // E.g. wl_pointer_listener::axis
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'u', 'f'>(callback);
    } else if (signature == "uui") {
      // E.g. wl_touch_listener::up
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'u', 'i'>(callback);
    } else if (signature == "uuoiff") {
      // E.g. wl_touch_listener::down
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'u', 'o', 'i', 'f', 'f'>(callback);
    } else if (signature == "uuou") {
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'u', 'o', 'u'>(callback);
    } else if (signature == "uuu") {
      // E.g. zwp_linux_dmabuf_v1::modifier
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'u', 'u'>(callback);
    } else if (signature == "uuuu") {
      // E.g. wl_pointer_listener::button
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'u', 'u', 'u'>(callback);
    } else if (signature == "uuuuu") {
      // E.g. wl_keyboard_listener::modifiers
      WaylandFinalizeHostTrampolineForGuestListener<'u', 'u', 'u', 'u', 'u'>(callback);
    } else if (signature == "s") {
      // E.g. wl_seat::name
      WaylandFinalizeHostTrampolineForGuestListener<'s'>(callback);
    } else if (signature == "sii") {
      // E.g. zwp_text_input_v3::preedit_string
      WaylandFinalizeHostTrampolineForGuestListener<'s', 'i', 'i'>(callback);
    } else if (signature == "ss") {
      WaylandFinalizeHostTrampolineForGuestListener<'s', 's'>(callback);
    } else {
      // Newer wayland protocol versions may grow event signature characters
      // we have not added marshalling for yet.  Aborting was hostile to any
      // forward-compat client (e.g. Sway with experimental protocol extensions).
      // Log loudly and skip — the listener fails silently for that proxy event,
      // which matches what an older wayland-client.so would do on an unknown event.
      fprintf(stderr, "FEX: warning: unhandled wayland event signature \"%s\"; listener skipped for this event\n", signature.data());
      (void)0;
    }
  }

  // Pass the original function pointer table to the host wayland library. This ensures the table is valid until the listener is unregistered.
  return fexldr_ptr_libwayland_client_wl_proxy_add_listener(proxy, callback_table, data);
}

void fexfn_impl_libwayland_client_fex_wl_exchange_interface_pointer(guest_layout<wl_interface*> guest_interface_raw, const char* name) {
  auto& guest_interface = *guest_interface_raw.get_pointer();
  auto& host_interface = guest_to_host_interface[reinterpret_cast<guest_layout<const wl_interface>*>(&guest_interface)];
  host_interface = reinterpret_cast<wl_interface*>(dlsym(fexldr_ptr_libwayland_client_so, name));
  if (!host_interface) {
    fprintf(stderr, "Could not find host interface corresponding to %p (%s)\n", &guest_interface, name);
    std::abort();
  }

  // Wayland-client declares interface pointers as `const`, which makes LD put
  // them into the rodata section of the application itself instead of the
  // library. To copy the host information to them on startup, we must
  // temporarily disable write-protection on this data hence.
  // NOTE: This may span page boundaries, so up to 2 pages may need to be changed.
  // Use the host's actual page size (POWER kernels may use 64K pages, in
  // which case hardcoding 4K silently breaks mprotect on most addresses).
  static const uintptr_t kPageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
  static const uintptr_t kPageMask = kPageSize - 1;
  const auto source_addr = reinterpret_cast<uintptr_t>(guest_interface_raw.force_get_host_pointer());
  const auto page_begin = source_addr & ~kPageMask;
  const auto remap_size = ((source_addr & kPageMask) + sizeof(*guest_interface_raw.force_get_host_pointer()) > kPageSize) ? 2 * kPageSize : kPageSize;
  if (0 != mprotect((void*)page_begin, remap_size, PROT_READ | PROT_WRITE)) {
    fprintf(stderr, "ERROR: %s\n", strerror(errno));
    std::abort();
  }

  memcpy(&guest_interface, host_interface, sizeof(wl_interface));

  // TODO: Disabled until we ensure the interface data is indeed stored in rodata
  //  mprotect((void*)page_begin, remap_size, PROT_READ);
}

void fexfn_impl_libwayland_client_fex_wl_get_method_signature(wl_proxy* proxy, uint32_t opcode, char* out) {
  strcpy(out, get_proxy_interface(proxy)->methods[opcode].signature);
}

int fexfn_impl_libwayland_client_fex_wl_get_interface_event_count(wl_proxy* proxy) {
  return get_proxy_interface(proxy)->event_count;
}

void fexfn_impl_libwayland_client_fex_wl_get_interface_event_name(wl_proxy* proxy, int i, char* out) {
  strcpy(out, get_proxy_interface(proxy)->events[i].name);
}

void fexfn_impl_libwayland_client_fex_wl_get_interface_event_signature(wl_proxy* proxy, int i, char* out) {
  strcpy(out, get_proxy_interface(proxy)->events[i].signature);
}

EXPORTS(libwayland_client)
