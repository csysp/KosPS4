// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <variant>
#include <vector>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
        u64 spec_hash{};
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    tsl::robin_map<u64, u8> perm_map; // spec_hash -> permutation index for O(1) lookup

    Program() = default;
    Program(Shader::Stage stage, Shader::LogicalStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        const u64 hash = spec.Hash(); // compute before move
        perm_map.emplace(hash, static_cast<u8>(modules.size()));
        modules.emplace_back(module, std::move(spec), hash);
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        ASSERT(perm_idx < MaxPermutations);
        const u64 hash = spec.Hash(); // compute before move
        modules.reserve(MaxPermutations);
        modules.resize(std::max(modules.size(), perm_idx + 1));
        modules[perm_idx] = {module, std::move(spec), hash};
        perm_map.insert_or_assign(hash, static_cast<u8>(perm_idx));
    }
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline();

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(Shader::Stage stage, Shader::LogicalStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::Stage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    // -- Async pipeline compilation --

    /// Job captured on the render thread; compiled on a background thread.
    struct AsyncGraphicsJob {
        GraphicsPipelineKey key;
        std::array<const Shader::Info*, MaxShaderStages> infos;
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos;
        std::array<vk::ShaderModule, MaxShaderStages> modules;
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
    };
    struct AsyncComputeJob {
        ComputePipelineKey key;
        const Shader::Info* info;
        vk::ShaderModule module;
    };
    using AsyncJob = std::variant<AsyncGraphicsJob, AsyncComputeJob>;

    /// Compiled pipeline posted back from a compile thread to the render thread.
    struct CompletedGraphics {
        GraphicsPipelineKey key;
        std::unique_ptr<GraphicsPipeline> pipeline;
        GraphicsPipeline::SerializationSupport sdata;
    };
    struct CompletedCompute {
        ComputePipelineKey key;
        std::unique_ptr<ComputePipeline> pipeline;
        ComputePipeline::SerializationSupport sdata;
    };

    /// Drain completed async pipelines into the pipeline maps (render thread only).
    void FlushCompletedPipelines();
    /// Thread function for background compile workers.
    void CompileThreadFunc(std::stop_token stop);

    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::Stage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::Stage stage, Shader::LogicalStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    // True when graphics_key and infos[]/modules[] are valid for the current GPU state.
    // Set false on any graphics register change; set true after a successful RefreshGraphicsKey.
    bool graphics_key_valid{false};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    // Async compilation (render-thread-only maps, no lock needed)
    tsl::robin_map<GraphicsPipelineKey, u8> enqueued_graphics;
    tsl::robin_map<ComputePipelineKey, u8> enqueued_compute;

    // Job queue shared between render thread (producer) and compile threads (consumers)
    std::mutex job_mutex;
    std::condition_variable_any job_cv;
    std::queue<AsyncJob> job_queue;

    // Result queue: compile threads produce, render thread consumes in FlushCompletedPipelines
    std::mutex result_mutex;
    std::vector<CompletedGraphics> completed_graphics;
    std::vector<CompletedCompute> completed_compute;

    std::vector<std::jthread> compile_threads;
};

} // namespace Vulkan
