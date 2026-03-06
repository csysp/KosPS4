// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance)
    : instance{instance}, master_semaphore{instance}, command_pool{instance, &master_semaphore} {
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = render_state.num_color_attachments > 0
                                 ? render_state.color_attachments.data()
                                 : nullptr,
        .pDepthAttachment = render_state.has_depth ? &render_state.depth_attachment : nullptr,
        .pStencilAttachment = render_state.has_stencil ? &render_state.stencil_attachment : nullptr,
    };

    current_cmdbuf.beginRendering(rendering_info);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    current_cmdbuf.endRendering();
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    if (tick >= master_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    master_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    master_semaphore.Refresh();
    while (!pending_ops.empty() && master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = master_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    EndRendering();
    Check(current_cmdbuf.end());

    const vk::Semaphore timeline = master_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    master_semaphore.Refresh();
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        master_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    if (dirty_flags == 0) {
        return;
    }
    if (dirty_flags & DirtyBit::Viewports) {
        dirty_flags &= ~DirtyBit::Viewports;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_flags & DirtyBit::Scissors) {
        dirty_flags &= ~DirtyBit::Scissors;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_flags & DirtyBit::DepthTestEnabled) {
        dirty_flags &= ~DirtyBit::DepthTestEnabled;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_flags & DirtyBit::DepthWriteEnabled) {
        dirty_flags &= ~DirtyBit::DepthWriteEnabled;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && (dirty_flags & DirtyBit::DepthCompareOp)) {
        dirty_flags &= ~DirtyBit::DepthCompareOp;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_flags & DirtyBit::DepthBoundsTestEnabled) {
        dirty_flags &= ~DirtyBit::DepthBoundsTestEnabled;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && (dirty_flags & DirtyBit::DepthBounds)) {
        dirty_flags &= ~DirtyBit::DepthBounds;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_flags & DirtyBit::DepthBiasEnabled) {
        dirty_flags &= ~DirtyBit::DepthBiasEnabled;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && (dirty_flags & DirtyBit::DepthBias)) {
        dirty_flags &= ~DirtyBit::DepthBias;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_flags & DirtyBit::StencilTestEnabled) {
        dirty_flags &= ~DirtyBit::StencilTestEnabled;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if ((dirty_flags & DirtyBit::StencilFrontOps) && (dirty_flags & DirtyBit::StencilBackOps) &&
            stencil_front_ops == stencil_back_ops) {
            dirty_flags &= ~(DirtyBit::StencilFrontOps | DirtyBit::StencilBackOps);
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_flags & DirtyBit::StencilFrontOps) {
                dirty_flags &= ~DirtyBit::StencilFrontOps;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_flags & DirtyBit::StencilBackOps) {
                dirty_flags &= ~DirtyBit::StencilBackOps;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if ((dirty_flags & DirtyBit::StencilFrontReference) &&
            (dirty_flags & DirtyBit::StencilBackReference) &&
            stencil_front_reference == stencil_back_reference) {
            dirty_flags &= ~(DirtyBit::StencilFrontReference | DirtyBit::StencilBackReference);
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_flags & DirtyBit::StencilFrontReference) {
                dirty_flags &= ~DirtyBit::StencilFrontReference;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_flags & DirtyBit::StencilBackReference) {
                dirty_flags &= ~DirtyBit::StencilBackReference;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if ((dirty_flags & DirtyBit::StencilFrontWriteMask) &&
            (dirty_flags & DirtyBit::StencilBackWriteMask) &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_flags &= ~(DirtyBit::StencilFrontWriteMask | DirtyBit::StencilBackWriteMask);
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_flags & DirtyBit::StencilFrontWriteMask) {
                dirty_flags &= ~DirtyBit::StencilFrontWriteMask;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_flags & DirtyBit::StencilBackWriteMask) {
                dirty_flags &= ~DirtyBit::StencilBackWriteMask;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if ((dirty_flags & DirtyBit::StencilFrontCompareMask) &&
            (dirty_flags & DirtyBit::StencilBackCompareMask) &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_flags &= ~(DirtyBit::StencilFrontCompareMask | DirtyBit::StencilBackCompareMask);
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_flags & DirtyBit::StencilFrontCompareMask) {
                dirty_flags &= ~DirtyBit::StencilFrontCompareMask;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_flags & DirtyBit::StencilBackCompareMask) {
                dirty_flags &= ~DirtyBit::StencilBackCompareMask;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_flags & DirtyBit::PrimitiveRestartEnable) {
        dirty_flags &= ~DirtyBit::PrimitiveRestartEnable;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_flags & DirtyBit::RasterizerDiscardEnable) {
        dirty_flags &= ~DirtyBit::RasterizerDiscardEnable;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_flags & DirtyBit::CullMode) {
        dirty_flags &= ~DirtyBit::CullMode;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_flags & DirtyBit::FrontFace) {
        dirty_flags &= ~DirtyBit::FrontFace;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_flags & DirtyBit::BlendConstants) {
        dirty_flags &= ~DirtyBit::BlendConstants;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_flags & DirtyBit::ColorWriteMasks) {
        dirty_flags &= ~DirtyBit::ColorWriteMasks;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_flags & DirtyBit::LineWidth) {
        dirty_flags &= ~DirtyBit::LineWidth;
        cmdbuf.setLineWidth(line_width);
    }
    if ((dirty_flags & DirtyBit::FeedbackLoopEnabled) &&
        instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_flags &= ~DirtyBit::FeedbackLoopEnabled;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
