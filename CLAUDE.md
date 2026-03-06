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

## Performance Audit Findings

Audit date: 2026-03-06. Analyzed the Vulkan rendering hot path (per-draw and per-frame operations).

### Phase 0: Critical Bug

#### 0.1 Buffer Cache Garbage Collector Is Dead Code

**Files:** `src/video_core/buffer_cache/buffer_cache.cpp:841-864`, `src/common/lru_cache.h:57`

The `RunGarbageCollector()` function defines a `clean_up` lambda (line 854) that calls `DownloadBufferMemory` and `DeleteBuffer`, but **never invokes it**. The LRU cache provides `ForEachItemBelow(tick, func)` (lru_cache.h:57) specifically designed for this purpose. The buffer cache inserts into the LRU (line 623) and touches it (line 867), but GC never iterates it.

**Consequence:** GPU buffers are never evicted. VRAM grows monotonically until OOM. The `trigger_gc_memory`, `critical_gc_memory`, `aggressive` flag, and `max_deletions` limiter are all dead logic.

**Fix:** After the early return at line 849, add:
```cpp
lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
```

**Status:** Not started

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

**Status:** Not started

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

**Status:** Not started

#### 2.2 Duplicate FindImage Calls Per Draw

**File:** `src/video_core/renderer_vulkan/vk_rasterizer.cpp:679` (BindTextures), `129,141` (PrepareRenderState)

`TextureCache::FindImage()` acquires a mutex, walks an interval map, and performs linear scans through image lists. When the same physical texture is bound to multiple slots (shadow maps, cubemaps, mipgen), the full lookup repeats.

**Fix:** Add a per-bind-call dedup cache:
```cpp
boost::container::small_vector<std::pair<VAddr, ImageId>, 16> image_dedup;
```
Before each `FindImage`, check `image_dedup` for matching address. Insert result on miss.

**Status:** Not started

---

### Phase 3: Medium-Risk, Moderate-Impact

#### 3.1 O(n) ResetBindings Per Draw

**File:** `src/video_core/renderer_vulkan/vk_rasterizer.h:106-111`

`ResetBindings()` iterates all bound images and clears their binding info via cache-unfriendly random writes into the texture cache slot pool. Called every draw call.

**Fix:** Replace with generation counter. Add `u32 current_bind_generation` to Rasterizer and `u32 bind_generation` to Image binding. `is_bound` becomes `image.bind_generation == current_bind_generation`. Reset becomes `++current_bind_generation; bound_images.clear()` — O(1).

The `is_target` flag needs separate handling via a small fixed-size array indexed by render target slot (max 9 entries).

**Status:** Not started

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

**Status:** Not started

---

### Phase 4: Higher-Risk, High-Impact

#### 4.1 Async Pipeline Compilation

**File:** `src/video_core/renderer_vulkan/vk_pipeline_cache.h`

Pipeline compilation (`vkCreateGraphicsPipelines` / `vkCreateComputePipelines`) runs synchronously on the render thread. New shaders cause 10-100ms frame hitches.

**Fix:** Add compilation thread pool (2-4 threads). On cache miss, enqueue job and return `nullptr`. Rasterizer skips draw when pipeline unavailable. Next frame uses compiled result. Make opt-in via config.

**Risks:** Visible pop-in, thread safety for `tsl::robin_map` caches, game compatibility.

**Status:** Not started

#### 4.2 InsertPermut Reallocation Guard

**File:** `src/video_core/renderer_vulkan/vk_pipeline_cache.h:66`

`modules.resize(std::max(modules.size(), perm_idx + 1))` can cause heap allocation when `perm_idx >= MaxPermutations(8)` and default-constructs intermediate elements.

**Fix:** Add `ASSERT(perm_idx < MaxPermutations)` and pre-reserve.

**Status:** Not started

---

## Performance Optimization Roadmap

| # | Issue | File(s) | Risk | Impact | Effort | Status |
|---|-------|---------|------|--------|--------|--------|
| 0.1 | Dead GC (memory leak bug) | `buffer_cache.cpp:841` | Very Low | Critical | 1 line | Not started |
| 1.1 | O(n) vertex buffer lookup | `buffer_cache.cpp:223` | Very Low | Medium-High | 3 lines | Not started |
| 2.1 | Broad pipeline barriers | `buffer_cache.cpp:326,659` | Medium | High | ~20 lines | Not started |
| 2.2 | Duplicate FindImage calls | `vk_rasterizer.cpp:679` | Medium | Medium | ~15 lines | Not started |
| 3.1 | O(n) ResetBindings | `vk_rasterizer.h:106` | Medium | Medium | ~20 lines | Not started |
| 3.2 | Dirty flags bitmask | `vk_scheduler.h:88` | Low | Low-Medium | ~50 lines | Not started |
| 4.1 | Async pipeline compilation | `vk_pipeline_cache.h` | High | High | ~200+ lines | Not started |
| 4.2 | InsertPermut guard | `vk_pipeline_cache.h:66` | Very Low | Low | 2 lines | Not started |

Items 0.1 and 1.1 can ship together with near-zero regression risk. Item 2.1 requires synchronization validation. Items 2.2-3.2 are independent and can be developed in parallel. Item 4.1 is a standalone feature branch.

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

## Measurement Protocol

1. **Baseline:** Select 3 representative titles. Record 60-second Tracy captures at fixed scenes. Extract: mean/P99 frame time, GPU active time, VRAM usage, draw calls/frame.
2. **Per-change:** Apply single optimization, re-record identical captures. Reject if P99 regresses >5%.
3. **Validation:** Zero new Vulkan validation errors. Synchronization validation for barrier changes.
4. **Regression:** 10+ titles through boot + first interactive scene. Automated screenshot diff where available.
5. **Memory:** 30-minute play session with VRAM monitoring after GC fix.
