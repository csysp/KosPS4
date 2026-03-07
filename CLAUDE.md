# CLAUDE.md — KosPS4 (shadPS4 Fork)

## Project Overview

PS4 emulator based on shadPS4. C++23, Vulkan rendering backend, CMake build system.

**Build:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Key subsystems:** `src/video_core/` (Vulkan renderer, buffer/texture cache, shader recompiler), `src/core/` (CPU emulation, kernel, 46 PS4 system libraries), `src/input/`, `src/imgui/`.

**No automated test suite.** Verify changes via:
- Vulkan validation layers (`VK_LAYER_KHRONOS_validation`)
- Tracy profiler (enabled in Debug builds)
- Game testing across multiple titles

**Code style:** PascalCase functions/classes, snake_case variables, 100-char line limit, clang-format enforced.

---

## Target Game

**Bloodborne** — all performance and memory work is scoped to Bloodborne compatibility and behavior. Use it as the primary regression title for all changes. Observe VRAM growth, frame time, and hitching specifically during Bloodborne gameplay (Central Yharnam → Cleric Beast fight is a good benchmark scene).

---

## Performance Audit Findings

Audit date: 2026-03-06. Analyzed the Vulkan rendering hot path (per-draw and per-frame operations). Scoped to Bloodborne.

### Phase 0: Critical Bug

#### 0.1 Buffer Cache Garbage Collector Is Dead Code

**Files:** `src/video_core/buffer_cache/buffer_cache.cpp:841-864`, `src/common/lru_cache.h:57`

The `RunGarbageCollector()` function defines a `clean_up` lambda (line 854) that calls `DownloadBufferMemory` and `DeleteBuffer`, but **never invokes it**. The LRU cache provides `ForEachItemBelow(tick, func)` (lru_cache.h:57) specifically designed for this purpose. The buffer cache inserts into the LRU (line 623) and touches it (line 867), but GC never iterates it.

**Consequence:** GPU buffers are never evicted. VRAM grows monotonically until OOM. The `trigger_gc_memory`, `critical_gc_memory`, `aggressive` flag, and `max_deletions` limiter are all dead logic.

**Fix:** After the early return at line 849, add:
```cpp
lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
```

**Status:** Done

---

### Phase 1: Low-Risk, High-Impact

#### 1.1 O(n) Linear Search in Vertex Buffer Binding

**File:** `src/video_core/buffer_cache/buffer_cache.cpp:223-229`

In `BindVertexBuffers()`, after sorting and merging vertex buffer ranges (lines 191-206), the code uses `std::ranges::find_if` to linearly search `ranges_merged` for each guest buffer. Since `ranges_merged` is already sorted by `base_address`, this should use binary search.

**Impact:** Up to 32 buffers × 32 merged ranges = 1024 comparisons per draw call × hundreds of draws per frame.

**Fix:** Replace with `std::ranges::upper_bound` + `std::prev`:
```cpp
const auto it = std::ranges::upper_bound(
    ranges_merged, buffer.base_address, std::less{}, &BufferRange::base_address);
ASSERT(it != ranges_merged.begin());
const auto host_buffer_info = std::prev(it);
```

**Status:** Done

---

### Phase 2: Medium-Risk, High-Impact

#### 2.1 Overly Broad Pipeline Barriers

**File:** `src/video_core/buffer_cache/buffer_cache.cpp:325-377` (CopyBuffer), `659-678` (SynchronizeBuffer)

Multiple barrier sites use `vk::PipelineStageFlagBits2::eAllCommands` for both src and dst stages. This creates full pipeline drains, preventing the GPU driver from overlapping transfer work with unrelated graphics/compute work.

**Fix:** Narrow to specific stages:
- Pre-barrier src: `eAllGraphics | eComputeShader` (stages that read/wrote the buffer)
- Pre-barrier dst: `eTransfer`
- Post-barrier src: `eTransfer`
- Post-barrier dst: `eAllGraphics | eComputeShader`

Also clean up redundant access masks in SynchronizeBuffer pre-barrier (lines 661-663) where `eTransferRead | eTransferWrite` are redundant with `eMemoryRead | eMemoryWrite`.

**Verification:** MUST run with Vulkan synchronization validation (`syncval`) and achieve zero errors.

**Status:** Done — narrowed all `eAllCommands` in CopyBuffer, SynchronizeBuffer, WriteDataBuffer, and JoinBuffers to `eAllGraphics | eComputeShader`.

#### 2.2 Duplicate FindImage Calls Per Draw

**File:** `src/video_core/renderer_vulkan/vk_rasterizer.cpp:679` (BindTextures), `129,141` (PrepareRenderState)

`TextureCache::FindImage()` acquires a mutex, walks an interval map, and performs linear scans through image lists. When the same physical texture is bound to multiple slots (shadow maps, cubemaps, mipgen), the full lookup repeats.

**Fix:** Add a per-bind-call dedup cache:
```cpp
boost::container::small_vector<std::pair<VAddr, ImageId>, 16> image_dedup;
```
Before each `FindImage`, check `image_dedup` for matching address. Insert result on miss.

**Status:** Done (already implemented)

---

### Phase 3: Medium-Risk, Moderate-Impact

#### 3.1 O(n) ResetBindings Per Draw

**File:** `src/video_core/renderer_vulkan/vk_rasterizer.h:106-111`

`ResetBindings()` iterates all bound images and clears their binding info via cache-unfriendly random writes into the texture cache slot pool. Called every draw call.

**Fix:** Replace with generation counter. Add `u32 current_bind_generation` to Rasterizer and `u32 bind_generation` to Image binding. `is_bound` becomes `image.bind_generation == current_bind_generation`. Reset becomes `++current_bind_generation; bound_images.clear()` — O(1).

The `is_target` flag needs separate handling via a small fixed-size array indexed by render target slot (max 9 entries).

**Status:** Done (already implemented via `AdvanceBindGeneration()` + `bind_gen` field)

#### 3.2 Dirty Flags Bitmask Consolidation

**File:** `src/video_core/renderer_vulkan/vk_scheduler.h:88-121`, `vk_scheduler.cpp:199-356`

`DynamicState::Commit()` checks 26 individual `bool` dirty flags per draw call. The common case (most clean) still evaluates many branches.

**Fix:** Replace bitfield struct with `u32 dirty_flags` enum:
```cpp
enum DirtyBit : u32 {
    Viewports = 1u << 0,
    Scissors  = 1u << 1,
    // ...
};
```
Benefits: early-out when `dirty_flags == 0`, grouped checks with masks, single-instruction clear.

**Status:** Done (already implemented)

---

### Phase 4: Higher-Risk, High-Impact

#### 4.1 Async Pipeline Compilation

**File:** `src/video_core/renderer_vulkan/vk_pipeline_cache.h`

Pipeline compilation (`vkCreateGraphicsPipelines` / `vkCreateComputePipelines`) runs synchronously on the render thread. New shaders cause 10-100ms frame hitches.

**Fix:** Add compilation thread pool (2-4 threads). On cache miss, enqueue job and return `nullptr`. Rasterizer skips draw when pipeline unavailable. Next frame uses compiled result. Make opt-in via config.

**Risks:** Visible pop-in, thread safety for `tsl::robin_map` caches, game compatibility.

**Status:** Done (already implemented) — 2-thread pool gated by `Config::getAsyncShaderCompilation()`, with `CompileThreadFunc`, `FlushCompletedPipelines`, dedup via `enqueued_graphics`/`enqueued_compute` maps, and proper shutdown.

#### 4.2 InsertPermut Reallocation Guard

**File:** `src/video_core/renderer_vulkan/vk_pipeline_cache.h:66`

`modules.resize(std::max(modules.size(), perm_idx + 1))` can cause heap allocation when `perm_idx >= MaxPermutations(8)` and default-constructs intermediate elements.

**Fix:** Add `ASSERT(perm_idx < MaxPermutations)` and pre-reserve.

**Status:** Done (already implemented)

---

## Performance Optimization Roadmap

| # | Issue | File(s) | Risk | Impact | Effort | Status |
|---|-------|---------|------|--------|--------|--------|
| 0.1 | Dead GC (memory leak bug) | `buffer_cache.cpp:841` | Very Low | Critical | 1 line | Done |
| 1.1 | O(n) vertex buffer lookup | `buffer_cache.cpp:223` | Very Low | Medium-High | 3 lines | Done |
| 2.1 | Broad pipeline barriers | `buffer_cache.cpp:326,659` | Medium | High | ~20 lines | Done |
| 2.2 | Duplicate FindImage calls | `vk_rasterizer.cpp:679` | Medium | Medium | ~15 lines | Done |
| 3.1 | O(n) ResetBindings | `vk_rasterizer.h:106` | Medium | Medium | ~20 lines | Done |
| 3.2 | Dirty flags bitmask | `vk_scheduler.h:88` | Low | Low-Medium | ~50 lines | Done |
| 4.1 | Async pipeline compilation | `vk_pipeline_cache.h` | High | High | ~200+ lines | Done |
| 4.2 | InsertPermut guard | `vk_pipeline_cache.h:66` | Very Low | Low | 2 lines | Done |

All items except 4.1 (async pipeline compilation) are complete. Item 4.1 is a standalone feature requiring ~200+ lines, thread pool infrastructure, and careful thread safety work.

---

## Existing Good Patterns (Preserve)

- `tsl::robin_map` for O(1) pipeline lookups (`vk_pipeline_cache.h:131-133`)
- `boost::container::static_vector` / `small_vector` for stack-allocated containers
- `boost::icl::interval_map` for memory range tracking (`range_set.h`)
- LRU cache design (`common/lru_cache.h`) — correct structure, broken call site
- Dirty flag system for dynamic state — avoids redundant Vulkan commands
- Stream buffers for small read-only data (`buffer_cache.cpp:383`)
- Tracy GPU profiling integration (`vk_scheduler.cpp:17-18`)
- Multi-level page table for O(1) address lookups (`multi_level_page_table.h`)

---

## Memory Leak Audit

Audit date: 2026-03-06. Scoped to Bloodborne. Focus: VRAM and heap growth over a 30-minute play session from the Hunter's Dream through Central Yharnam to the Cleric Beast fight.

### ML-1: ShaderModule Handles Leaked at Shutdown

**File:** `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:295`, `vk_shader_util.cpp:257`

`CompileSPV()` returns a raw `vk::ShaderModule` (not `vk::UniqueShaderModule`). These handles are stored in `Program::modules` inside `program_cache`. `~PipelineCache()` never iterates `program_cache` to call `device.destroyShaderModule()`. Hundreds of shader modules are silently leaked at emulator shutdown.

**Impact:** Shutdown-time leak only — no per-session growth. However it generates Vulkan validation errors (`VUID-vkDestroyDevice-device-05137`) and is trivially fixable.

**Fix:** Add to `~PipelineCache()` after joining threads:
```cpp
const auto& device = instance.GetDevice();
for (const auto& [_, program] : program_cache) {
    for (const auto& m : program->modules) {
        if (m.module) {
            device.destroyShaderModule(m.module);
        }
    }
}
```

**Status:** Done

---

### ML-2: Texture Cache GC Blind Spot — `trigger_gc_memory = 0` Path

**File:** `src/video_core/texture_cache/texture_cache.cpp:35-38, 969-973`

When `instance.CanReportMemoryUsage()` is false (driver doesn't support `VK_EXT_memory_budget`), `trigger_gc_memory` is set to `0`. At line 972, `if (total_used_memory < trigger_gc_memory)` becomes `if (0 < 0)` — always false — so GC *always* runs every frame regardless of memory pressure, burning CPU time each submit for the no-op case when `total_used_memory` (accumulated via `guest_size` estimates) is also 0 at startup.

More critically, when the driver *does* support `memory_budget`, `GetDeviceMemoryUsage()` overwrites the estimated `total_used_memory` (line 970). If the driver reports less usage than the estimate (e.g. memory-compressed formats), `trigger_gc_memory` may never be reached, silently preventing all GC.

**Impact:** Either always-running GC (wasted CPU) or never-running GC (growing VRAM) depending on driver. For Bloodborne on AMD/Intel without `memory_budget`, GC runs every frame but `ticks_to_destroy=16` is fine. On NVIDIA (which does report `memory_budget`), verify GC actually fires under real VRAM pressure.

**Fix:** Add a fallback estimator: when `CanReportMemoryUsage()` is false, use `total_used_memory` (the `guest_size`-accumulated counter) rather than replacing it with 0 at the check site.

**Status:** Done — `trigger_gc_memory` now set to `DEFAULT_TRIGGER_GC_MEMORY` (768 MB) in the no-`memory_budget` path. Added named constant to `texture_cache.h`.

---

### ML-3: Texture Cache `image_map` and `page_table` — Vector Capacity Never Trimmed

**File:** `src/video_core/texture_cache/texture_cache.cpp:831, 839-854`

`page_table[page]` is a `vector<ImageId>`. Images are registered with `push_back` (line 831) and unregistered with `erase` (line 853). After a burst of image creation/destruction (e.g. Bloodborne's loading screens create/destroy hundreds of render targets), vectors for heavily-used pages accumulate high `capacity` without ever shrinking. Across a 4 KB page table covering a 4 GB address space, this can accumulate MB of fragmented `vector` storage.

**Fix:** After `erase`, `shrink_to_fit()` when the vector becomes empty:
```cpp
if (image_ids.empty()) {
    image_ids.shrink_to_fit();
}
```

**Status:** Done

---

### ML-4: `surface_metas` Map Grows Without Dedicated GC

**File:** `src/video_core/texture_cache/texture_cache.cpp:670, 676, 692, 1044-1050`

`surface_metas` (a `tsl::robin_map`) entries for CMASK/FMASK/HTILE addresses are only removed inside `DeleteImage()`. However, `FreeImage()` → `DeleteImage()` is only called from GC or explicit invalidation. If images are never freed (e.g. VRAM usage stays below `trigger_gc_memory`), `surface_metas` grows monotonically with every unique render target surface seen. Bloodborne uses dozens of unique render target configurations per scene transition.

**Impact:** Unbounded growth during normal play on machines with large VRAM (e.g. 16+ GB GPUs where GC never triggers). Each entry is ~64 bytes; 10,000 entries = ~640 KB, non-trivial over a long session.

**Fix:** This is fundamentally tied to fixing ML-2 (ensuring GC runs). Additionally, consider a `surface_metas.max_load_factor(0.5)` or periodic sweep to remove entries whose associated image no longer exists.

**Status:** Done — added periodic sweep every 512 GC ticks in `RunGarbageCollector()`. Builds valid meta address set from all live images, prunes stale entries.

---

### ML-5: `UnmapMemory` Skips Image Data Download

**File:** `src/video_core/texture_cache/texture_cache.cpp:197`

`TextureCache::UnmapMemory()` frees images without downloading their data back to host memory (the `TODO` at line 197). When the PS4 game unmaps and remaps a virtual address range — which Bloodborne does during scene transitions — any dirty GPU-side image data is discarded silently. This causes visual corruption (textures go black or show stale data).

**Impact:** Correctness bug that may explain texture corruption in Bloodborne. Not a memory leak per se but is the root cause of one class of image re-creation churn (game unmaps → remaps → emulator creates new images → old images linger in GC queue).

**Fix:** Before `FreeImage(id)`, check `image.usage.transfer_src` and issue a readback if dirty:
```cpp
for (const ImageId id : deleted_images) {
    auto& image = slot_images[id];
    if (image.flags & ImageFlagBits::GpuModified) {
        DownloadImageMemory(id);
    }
    FreeImage(id);
}
```

**Status:** Done — `DownloadImageMemory(id)` called unconditionally before `FreeImage` (the function already guards on `GpuModified` internally).

---

### ML-6: `ObjectManager<T>` Raw `new` Without Destructor Cleanup

**File:** `src/core/libraries/np/object_manager.h:28, 47`

`ObjectManager` stores raw `T*` pointers via `new T{args...}` (line 28) and frees them via `delete obj` (line 47). The struct itself has no destructor — if the game exits without calling the corresponding `DeleteObject` for every `CreateObject`, all allocated objects leak. This affects `np_tus.cpp` (`NpTusRequest`, `NpTusTitleContext`) and potentially other users.

**Fix:** Add a destructor to `ObjectManager`:
```cpp
~ObjectManager() {
    std::scoped_lock lk{mutex};
    for (auto* obj : objects) {
        delete obj;
    }
}
```

**Status:** Done

---

### Memory Leak Roadmap

| # | Issue | File(s) | Type | Impact | Effort | Status |
|---|-------|---------|------|--------|--------|--------|
| ML-1 | ShaderModule handles not destroyed | `vk_pipeline_cache.cpp:295` | Shutdown leak | Low (VK validation) | ~8 lines | Done |
| ML-2 | Texture GC trigger logic broken | `texture_cache.cpp:35,972` | Logic bug → no GC | High (VRAM growth) | ~10 lines | Done |
| ML-3 | Page table vector capacity not trimmed | `texture_cache.cpp:831,853` | Heap fragmentation | Medium | ~3 lines | Done |
| ML-4 | `surface_metas` grows without GC | `texture_cache.cpp:670` | Unbounded map | Medium | ~20 lines | Done |
| ML-5 | `UnmapMemory` drops dirty image data | `texture_cache.cpp:197` | Correctness + churn | High (visual bugs) | ~10 lines | Done |
| ML-6 | `ObjectManager` no destructor | `object_manager.h:28` | Shutdown leak | Low (NP libs) | ~5 lines | Done |

**Suggested order:** ML-2 first (gates ML-4, fixes VRAM growth), then ML-5 (correctness, reduces image churn), then ML-1+ML-6 together (shutdown cleanup), then ML-3+ML-4 (polish).

---

## Measurement Protocol

1. **Baseline:** Bloodborne. Record 60-second Tracy captures in Central Yharnam. Extract: mean/P99 frame time, GPU active time, VRAM usage (via `VK_EXT_memory_budget` if available), draw calls/frame.
2. **Memory baseline:** 30-minute session, sample VRAM every 60 seconds. Establish growth rate before any fix.
3. **Per-change:** Apply single fix, re-record identical captures. Reject if P99 regresses >5%.
4. **Validation:** Zero new Vulkan validation errors (`VK_LAYER_KHRONOS_validation`). Synchronization validation for barrier changes.
5. **Regression:** Boot + first interactive scene. Watch for texture corruption, especially after scene transitions (ML-5 area).
6. **Memory verification:** After ML-2 fix, confirm VRAM stabilizes within 5 minutes of play (no monotonic growth).
