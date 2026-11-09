// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers shared by the layers in this repo.
#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__GNUC__)
#define LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define LAYER_EXPORT extern "C"
#endif

// Every dispatchable handle starts with a pointer to the loader's dispatch
// table. Handles that belong to the same instance/device share that pointer,
// so it is the usual key for per-instance / per-device layer data.
template <typename T>
static inline void* dispatch_key(T handle) {
  return *reinterpret_cast<void**>(handle);
}

static inline VkLayerInstanceCreateInfo* find_instance_link(const VkInstanceCreateInfo* ci) {
  auto* p = (VkLayerInstanceCreateInfo*)ci->pNext;
  while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                p->function == VK_LAYER_LINK_INFO))
    p = (VkLayerInstanceCreateInfo*)p->pNext;
  return p;
}

static inline VkLayerDeviceCreateInfo* find_device_link(const VkDeviceCreateInfo* ci) {
  auto* p = (VkLayerDeviceCreateInfo*)ci->pNext;
  while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                p->function == VK_LAYER_LINK_INFO))
    p = (VkLayerDeviceCreateInfo*)p->pNext;
  return p;
}

// Walks a const pNext chain looking for a struct of the given sType.
static inline const void* find_in_chain(const void* pNext, VkStructureType sType) {
  auto* p = (const VkBaseInStructure*)pNext;
  while (p) {
    if (p->sType == sType) return p;
    p = p->pNext;
  }
  return nullptr;
}

static inline void* find_in_chain_mut(void* pNext, VkStructureType sType) {
  auto* p = (VkBaseOutStructure*)pNext;
  while (p) {
    if (p->sType == sType) return p;
    p = p->pNext;
  }
  return nullptr;
}

static inline long env_long(const char* name, long def) {
  const char* v = getenv(name);
  if (!v || !*v) return def;
  return strtol(v, nullptr, 10);
}
