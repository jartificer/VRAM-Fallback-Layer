// SPDX-License-Identifier: GPL-3.0-or-later
// VK_LAYER_TEST_nvsim  (test only)
//
// Makes a software Vulkan driver (Mesa lavapipe, which has a single memory
// type) look like an NVIDIA discrete GPU with a small amount of VRAM:
//
//   heap 0: NVSIM_VRAM_MB of "VRAM"          (DEVICE_LOCAL)
//   heap 1: system RAM
//   type 0: heap 1, no flags                 (GPU-only system memory)
//   type 1: heap 0, DEVICE_LOCAL
//   type 2: heap 1, HOST_VISIBLE|COHERENT
//   type 3: heap 1, HOST_VISIBLE|COHERENT|CACHED
//   type 4: heap 0, DEVICE_LOCAL|HOST_VISIBLE|COHERENT   (ReBAR window)
//
// Like the NVIDIA driver, optimal-tiling images only accept types 0 and 1,
// while buffers and linear images accept all five. Device-local allocations
// past the VRAM size fail with VK_ERROR_OUT_OF_DEVICE_MEMORY, which is what
// NVIDIA's Linux driver does, since it has no system-memory spill.
//
// Every fake type maps to lavapipe's real type 0 underneath.

#include "../src/layer_common.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

static const uint32_t kFakeTypeCount = 5;
static const uint32_t kOptimalImageBits = 0x03;
static const uint32_t kAllBits = 0x1f;

static uint64_t g_vram_size;
static uint64_t g_sys_size;
static std::atomic<uint64_t> g_heap_used[2];

static PFN_vkGetInstanceProcAddr g_next_gipa;
static PFN_vkGetDeviceProcAddr g_next_gdpa;
static PFN_vkGetPhysicalDeviceMemoryProperties g_GetMemProps;
static PFN_vkGetPhysicalDeviceMemoryProperties2 g_GetMemProps2;

static PFN_vkAllocateMemory g_AllocateMemory;
static PFN_vkFreeMemory g_FreeMemory;
static PFN_vkCreateImage g_CreateImage;
static PFN_vkDestroyImage g_DestroyImage;
static PFN_vkGetBufferMemoryRequirements g_GetBufReq;
static PFN_vkGetImageMemoryRequirements g_GetImgReq;
static PFN_vkGetBufferMemoryRequirements2 g_GetBufReq2;
static PFN_vkGetImageMemoryRequirements2 g_GetImgReq2;
static PFN_vkGetDeviceBufferMemoryRequirements g_GetDevBufReq;
static PFN_vkGetDeviceImageMemoryRequirements g_GetDevImgReq;

static std::mutex g_lock;
static std::unordered_map<VkDeviceMemory, std::pair<uint32_t, VkDeviceSize>> g_allocs;
static std::unordered_map<VkImage, VkImageTiling> g_tiling;

static const VkMemoryPropertyFlags kFlags[kFakeTypeCount] = {
    0,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
};
static const uint32_t kHeap[kFakeTypeCount] = {1, 0, 1, 1, 0};

static void fill_fake(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties* p) {
  if (!g_sys_size) {
    VkPhysicalDeviceMemoryProperties real;
    g_GetMemProps(phys, &real);
    g_sys_size = real.memoryHeaps[0].size;
  }
  memset(p, 0, sizeof(*p));
  p->memoryHeapCount = 2;
  p->memoryHeaps[0] = {g_vram_size, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
  p->memoryHeaps[1] = {g_sys_size, 0};
  p->memoryTypeCount = kFakeTypeCount;
  for (uint32_t i = 0; i < kFakeTypeCount; i++) p->memoryTypes[i] = {kFlags[i], kHeap[i]};
}

static VKAPI_ATTR void VKAPI_CALL sim_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties* p) {
  fill_fake(phys, p);
}

static VKAPI_ATTR void VKAPI_CALL sim_GetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties2* p) {
  fill_fake(phys, &p->memoryProperties);
  auto* b = (VkPhysicalDeviceMemoryBudgetPropertiesEXT*)find_in_chain_mut(
      p->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
  if (b) {
    memset(b->heapBudget, 0, sizeof(b->heapBudget));
    memset(b->heapUsage, 0, sizeof(b->heapUsage));
    b->heapBudget[0] = g_vram_size;
    b->heapUsage[0] = g_heap_used[0];
    b->heapBudget[1] = g_sys_size / 2;
    b->heapUsage[1] = g_heap_used[1];
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL sim_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo* ai,
                                                         const VkAllocationCallbacks* cb,
                                                         VkDeviceMemory* out) {
  uint32_t t = ai->memoryTypeIndex;
  if (t >= kFakeTypeCount) return VK_ERROR_UNKNOWN;
  uint32_t heap = kHeap[t];
  if (heap == 0) {
    uint64_t prev = g_heap_used[0].fetch_add(ai->allocationSize);
    if (prev + ai->allocationSize > g_vram_size) {
      g_heap_used[0] -= ai->allocationSize;
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
  } else {
    g_heap_used[1] += ai->allocationSize;
  }
  VkMemoryAllocateInfo real = *ai;
  real.memoryTypeIndex = 0;
  VkResult r = g_AllocateMemory(dev, &real, cb, out);
  if (r != VK_SUCCESS) {
    g_heap_used[heap] -= ai->allocationSize;
    return r;
  }
  std::lock_guard<std::mutex> l(g_lock);
  g_allocs[*out] = {heap, ai->allocationSize};
  return r;
}

static VKAPI_ATTR void VKAPI_CALL sim_FreeMemory(VkDevice dev, VkDeviceMemory mem,
                                                 const VkAllocationCallbacks* cb) {
  {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_allocs.find(mem);
    if (it != g_allocs.end()) {
      g_heap_used[it->second.first] -= it->second.second;
      g_allocs.erase(it);
    }
  }
  g_FreeMemory(dev, mem, cb);
}

static VKAPI_ATTR VkResult VKAPI_CALL sim_CreateImage(VkDevice dev, const VkImageCreateInfo* ci,
                                                      const VkAllocationCallbacks* cb, VkImage* out) {
  VkResult r = g_CreateImage(dev, ci, cb, out);
  if (r == VK_SUCCESS) {
    std::lock_guard<std::mutex> l(g_lock);
    g_tiling[*out] = ci->tiling;
  }
  return r;
}

static VKAPI_ATTR void VKAPI_CALL sim_DestroyImage(VkDevice dev, VkImage img,
                                                   const VkAllocationCallbacks* cb) {
  {
    std::lock_guard<std::mutex> l(g_lock);
    g_tiling.erase(img);
  }
  g_DestroyImage(dev, img, cb);
}

static uint32_t image_bits(VkImage img) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_tiling.find(img);
  return (it != g_tiling.end() && it->second == VK_IMAGE_TILING_LINEAR) ? kAllBits : kOptimalImageBits;
}

static VKAPI_ATTR void VKAPI_CALL sim_GetBufferMemoryRequirements(VkDevice d, VkBuffer b,
                                                                  VkMemoryRequirements* r) {
  g_GetBufReq(d, b, r);
  r->memoryTypeBits = kAllBits;
}
static VKAPI_ATTR void VKAPI_CALL sim_GetImageMemoryRequirements(VkDevice d, VkImage i,
                                                                 VkMemoryRequirements* r) {
  g_GetImgReq(d, i, r);
  r->memoryTypeBits = image_bits(i);
}
static VKAPI_ATTR void VKAPI_CALL sim_GetBufferMemoryRequirements2(
    VkDevice d, const VkBufferMemoryRequirementsInfo2* i, VkMemoryRequirements2* r) {
  g_GetBufReq2(d, i, r);
  r->memoryRequirements.memoryTypeBits = kAllBits;
}
static VKAPI_ATTR void VKAPI_CALL sim_GetImageMemoryRequirements2(
    VkDevice d, const VkImageMemoryRequirementsInfo2* i, VkMemoryRequirements2* r) {
  g_GetImgReq2(d, i, r);
  r->memoryRequirements.memoryTypeBits = image_bits(i->image);
}
static VKAPI_ATTR void VKAPI_CALL sim_GetDeviceBufferMemoryRequirements(
    VkDevice d, const VkDeviceBufferMemoryRequirements* i, VkMemoryRequirements2* r) {
  g_GetDevBufReq(d, i, r);
  r->memoryRequirements.memoryTypeBits = kAllBits;
}
static VKAPI_ATTR void VKAPI_CALL sim_GetDeviceImageMemoryRequirements(
    VkDevice d, const VkDeviceImageMemoryRequirements* i, VkMemoryRequirements2* r) {
  g_GetDevImgReq(d, i, r);
  r->memoryRequirements.memoryTypeBits =
      i->pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR ? kAllBits : kOptimalImageBits;
}

static VKAPI_ATTR VkResult VKAPI_CALL sim_CreateInstance(const VkInstanceCreateInfo* ci,
                                                         const VkAllocationCallbacks* cb,
                                                         VkInstance* out) {
  VkLayerInstanceCreateInfo* link = find_instance_link(ci);
  if (!link) return VK_ERROR_INITIALIZATION_FAILED;
  g_next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  VkResult r = ((PFN_vkCreateInstance)g_next_gipa(nullptr, "vkCreateInstance"))(ci, cb, out);
  if (r != VK_SUCCESS) return r;
  g_vram_size = (uint64_t)env_long("NVSIM_VRAM_MB", 256) << 20;
  g_GetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)g_next_gipa(
      *out, "vkGetPhysicalDeviceMemoryProperties");
  g_GetMemProps2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)g_next_gipa(
      *out, "vkGetPhysicalDeviceMemoryProperties2");
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL sim_CreateDevice(VkPhysicalDevice phys,
                                                       const VkDeviceCreateInfo* ci,
                                                       const VkAllocationCallbacks* cb,
                                                       VkDevice* out) {
  VkLayerDeviceCreateInfo* link = find_device_link(ci);
  if (!link) return VK_ERROR_INITIALIZATION_FAILED;
  g_next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  VkResult r = ((PFN_vkCreateDevice)gipa(nullptr, "vkCreateDevice"))(phys, ci, cb, out);
  if (r != VK_SUCCESS) return r;
#define G(var, name) var = (decltype(var))g_next_gdpa(*out, name)
  G(g_AllocateMemory, "vkAllocateMemory");
  G(g_FreeMemory, "vkFreeMemory");
  G(g_CreateImage, "vkCreateImage");
  G(g_DestroyImage, "vkDestroyImage");
  G(g_GetBufReq, "vkGetBufferMemoryRequirements");
  G(g_GetImgReq, "vkGetImageMemoryRequirements");
  G(g_GetBufReq2, "vkGetBufferMemoryRequirements2");
  G(g_GetImgReq2, "vkGetImageMemoryRequirements2");
  G(g_GetDevBufReq, "vkGetDeviceBufferMemoryRequirements");
  G(g_GetDevImgReq, "vkGetDeviceImageMemoryRequirements");
#undef G
  return r;
}

static PFN_vkVoidFunction sim_hook(const char* n) {
#define H(fn) if (!strcmp(n, "vk" #fn)) return (PFN_vkVoidFunction)sim_##fn
#define HA(alias, fn) if (!strcmp(n, "vk" #alias)) return (PFN_vkVoidFunction)sim_##fn
  H(AllocateMemory);
  H(FreeMemory);
  H(CreateImage);
  H(DestroyImage);
  H(GetBufferMemoryRequirements);
  H(GetImageMemoryRequirements);
  H(GetBufferMemoryRequirements2);
  HA(GetBufferMemoryRequirements2KHR, GetBufferMemoryRequirements2);
  H(GetImageMemoryRequirements2);
  HA(GetImageMemoryRequirements2KHR, GetImageMemoryRequirements2);
  H(GetDeviceBufferMemoryRequirements);
  HA(GetDeviceBufferMemoryRequirementsKHR, GetDeviceBufferMemoryRequirements);
  H(GetDeviceImageMemoryRequirements);
  HA(GetDeviceImageMemoryRequirementsKHR, GetDeviceImageMemoryRequirements);
#undef H
#undef HA
  return nullptr;
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nvsim_GetDeviceProcAddr(VkDevice d,
                                                                             const char* n) {
  if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)nvsim_GetDeviceProcAddr;
  if (auto h = sim_hook(n)) return h;
  return g_next_gdpa(d, n);
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nvsim_GetInstanceProcAddr(VkInstance inst,
                                                                               const char* n) {
  if (!strcmp(n, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)nvsim_GetInstanceProcAddr;
  if (!strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)sim_CreateInstance;
  if (!strcmp(n, "vkCreateDevice")) return (PFN_vkVoidFunction)sim_CreateDevice;
  if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)nvsim_GetDeviceProcAddr;
  if (!strcmp(n, "vkGetPhysicalDeviceMemoryProperties"))
    return (PFN_vkVoidFunction)sim_GetPhysicalDeviceMemoryProperties;
  if (!strcmp(n, "vkGetPhysicalDeviceMemoryProperties2") ||
      !strcmp(n, "vkGetPhysicalDeviceMemoryProperties2KHR"))
    return (PFN_vkVoidFunction)sim_GetPhysicalDeviceMemoryProperties2;
  if (auto h = sim_hook(n)) return h;
  return g_next_gipa ? g_next_gipa(inst, n) : nullptr;
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
  if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
  v->pfnGetInstanceProcAddr = nvsim_GetInstanceProcAddr;
  v->pfnGetDeviceProcAddr = nvsim_GetDeviceProcAddr;
  v->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}
