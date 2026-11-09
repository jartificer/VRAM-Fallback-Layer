// SPDX-License-Identifier: GPL-3.0-or-later
// VK_LAYER_VRAM_fallback
//
// When a game asks for device-local (VRAM) memory and the driver answers
// VK_ERROR_OUT_OF_DEVICE_MEMORY, retry the allocation in a system-memory
// type the GPU can still reach, instead of passing the failure up.
//
// The tricky part is that vkAllocateMemory does not say which buffers or
// images will be bound to the memory. Engines and translation layers
// (DXVK, VKD3D-Proton, VMA) allocate big blocks per memory type and then
// place any resource whose memoryTypeBits contains that type into them.
// So a fallback type is only safe if every resource that allows the
// original type also allows the fallback type. The layer learns that by
// intersecting the memoryTypeBits of every memory-requirements query it
// sees (plus a few probe resources created at vkCreateDevice), and checks
// again at bind time so an unsafe bind fails cleanly instead of corrupting.
//
// Environment variables:
//   VRAM_FALLBACK_HEADROOM_MB  Keep this much VRAM free: once the device-local
//                              budget gets this tight, new device-local
//                              allocations go straight to system memory.
//                              0 (default) = only fall back after a real OOM.
//   VRAM_FALLBACK_LOG          0 = quiet, 1 = log fallbacks (default), 2 = verbose.

#include "layer_common.h"

#include <atomic>
#include <cinttypes>
#include <mutex>
#include <unordered_map>
#include <vector>

#define LAYER_NAME "VK_LAYER_VRAM_fallback"
#define LOG(lvl, ...)                                         \
  do {                                                        \
    if (g_log_level >= (lvl)) {                               \
      fprintf(stderr, "[vram-fallback] " __VA_ARGS__);        \
      fputc('\n', stderr);                                    \
    }                                                         \
  } while (0)

static int g_log_level = 1;
static uint64_t g_headroom = 0;

static constexpr VkMemoryPropertyFlags kHostFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

// ---------------------------------------------------------------------------
// Per-instance and per-device state

struct InstanceData {
  VkInstance instance;
  PFN_vkGetInstanceProcAddr gipa;
  PFN_vkDestroyInstance DestroyInstance;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
  PFN_vkGetPhysicalDeviceMemoryProperties2 GetPhysicalDeviceMemoryProperties2;
  PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
};

struct AllocInfo {
  VkDeviceSize size;
  uint32_t requested_type;
  uint32_t actual_type;
};

struct DeviceData {
  VkDevice device;
  VkPhysicalDevice phys;
  InstanceData* inst;
  PFN_vkGetDeviceProcAddr gdpa;
  bool has_budget = false;

  PFN_vkDestroyDevice DestroyDevice;
  PFN_vkAllocateMemory AllocateMemory;
  PFN_vkFreeMemory FreeMemory;
  PFN_vkCreateBuffer CreateBuffer;
  PFN_vkDestroyBuffer DestroyBuffer;
  PFN_vkCreateImage CreateImage;
  PFN_vkDestroyImage DestroyImage;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
  PFN_vkGetBufferMemoryRequirements2 GetBufferMemoryRequirements2;
  PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2;
  PFN_vkGetDeviceBufferMemoryRequirements GetDeviceBufferMemoryRequirements;
  PFN_vkGetDeviceImageMemoryRequirements GetDeviceImageMemoryRequirements;
  PFN_vkBindBufferMemory BindBufferMemory;
  PFN_vkBindImageMemory BindImageMemory;
  PFN_vkBindBufferMemory2 BindBufferMemory2;
  PFN_vkBindImageMemory2 BindImageMemory2;

  VkPhysicalDeviceMemoryProperties mem;

  // compat[t]: AND of memoryTypeBits over every resource that allows type t.
  // Any type set here can host every resource that could land in a block of t.
  std::atomic<uint32_t> compat[VK_MAX_MEMORY_TYPES];

  std::mutex lock;
  std::unordered_map<VkDeviceMemory, AllocInfo> allocs;  // guarded by lock
  uint64_t heap_used[VK_MAX_MEMORY_HEAPS] = {};          // guarded by lock, bytes this layer saw allocated

  std::atomic<uint64_t> fallback_bytes{0}, fallback_peak{0}, fallback_count{0};
};

static std::mutex g_map_lock;
static std::unordered_map<void*, InstanceData*> g_instances;
static std::unordered_map<void*, DeviceData*> g_devices;

template <typename H>
static InstanceData* get_instance(H h) {
  std::lock_guard<std::mutex> l(g_map_lock);
  auto it = g_instances.find(dispatch_key(h));
  return it == g_instances.end() ? nullptr : it->second;
}
template <typename H>
static DeviceData* get_device(H h) {
  std::lock_guard<std::mutex> l(g_map_lock);
  auto it = g_devices.find(dispatch_key(h));
  return it == g_devices.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Memory type helpers

static bool type_is_vram(const DeviceData* d, uint32_t t) {
  return d->mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}

static bool heap_is_vram(const DeviceData* d, uint32_t heap) {
  return d->mem.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

static void note_type_bits(DeviceData* d, uint32_t bits) {
  for (uint32_t t = 0; t < d->mem.memoryTypeCount; t++)
    if (bits & (1u << t)) d->compat[t].fetch_and(bits);
}

// Picks a system-memory type that can stand in for `requested`.
// `allowed` is the set of types every possible user of the memory accepts.
// Returns UINT32_MAX if there is none.
static uint32_t pick_fallback(const DeviceData* d, uint32_t requested, uint32_t allowed) {
  const VkMemoryPropertyFlags req_flags = d->mem.memoryTypes[requested].propertyFlags;
  // Keep whatever host access the app relies on (mapping, coherency).
  const VkMemoryPropertyFlags must = req_flags & kHostFlags;
  const VkMemoryPropertyFlags never =
      VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
      (~req_flags & VK_MEMORY_PROPERTY_PROTECTED_BIT);

  uint32_t best = UINT32_MAX;
  int best_score = 1 << 30;
  for (uint32_t t = 0; t < d->mem.memoryTypeCount; t++) {
    if (!(allowed & (1u << t))) continue;
    const VkMemoryType& mt = d->mem.memoryTypes[t];
    if (heap_is_vram(d, mt.heapIndex)) continue;  // must be system RAM
    if ((mt.propertyFlags & must) != must) continue;
    if (mt.propertyFlags & never) continue;
    if ((mt.propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT) !=
        (req_flags & VK_MEMORY_PROPERTY_PROTECTED_BIT))
      continue;
    // Prefer types that add as few extra properties as possible. In
    // particular avoid HOST_CACHED (snooped) memory for GPU-only data, and
    // prefer a plain "no flags" system type, which is what NVIDIA exposes for
    // GPU-only system memory.
    int score = __builtin_popcount(mt.propertyFlags & ~req_flags);
    if ((mt.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
        !(req_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
      score += 4;
    if (score < best_score) {
      best = t;
      best_score = score;
    }
  }
  return best;
}

// How much VRAM in `heap` is still free, by the driver's budget if possible.
static uint64_t vram_available(DeviceData* d, uint32_t heap) {
  if (d->has_budget && d->inst->GetPhysicalDeviceMemoryProperties2) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                            &budget};
    d->inst->GetPhysicalDeviceMemoryProperties2(d->phys, &p2);
    uint64_t b = budget.heapBudget[heap], u = budget.heapUsage[heap];
    return u >= b ? 0 : b - u;
  }
  std::lock_guard<std::mutex> l(d->lock);
  uint64_t size = d->mem.memoryHeaps[heap].size, used = d->heap_used[heap];
  return used >= size ? 0 : size - used;
}

// ---------------------------------------------------------------------------
// Probing: create a few typical resources so `compat` is meaningful even for
// the very first allocation, before the app has queried anything itself.

static void probe_resource_types(DeviceData* d) {
  for (uint32_t t = 0; t < d->mem.memoryTypeCount; t++) d->compat[t] = ~0u;

  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = 1 << 20;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  VkBuffer buf;
  if (d->CreateBuffer(d->device, &bci, nullptr, &buf) == VK_SUCCESS) {
    VkMemoryRequirements r;
    d->GetBufferMemoryRequirements(d->device, buf, &r);
    note_type_bits(d, r.memoryTypeBits);
    d->DestroyBuffer(d->device, buf, nullptr);
  }

  struct Probe {
    VkFormat fmt;
    VkImageUsageFlags usage;
  } probes[] = {
      {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
      {VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
      {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT},
      {VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
      {VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
      {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
  };
  for (const Probe& p : probes) {
    for (VkSampleCountFlagBits samples : {VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_4_BIT}) {
      VkImageUsageFlags usage = p.usage;
      if (samples != VK_SAMPLE_COUNT_1_BIT) usage &= ~(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
      VkImageFormatProperties ifp;
      if (d->inst->GetPhysicalDeviceImageFormatProperties(d->phys, p.fmt, VK_IMAGE_TYPE_2D,
                                                          VK_IMAGE_TILING_OPTIMAL, usage, 0,
                                                          &ifp) != VK_SUCCESS ||
          !(ifp.sampleCounts & samples))
        continue;
      VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ici.imageType = VK_IMAGE_TYPE_2D;
      ici.format = p.fmt;
      ici.extent = {256, 256, 1};
      ici.mipLevels = 1;
      ici.arrayLayers = 1;
      ici.samples = samples;
      ici.tiling = VK_IMAGE_TILING_OPTIMAL;
      ici.usage = usage;
      ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImage img;
      if (d->CreateImage(d->device, &ici, nullptr, &img) != VK_SUCCESS) continue;
      VkMemoryRequirements r;
      d->GetImageMemoryRequirements(d->device, img, &r);
      note_type_bits(d, r.memoryTypeBits);
      d->DestroyImage(d->device, img, nullptr);
    }
  }
}

static void log_memory_layout(const DeviceData* d) {
  if (g_log_level < 1) return;
  LOG(1, "active (headroom %" PRIu64 " MiB, budget ext %s)", g_headroom >> 20,
      d->has_budget ? "yes" : "no");
  for (uint32_t h = 0; h < d->mem.memoryHeapCount; h++)
    LOG(2, "  heap %u: %" PRIu64 " MiB%s", h, (uint64_t)(d->mem.memoryHeaps[h].size >> 20),
        heap_is_vram(d, h) ? " (VRAM)" : "");
  for (uint32_t t = 0; t < d->mem.memoryTypeCount; t++) {
    if (!type_is_vram(d, t)) continue;
    uint32_t fb = pick_fallback(d, t, d->compat[t].load());
    if (fb == UINT32_MAX)
      LOG(1, "  type %u (heap %u, flags 0x%x): no safe system-memory fallback", t,
          d->mem.memoryTypes[t].heapIndex, d->mem.memoryTypes[t].propertyFlags);
    else
      LOG(1, "  type %u (heap %u, flags 0x%x) -> falls back to type %u (heap %u, flags 0x%x)", t,
          d->mem.memoryTypes[t].heapIndex, d->mem.memoryTypes[t].propertyFlags, fb,
          d->mem.memoryTypes[fb].heapIndex, d->mem.memoryTypes[fb].propertyFlags);
  }
}

// ---------------------------------------------------------------------------
// Memory requirement queries: pass through, and learn the type masks.

static VKAPI_ATTR void VKAPI_CALL layer_GetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                                                    VkMemoryRequirements* r) {
  DeviceData* d = get_device(device);
  d->GetBufferMemoryRequirements(device, buffer, r);
  note_type_bits(d, r->memoryTypeBits);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetImageMemoryRequirements(VkDevice device, VkImage image,
                                                                   VkMemoryRequirements* r) {
  DeviceData* d = get_device(device);
  d->GetImageMemoryRequirements(device, image, r);
  note_type_bits(d, r->memoryTypeBits);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2* info, VkMemoryRequirements2* r) {
  DeviceData* d = get_device(device);
  d->GetBufferMemoryRequirements2(device, info, r);
  note_type_bits(d, r->memoryRequirements.memoryTypeBits);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2* info, VkMemoryRequirements2* r) {
  DeviceData* d = get_device(device);
  d->GetImageMemoryRequirements2(device, info, r);
  note_type_bits(d, r->memoryRequirements.memoryTypeBits);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetDeviceBufferMemoryRequirements(
    VkDevice device, const VkDeviceBufferMemoryRequirements* info, VkMemoryRequirements2* r) {
  DeviceData* d = get_device(device);
  d->GetDeviceBufferMemoryRequirements(device, info, r);
  note_type_bits(d, r->memoryRequirements.memoryTypeBits);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetDeviceImageMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements* info, VkMemoryRequirements2* r) {
  DeviceData* d = get_device(device);
  d->GetDeviceImageMemoryRequirements(device, info, r);
  note_type_bits(d, r->memoryRequirements.memoryTypeBits);
}

// ---------------------------------------------------------------------------
// Allocation

static bool is_import(const VkMemoryAllocateInfo* ai) {
  for (auto* p = (const VkBaseInStructure*)ai->pNext; p; p = p->pNext) {
    switch (p->sType) {
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
      case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID:
        return true;
      default:
        break;
    }
  }
  return false;
}

// Exact memoryTypeBits when the allocation is dedicated to one resource,
// otherwise everything we have learned about the requested type.
static uint32_t allowed_types_for(DeviceData* d, const VkMemoryAllocateInfo* ai) {
  auto* ded = (const VkMemoryDedicatedAllocateInfo*)find_in_chain(
      ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
  VkMemoryRequirements r;
  if (ded && ded->image != VK_NULL_HANDLE) {
    d->GetImageMemoryRequirements(d->device, ded->image, &r);
    return r.memoryTypeBits;
  }
  if (ded && ded->buffer != VK_NULL_HANDLE) {
    d->GetBufferMemoryRequirements(d->device, ded->buffer, &r);
    return r.memoryTypeBits;
  }
  return d->compat[ai->memoryTypeIndex].load();
}

static void record_alloc(DeviceData* d, VkDeviceMemory mem, const VkMemoryAllocateInfo* ai,
                         uint32_t actual) {
  std::lock_guard<std::mutex> l(d->lock);
  d->allocs[mem] = {ai->allocationSize, ai->memoryTypeIndex, actual};
  d->heap_used[d->mem.memoryTypes[actual].heapIndex] += ai->allocationSize;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_AllocateMemory(VkDevice device,
                                                           const VkMemoryAllocateInfo* ai,
                                                           const VkAllocationCallbacks* cb,
                                                           VkDeviceMemory* out) {
  DeviceData* d = get_device(device);
  const uint32_t req = ai->memoryTypeIndex;

  if (req >= d->mem.memoryTypeCount || !type_is_vram(d, req) || is_import(ai)) {
    VkResult res = d->AllocateMemory(device, ai, cb, out);
    if (res == VK_SUCCESS && req < d->mem.memoryTypeCount) record_alloc(d, *out, ai, req);
    return res;
  }

  const uint32_t heap = d->mem.memoryTypes[req].heapIndex;
  bool tried_vram = false;
  VkResult res = VK_ERROR_OUT_OF_DEVICE_MEMORY;

  // Reactive path: just try VRAM first, unless headroom says it is too tight.
  bool vram_tight = g_headroom && vram_available(d, heap) < ai->allocationSize + g_headroom;
  if (!vram_tight) {
    res = d->AllocateMemory(device, ai, cb, out);
    tried_vram = true;
    if (res == VK_SUCCESS) {
      record_alloc(d, *out, ai, req);
      return res;
    }
    if (res != VK_ERROR_OUT_OF_DEVICE_MEMORY) return res;
  }

  const uint32_t fb = pick_fallback(d, req, allowed_types_for(d, ai));
  if (fb != UINT32_MAX) {
    VkMemoryAllocateInfo copy = *ai;
    copy.memoryTypeIndex = fb;
    VkResult fres = d->AllocateMemory(device, &copy, cb, out);
    if (fres == VK_SUCCESS) {
      record_alloc(d, *out, ai, fb);
      uint64_t now = d->fallback_bytes += ai->allocationSize;
      d->fallback_count++;
      uint64_t peak = d->fallback_peak.load();
      while (now > peak && !d->fallback_peak.compare_exchange_weak(peak, now)) {}
      LOG(1, "%s: %" PRIu64 " MiB type %u -> system type %u (now %" PRIu64 " MiB in system RAM)",
          vram_tight ? "VRAM nearly full" : "VRAM full",
          (uint64_t)((ai->allocationSize + (1 << 20) - 1) >> 20), req, fb, now >> 20);
      return VK_SUCCESS;
    }
    LOG(2, "fallback allocation of %" PRIu64 " bytes in type %u failed (%d)",
        (uint64_t)ai->allocationSize, fb, fres);
  } else {
    LOG(1, "VRAM full and no safe system-memory type for type %u; passing error to the app", req);
  }

  if (!tried_vram) {
    // Headroom made us skip VRAM, but system memory did not work out either.
    res = d->AllocateMemory(device, ai, cb, out);
    if (res == VK_SUCCESS) record_alloc(d, *out, ai, req);
  }
  return res;
}

static VKAPI_ATTR void VKAPI_CALL layer_FreeMemory(VkDevice device, VkDeviceMemory mem,
                                                   const VkAllocationCallbacks* cb) {
  DeviceData* d = get_device(device);
  if (mem != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> l(d->lock);
    auto it = d->allocs.find(mem);
    if (it != d->allocs.end()) {
      d->heap_used[d->mem.memoryTypes[it->second.actual_type].heapIndex] -= it->second.size;
      if (it->second.actual_type != it->second.requested_type) d->fallback_bytes -= it->second.size;
      d->allocs.erase(it);
    }
  }
  d->FreeMemory(device, mem, cb);
}

// ---------------------------------------------------------------------------
// Binding: refuse binds that would put a resource into a memory type it does
// not support. This only happens if a resource with a narrower memoryTypeBits
// shows up after we already handed out a fallback block. VK_ERROR_OUT_OF_DEVICE_MEMORY
// is a legal result for vkBind*Memory, and allocators handle it as "try elsewhere".

static bool fallback_type_of(DeviceData* d, VkDeviceMemory mem, uint32_t* actual) {
  std::lock_guard<std::mutex> l(d->lock);
  auto it = d->allocs.find(mem);
  if (it == d->allocs.end() || it->second.actual_type == it->second.requested_type) return false;
  *actual = it->second.actual_type;
  return true;
}

static bool buffer_bind_ok(DeviceData* d, VkBuffer buf, VkDeviceMemory mem) {
  uint32_t actual;
  if (!fallback_type_of(d, mem, &actual)) return true;
  VkMemoryRequirements r;
  d->GetBufferMemoryRequirements(d->device, buf, &r);
  if (r.memoryTypeBits & (1u << actual)) return true;
  LOG(1, "refusing to bind a buffer (types 0x%x) into fallback memory of type %u",
      r.memoryTypeBits, actual);
  return false;
}

static bool image_bind_ok(DeviceData* d, VkImage img, VkDeviceMemory mem, bool per_plane) {
  uint32_t actual;
  if (per_plane || !fallback_type_of(d, mem, &actual)) return true;
  VkMemoryRequirements r;
  d->GetImageMemoryRequirements(d->device, img, &r);
  if (r.memoryTypeBits & (1u << actual)) return true;
  LOG(1, "refusing to bind an image (types 0x%x) into fallback memory of type %u",
      r.memoryTypeBits, actual);
  return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_BindBufferMemory(VkDevice device, VkBuffer buf,
                                                             VkDeviceMemory mem, VkDeviceSize off) {
  DeviceData* d = get_device(device);
  if (!buffer_bind_ok(d, buf, mem)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  return d->BindBufferMemory(device, buf, mem, off);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_BindImageMemory(VkDevice device, VkImage img,
                                                            VkDeviceMemory mem, VkDeviceSize off) {
  DeviceData* d = get_device(device);
  if (!image_bind_ok(d, img, mem, false)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  return d->BindImageMemory(device, img, mem, off);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_BindBufferMemory2(VkDevice device, uint32_t n,
                                                              const VkBindBufferMemoryInfo* infos) {
  DeviceData* d = get_device(device);
  for (uint32_t i = 0; i < n; i++)
    if (!buffer_bind_ok(d, infos[i].buffer, infos[i].memory)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  return d->BindBufferMemory2(device, n, infos);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_BindImageMemory2(VkDevice device, uint32_t n,
                                                             const VkBindImageMemoryInfo* infos) {
  DeviceData* d = get_device(device);
  for (uint32_t i = 0; i < n; i++) {
    bool per_plane = find_in_chain(infos[i].pNext, VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO);
    if (infos[i].memory != VK_NULL_HANDLE &&
        !image_bind_ok(d, infos[i].image, infos[i].memory, per_plane))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  return d->BindImageMemory2(device, n, infos);
}

// ---------------------------------------------------------------------------
// Instance / device lifetime

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo* ci,
                                                           const VkAllocationCallbacks* cb,
                                                           VkInstance* out) {
  VkLayerInstanceCreateInfo* link = find_instance_link(ci);
  if (!link) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;

  auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkResult res = create(ci, cb, out);
  if (res != VK_SUCCESS) return res;

  g_log_level = (int)env_long("VRAM_FALLBACK_LOG", 1);
  g_headroom = (uint64_t)env_long("VRAM_FALLBACK_HEADROOM_MB", 0) << 20;

  auto* id = new InstanceData();
  id->instance = *out;
  id->gipa = gipa;
#define IGET(name) id->name = (PFN_vk##name)gipa(*out, "vk" #name)
  IGET(DestroyInstance);
  IGET(GetPhysicalDeviceMemoryProperties);
  IGET(GetPhysicalDeviceMemoryProperties2);
  IGET(GetPhysicalDeviceImageFormatProperties);
  IGET(EnumerateDeviceExtensionProperties);
#undef IGET
  if (!id->GetPhysicalDeviceMemoryProperties2)
    id->GetPhysicalDeviceMemoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(
        *out, "vkGetPhysicalDeviceMemoryProperties2KHR");

  std::lock_guard<std::mutex> l(g_map_lock);
  g_instances[dispatch_key(*out)] = id;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance,
                                                        const VkAllocationCallbacks* cb) {
  InstanceData* id;
  {
    std::lock_guard<std::mutex> l(g_map_lock);
    auto it = g_instances.find(dispatch_key(instance));
    if (it == g_instances.end()) return;
    id = it->second;
    g_instances.erase(it);
  }
  id->DestroyInstance(instance, cb);
  delete id;
}

static bool device_supports_ext(InstanceData* id, VkPhysicalDevice phys, const char* name) {
  uint32_t n = 0;
  id->EnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  id->EnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
  for (auto& e : exts)
    if (!strcmp(e.extensionName, name)) return true;
  return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice phys,
                                                         const VkDeviceCreateInfo* ci,
                                                         const VkAllocationCallbacks* cb,
                                                         VkDevice* out) {
  InstanceData* id = get_instance(phys);
  VkLayerDeviceCreateInfo* link = find_device_link(ci);
  if (!id || !link) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;

  // Turn on VK_EXT_memory_budget for ourselves if the driver has it, so the
  // headroom check can read real VRAM usage (including other processes).
  bool app_has_budget = false;
  for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
    if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
      app_has_budget = true;
  bool budget_supported =
      app_has_budget || device_supports_ext(id, phys, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  std::vector<const char*> exts(ci->ppEnabledExtensionNames,
                                ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
  if (budget_supported && !app_has_budget) exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  VkDeviceCreateInfo ci2 = *ci;
  ci2.enabledExtensionCount = (uint32_t)exts.size();
  ci2.ppEnabledExtensionNames = exts.data();

  auto create = (PFN_vkCreateDevice)gipa(id->instance, "vkCreateDevice");
  VkResult res = create(phys, &ci2, cb, out);
  if (res != VK_SUCCESS) return res;

  auto* d = new DeviceData();
  d->device = *out;
  d->phys = phys;
  d->inst = id;
  d->gdpa = gdpa;
  d->has_budget = budget_supported;
#define DGET(name) d->name = (PFN_vk##name)gdpa(*out, "vk" #name)
#define DGET2(name, alias) \
  do { DGET(name); if (!d->name) d->name = (PFN_vk##name)gdpa(*out, "vk" #alias); } while (0)
  DGET(DestroyDevice);
  DGET(AllocateMemory);
  DGET(FreeMemory);
  DGET(CreateBuffer);
  DGET(DestroyBuffer);
  DGET(CreateImage);
  DGET(DestroyImage);
  DGET(GetBufferMemoryRequirements);
  DGET(GetImageMemoryRequirements);
  DGET2(GetBufferMemoryRequirements2, GetBufferMemoryRequirements2KHR);
  DGET2(GetImageMemoryRequirements2, GetImageMemoryRequirements2KHR);
  DGET2(GetDeviceBufferMemoryRequirements, GetDeviceBufferMemoryRequirementsKHR);
  DGET2(GetDeviceImageMemoryRequirements, GetDeviceImageMemoryRequirementsKHR);
  DGET(BindBufferMemory);
  DGET(BindImageMemory);
  DGET2(BindBufferMemory2, BindBufferMemory2KHR);
  DGET2(BindImageMemory2, BindImageMemory2KHR);
#undef DGET
#undef DGET2
  id->GetPhysicalDeviceMemoryProperties(phys, &d->mem);

  {
    std::lock_guard<std::mutex> l(g_map_lock);
    g_devices[dispatch_key(*out)] = d;
  }
  probe_resource_types(d);
  log_memory_layout(d);
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device,
                                                      const VkAllocationCallbacks* cb) {
  DeviceData* d;
  {
    std::lock_guard<std::mutex> l(g_map_lock);
    auto it = g_devices.find(dispatch_key(device));
    if (it == g_devices.end()) return;
    d = it->second;
    g_devices.erase(it);
  }
  if (d->fallback_count)
    LOG(1, "device destroyed: %" PRIu64 " allocations fell back, peak %" PRIu64
           " MiB in system RAM",
        d->fallback_count.load(), d->fallback_peak.load() >> 20);
  d->DestroyDevice(device, cb);
  delete d;
}

// ---------------------------------------------------------------------------
// Proc address plumbing

static PFN_vkVoidFunction device_hook(const char* name) {
#define HOOK(fn) if (!strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)layer_##fn
#define HOOK_AS(alias, fn) if (!strcmp(name, "vk" #alias)) return (PFN_vkVoidFunction)layer_##fn
  HOOK(DestroyDevice);
  HOOK(AllocateMemory);
  HOOK(FreeMemory);
  HOOK(GetBufferMemoryRequirements);
  HOOK(GetImageMemoryRequirements);
  HOOK(GetBufferMemoryRequirements2);
  HOOK_AS(GetBufferMemoryRequirements2KHR, GetBufferMemoryRequirements2);
  HOOK(GetImageMemoryRequirements2);
  HOOK_AS(GetImageMemoryRequirements2KHR, GetImageMemoryRequirements2);
  HOOK(GetDeviceBufferMemoryRequirements);
  HOOK_AS(GetDeviceBufferMemoryRequirementsKHR, GetDeviceBufferMemoryRequirements);
  HOOK(GetDeviceImageMemoryRequirements);
  HOOK_AS(GetDeviceImageMemoryRequirementsKHR, GetDeviceImageMemoryRequirements);
  HOOK(BindBufferMemory);
  HOOK(BindImageMemory);
  HOOK(BindBufferMemory2);
  HOOK_AS(BindBufferMemory2KHR, BindBufferMemory2);
  HOOK(BindImageMemory2);
  HOOK_AS(BindImageMemory2KHR, BindImageMemory2);
#undef HOOK
#undef HOOK_AS
  return nullptr;
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vram_fallback_GetDeviceProcAddr(VkDevice device,
                                                                                    const char* name);

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vram_fallback_GetDeviceProcAddr(VkDevice device, const char* name) {
  if (!strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)vram_fallback_GetDeviceProcAddr;
  DeviceData* d = get_device(device);
  if (!d) return nullptr;
  PFN_vkVoidFunction next = d->gdpa(device, name);
  // Only hook what the driver below actually provides (extension aliases).
  if (!next) return nullptr;
  PFN_vkVoidFunction hook = device_hook(name);
  return hook ? hook : next;
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vram_fallback_GetInstanceProcAddr(VkInstance instance, const char* name) {
  if (!strcmp(name, "vkGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)vram_fallback_GetInstanceProcAddr;
  if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)layer_CreateInstance;
  if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)layer_DestroyInstance;
  if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)layer_CreateDevice;
  if (!strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)vram_fallback_GetDeviceProcAddr;
  if (instance == VK_NULL_HANDLE) return nullptr;
  InstanceData* id = get_instance(instance);
  if (!id) return nullptr;
  PFN_vkVoidFunction next = id->gipa(instance, name);
  if (!next) return nullptr;
  PFN_vkVoidFunction hook = device_hook(name);
  return hook ? hook : next;
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
  if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
  if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
  v->pfnGetInstanceProcAddr = vram_fallback_GetInstanceProcAddr;
  v->pfnGetDeviceProcAddr = vram_fallback_GetDeviceProcAddr;
  v->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}
