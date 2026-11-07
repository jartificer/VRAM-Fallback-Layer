// Test app: behaves like a game engine that runs out of VRAM.
// Run with the nvsim layer (fake 256 MiB NVIDIA card on lavapipe), with or
// without the fallback layer, and optionally the Khronos validation layer.
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_validation_errors = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      printf("  FAIL: " __VA_ARGS__);                     \
      printf("\n");                                       \
      g_failures++;                                       \
    }                                                     \
  } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL on_debug(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT,
                                               const VkDebugUtilsMessengerCallbackDataEXT* d,
                                               void*) {
  if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    g_validation_errors++;
    printf("  VALIDATION: %s\n", d->pMessage);
  }
  return VK_FALSE;
}

struct Ctx {
  VkInstance inst;
  VkPhysicalDevice phys;
  VkDevice dev;
  VkQueue queue;
  VkCommandPool pool;
  VkPhysicalDeviceMemoryProperties mem;
  PFN_vkGetPhysicalDeviceMemoryProperties2 getMem2;
};

static uint32_t find_type(const Ctx& c, VkMemoryPropertyFlags exact) {
  for (uint32_t i = 0; i < c.mem.memoryTypeCount; i++)
    if (c.mem.memoryTypes[i].propertyFlags == exact) return i;
  return UINT32_MAX;
}

static uint64_t vram_usage(const Ctx& c) {
  VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
  c.getMem2(c.phys, &p);
  return b.heapUsage[0];
}

static VkResult alloc(const Ctx& c, VkDeviceSize size, uint32_t type, VkDeviceMemory* out,
                      const void* pNext = nullptr) {
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, pNext, size, type};
  return vkAllocateMemory(c.dev, &ai, nullptr, out);
}

static VkCommandBuffer begin(const Ctx& c) {
  VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, c.pool,
                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(c.dev, &ai, &cb);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  vkBeginCommandBuffer(cb, &bi);
  return cb;
}

static void submit(const Ctx& c, VkCommandBuffer cb) {
  vkEndCommandBuffer(cb);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(c.queue);
  vkFreeCommandBuffers(c.dev, c.pool, 1, &cb);
}

struct Buf {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  void* ptr = nullptr;
};

static VkBuffer make_buffer(const Ctx& c, VkDeviceSize size) {
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VkBuffer b;
  vkCreateBuffer(c.dev, &bci, nullptr, &b);
  return b;
}

static Buf staging(const Ctx& c, VkDeviceSize size) {
  Buf s;
  s.buf = make_buffer(c, size);
  VkMemoryRequirements r;
  vkGetBufferMemoryRequirements(c.dev, s.buf, &r);
  alloc(c, r.size, find_type(c, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
        &s.mem);
  vkBindBufferMemory(c.dev, s.buf, s.mem, 0);
  vkMapMemory(c.dev, s.mem, 0, VK_WHOLE_SIZE, 0, &s.ptr);
  return s;
}

static void free_buf(const Ctx& c, Buf& b) {
  vkDestroyBuffer(c.dev, b.buf, nullptr);
  vkFreeMemory(c.dev, b.mem, nullptr);
}

static VkImage make_image(const Ctx& c, uint32_t w, uint32_t h) {
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  VkImage img;
  vkCreateImage(c.dev, &ici, nullptr, &img);
  return img;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &b);
}

// Uploads a pattern into `img` and reads it back through the GPU.
static bool image_roundtrip(const Ctx& c, VkImage img, uint32_t w, uint32_t h, uint32_t seed) {
  VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
  Buf up = staging(c, bytes), down = staging(c, bytes);
  uint32_t* src = (uint32_t*)up.ptr;
  for (VkDeviceSize i = 0; i < bytes / 4; i++) src[i] = (uint32_t)(i * 2654435761u) ^ seed;
  memset(down.ptr, 0, bytes);

  VkCommandBuffer cb = begin(c);
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyBufferToImage(cb, up.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.buf, 1, &region);
  submit(c, cb);

  bool ok = !memcmp(up.ptr, down.ptr, bytes);
  free_buf(c, up);
  free_buf(c, down);
  return ok;
}

// Fills `buf` on the GPU and reads it back.
static bool buffer_roundtrip(const Ctx& c, VkBuffer buf, VkDeviceSize size, uint32_t value) {
  Buf down = staging(c, size);
  VkCommandBuffer cb = begin(c);
  vkCmdFillBuffer(cb, buf, 0, size, value);
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb,
                       0, nullptr, 0, nullptr);
  VkBufferCopy region = {0, 0, size};
  vkCmdCopyBuffer(cb, buf, down.buf, 1, &region);
  submit(c, cb);
  bool ok = true;
  for (VkDeviceSize i = 0; i < size / 4; i++)
    if (((uint32_t*)down.ptr)[i] != value) { ok = false; break; }
  free_buf(c, down);
  return ok;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  Ctx c = {};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "vram_test";
  app.apiVersion = VK_API_VERSION_1_3;
  const char* inst_exts[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = 1;
  ici.ppEnabledExtensionNames = inst_exts;
  if (vkCreateInstance(&ici, nullptr, &c.inst) != VK_SUCCESS) {
    printf("vkCreateInstance failed\n");
    return 2;
  }
  auto mk = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(c.inst, "vkCreateDebugUtilsMessengerEXT");
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  if (mk) {
    VkDebugUtilsMessengerCreateInfoEXT mci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    mci.pfnUserCallback = on_debug;
    mk(c.inst, &mci, nullptr, &messenger);
  }

  uint32_t n = 1;
  vkEnumeratePhysicalDevices(c.inst, &n, &c.phys);
  vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem);
  c.getMem2 = vkGetPhysicalDeviceMemoryProperties2;

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = 0;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dev_exts[] = {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dev_exts;
  if (vkCreateDevice(c.phys, &dci, nullptr, &c.dev) != VK_SUCCESS) {
    printf("vkCreateDevice failed\n");
    return 2;
  }
  vkGetDeviceQueue(c.dev, 0, 0, &c.queue);
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  vkCreateCommandPool(c.dev, &pci, nullptr, &c.pool);

  const uint32_t vram_type = find_type(c, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  const uint32_t bar_type = find_type(c, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  printf("VRAM heap: %llu MiB, device-local type %u, BAR type %u\n",
         (unsigned long long)(c.mem.memoryHeaps[0].size >> 20), vram_type, bar_type);
  if (vram_type == UINT32_MAX || bar_type == UINT32_MAX) {
    printf("expected NVIDIA-like memory types; is the nvsim layer enabled?\n");
    return 2;
  }

  // 1. Allocate 8 x 64 MiB "VRAM" blocks on a 256 MiB card, like a
  //    suballocator would.
  printf("[1] allocate 8 x 64 MiB device-local blocks\n");
  const VkDeviceSize kBlock = 64ull << 20;
  std::vector<VkDeviceMemory> blocks;
  int failed = 0;
  for (int i = 0; i < 8; i++) {
    VkDeviceMemory m;
    VkResult r = alloc(c, kBlock, vram_type, &m);
    if (r == VK_SUCCESS) blocks.push_back(m);
    else failed++;
    printf("  block %d: %s (VRAM in use: %llu MiB)\n", i, r == VK_SUCCESS ? "ok" : "OUT_OF_DEVICE_MEMORY",
           (unsigned long long)(vram_usage(c) >> 20));
  }
  CHECK(failed == 0, "%d of 8 allocations failed", failed);

  // 2. Put a buffer and an optimal-tiling image into the last block (which
  //    can only be system memory by now) and check the GPU reads and writes it.
  if (!blocks.empty()) {
    printf("[2] buffer + optimal image inside the last block\n");
    VkDeviceMemory m = blocks.back();
    VkBuffer b = make_buffer(c, 4 << 20);
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(c.dev, b, &br);
    CHECK(br.memoryTypeBits & (1u << vram_type), "buffer can't use VRAM type?");
    bool bound = vkBindBufferMemory(c.dev, b, m, 0) == VK_SUCCESS;
    CHECK(bound, "bind buffer");
    if (bound) CHECK(buffer_roundtrip(c, b, 4 << 20, 0xC0FFEE11), "buffer contents wrong");

    VkImage img = make_image(c, 1024, 1024);
    VkMemoryRequirements ir;
    vkGetImageMemoryRequirements(c.dev, img, &ir);
    VkDeviceSize off = ((8ull << 20) + ir.alignment - 1) / ir.alignment * ir.alignment;
    bound = vkBindImageMemory(c.dev, img, m, off) == VK_SUCCESS;
    CHECK(bound, "bind image");
    if (bound) CHECK(image_roundtrip(c, img, 1024, 1024, 0x1234), "image contents wrong");
    vkDestroyImage(c.dev, img, nullptr);
    vkDestroyBuffer(c.dev, b, nullptr);
  }

  // 3. Host-visible VRAM (ReBAR) while VRAM is full: must stay mappable.
  printf("[3] 64 MiB of host-visible VRAM (ReBAR) while VRAM is full\n");
  {
    VkDeviceMemory m;
    VkResult r = alloc(c, kBlock, bar_type, &m);
    CHECK(r == VK_SUCCESS, "ReBAR allocation failed (%d)", r);
    if (r == VK_SUCCESS) {
      void* p = nullptr;
      CHECK(vkMapMemory(c.dev, m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p, "map failed");
      if (p) {
        for (size_t i = 0; i < kBlock / 4; i++) ((uint32_t*)p)[i] = 0xABCD0000u + (uint32_t)i;
        VkBuffer b = make_buffer(c, 1 << 20);
        vkBindBufferMemory(c.dev, b, m, 0);
        Buf down = staging(c, 1 << 20);
        VkCommandBuffer cb = begin(c);
        VkBufferCopy region = {0, 0, 1 << 20};
        vkCmdCopyBuffer(cb, b, down.buf, 1, &region);
        submit(c, cb);
        CHECK(!memcmp(p, down.ptr, 1 << 20), "GPU did not see CPU writes");
        free_buf(c, down);
        vkDestroyBuffer(c.dev, b, nullptr);
        vkUnmapMemory(c.dev, m);
      }
      vkFreeMemory(c.dev, m, nullptr);
    }
  }

  // 4. Dedicated allocation for a big render target (VKD3D does this).
  printf("[4] dedicated 4096x4096 RGBA8 image while VRAM is full\n");
  {
    VkImage img = make_image(c, 4096, 4096);
    VkMemoryRequirements ir;
    vkGetImageMemoryRequirements(c.dev, img, &ir);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = img;
    VkDeviceMemory m;
    VkResult r = alloc(c, ir.size, vram_type, &m, &ded);
    CHECK(r == VK_SUCCESS, "dedicated allocation failed (%d)", r);
    if (r == VK_SUCCESS) {
      bool bound = vkBindImageMemory(c.dev, img, m, 0) == VK_SUCCESS;
      CHECK(bound, "bind dedicated image");
      if (bound) CHECK(image_roundtrip(c, img, 4096, 4096, 0x5555), "dedicated image contents wrong");
      vkFreeMemory(c.dev, m, nullptr);
    }
    vkDestroyImage(c.dev, img, nullptr);
  }

  // 5. Free everything; VRAM should be used again for new allocations.
  printf("[5] free all blocks, then allocate again\n");
  for (auto m : blocks) vkFreeMemory(c.dev, m, nullptr);
  blocks.clear();
  {
    VkDeviceMemory m;
    uint64_t before = vram_usage(c);
    CHECK(alloc(c, kBlock, vram_type, &m) == VK_SUCCESS, "re-allocation failed");
    uint64_t after = vram_usage(c);
    printf("  VRAM in use %llu -> %llu MiB\n", (unsigned long long)(before >> 20),
           (unsigned long long)(after >> 20));
    CHECK(after - before == kBlock, "new allocation did not go to VRAM");
    vkFreeMemory(c.dev, m, nullptr);
  }

  vkDestroyCommandPool(c.dev, c.pool, nullptr);
  vkDestroyDevice(c.dev, nullptr);
  if (messenger) {
    auto rm = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(c.inst, "vkDestroyDebugUtilsMessengerEXT");
    rm(c.inst, messenger, nullptr);
  }
  vkDestroyInstance(c.inst, nullptr);

  printf("result: %d failures, %d validation errors\n", g_failures, g_validation_errors);
  return (g_failures || g_validation_errors) ? 1 : 0;
}
