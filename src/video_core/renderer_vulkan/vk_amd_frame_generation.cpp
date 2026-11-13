// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_amd_frame_generation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_ocarina_camera_pose.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ffx_runtime.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

#ifdef _WIN32
#include "api/include/vk/ffx_api_vk.h"
#include "framegeneration/include/ffx_framegeneration.h"
#endif

namespace Vulkan {

#ifdef _WIN32
namespace {
constexpr VkImageSubresourceRange COLOR{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange DEPTH{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
constexpr VkFormat SCENE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr u32 STANDALONE = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;

VkImageMemoryBarrier Barrier(VkImage image, VkImageSubresourceRange range, VkImageLayout from,
                             VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    return {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
}

FfxApiResource Resource(VkImage image, VkFormat format, VkExtent2D extent, u32 usage, u32 state) {
    FfxApiResource resource{};
    resource.resource = reinterpret_cast<void*>(image);
    resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = ffxApiGetSurfaceFormatVK(format);
    resource.description.width = extent.width;
    resource.description.height = extent.height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.description.usage = usage;
    resource.state = state;
    return resource;
}

bool SameExtent(VkExtent2D lhs, VkExtent2D rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height;
}
} // namespace

struct AmdFrameGeneration::Impl {
    using Clock = std::chrono::steady_clock;
    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    std::shared_ptr<const FfxRuntime::Api> api;
    ffxContext context{};
    vk::Image output;
    vk::Image hudless;
    VkRect2D game_area{};
    VkExtent2D display{};
    VkExtent2D render{};
    VkExtent2D configured_display{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkFormat configured_format{VK_FORMAT_UNDEFINED};
    std::optional<u64> prepared;
    std::optional<u64> previous;
    Clock::time_point last_time{};
    bool reset{};
    std::atomic_bool failed{};
    bool generated{};

    FfxApiRect2D GenerationRect() const {
        return {game_area.offset.x, game_area.offset.y, static_cast<s32>(game_area.extent.width),
                static_cast<s32>(game_area.extent.height)};
    }

    Impl(const Device& device_, Scheduler& scheduler_, MemoryAllocator& allocator_)
        : device{device_}, scheduler{scheduler_}, allocator{allocator_} {}

    ~Impl() {
        Destroy();
    }

    void Destroy() {
        if (context) {
            scheduler.Finish();
            api->destroy_context(&context, nullptr);
            context = nullptr;
        }
        output = {};
        hudless = {};
        prepared.reset();
        previous.reset();
    }

    bool Check(ffxReturnCode_t code, const char* step) {
        if (code == FFX_API_RETURN_OK) {
            return true;
        }
        LOG_ERROR(Render_Vulkan, "AMD FSR Frame Generation: {} failed ({})", step, code);
        failed = true;
        return false;
    }

    bool Create(VkExtent2D render_extent) {
        Destroy();
        if (device.GetDriverID() != VK_DRIVER_ID_AMD_PROPRIETARY) {
            LOG_WARNING(Render_Vulkan, "AMD FSR Frame Generation needs the AMD Windows driver");
            return false;
        }
        api = FfxRuntime::AcquireFrameGeneration();
        if (!api || !api->configure) {
            return false;
        }

        VkDeviceContext vk_context{
            .vkDevice = *device.GetLogical(),
            .vkPhysicalDevice = static_cast<VkPhysicalDevice>(device.GetPhysical()),
            .vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr,
        };
        u64 count{};
        ffxQueryDescGetVersions query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        query.device = &vk_context;
        query.outputCount = &count;
        if (!Check(api->query(nullptr, &query.header), "query providers") || count == 0) {
            LOG_WARNING(Render_Vulkan, "AMD FSR Frame Generation: no providers available");
            return false;
        }
        std::vector<u64> ids(count);
        std::vector<const char*> names(count);
        query.versionIds = ids.data();
        query.versionNames = names.data();
        if (!Check(api->query(nullptr, &query.header), "read providers")) {
            return false;
        }
        const auto provider = std::ranges::find_if(names, [](const char* name) {
            return name && std::string_view{name} == "3.1.6";
        });
        if (provider == names.end()) {
            LOG_WARNING(Render_Vulkan, "AMD FSR Frame Generation: no 3.1.6 provider available");
            return false;
        }
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.deviceContext = vk_context;
        ffxOverrideVersion override_version{};
        override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        override_version.header.pNext = &backend.header;
        override_version.versionId = ids[static_cast<size_t>(provider - names.begin())];
        ffxCreateContextDescFrameGenerationVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        ffxCreateContextDescFrameGenerationHudless scene_format{};
        scene_format.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS,
                               &override_version.header};
        scene_format.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(SCENE_FORMAT);
        version.header.pNext = &scene_format.header;
        version.version = FFX_FRAMEGENERATION_VERSION;
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &version.header;
        create.displaySize = {display.width, display.height};
        create.maxRenderSize = {render_extent.width, render_extent.height};
        create.backBufferFormat = ffxApiGetSurfaceFormatVK(format);
        if (!Check(api->create_context(&context, &create.header, nullptr), "create context")) {
            return false;
        }

        if (!device.IsFormatSupported(SCENE_FORMAT,
                                      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                                      FormatType::Optimal)) {
            LOG_WARNING(Render_Vulkan, "AMD FSR Frame Generation: HUD-less blit unsupported");
            return false;
        }
        try {
            const VkImageCreateInfo image_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent = {display.width, display.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            output = allocator.CreateImage(image_info);
            VkImageCreateInfo scene_info = image_info;
            scene_info.format = SCENE_FORMAT;
            scene_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            hudless = allocator.CreateImage(scene_info);
        } catch (const vk::Exception& error) {
            LOG_ERROR(Render_Vulkan, "AMD FSR Frame Generation: output allocation failed ({})",
                      error.what());
            return false;
        }
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([image = *output, scene = *hudless](vk::CommandBuffer cmdbuf) {
            const std::array barriers{
                Barrier(image, COLOR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        0, VK_ACCESS_SHADER_WRITE_BIT),
                Barrier(scene, COLOR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, {}, {}, barriers);
            cmdbuf.ClearColorImage(scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {}, COLOR);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {},
                Barrier(scene, COLOR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT));
        });
        render = render_extent;
        configured_display = display;
        configured_format = format;
        LOG_INFO(Render_Vulkan, "AMD FSR Frame Generation: provider {}, {}x{} -> {}x{}",
                 *provider, render.width, render.height, display.width, display.height);
        return true;
    }
};
#else
struct AmdFrameGeneration::Impl {};
#endif

AmdFrameGeneration::AmdFrameGeneration([[maybe_unused]] const Device& device,
                                       [[maybe_unused]] Scheduler& scheduler,
                                       [[maybe_unused]] MemoryAllocator& allocator)
#ifdef _WIN32
    : impl{std::make_unique<Impl>(device, scheduler, allocator)}
#endif
{}

AmdFrameGeneration::~AmdFrameGeneration() = default;

void AmdFrameGeneration::SetOutput([[maybe_unused]] VkExtent2D extent,
                                   [[maybe_unused]] VkFormat format,
                                   [[maybe_unused]] VkRect2D game_area) {
#ifdef _WIN32
    auto& state = *impl;
    if (!SameExtent(extent, state.display) || format != state.format ||
        !SameExtent(game_area.extent, state.game_area.extent) ||
        game_area.offset.x != state.game_area.offset.x || game_area.offset.y != state.game_area.offset.y) {
        state.prepared.reset();
        state.previous.reset();
        if (extent.width == 0 || extent.height == 0) {
            state.Destroy();
        }
    }
    state.display = extent;
    state.format = format;
    state.game_area = game_area;
#endif
}

void AmdFrameGeneration::Prepare([[maybe_unused]] VkImage depth, [[maybe_unused]] VkImage motion,
                                 [[maybe_unused]] VkExtent2D render_extent,
                                 [[maybe_unused]] const std::array<f32, 16>& view_projection,
                                 [[maybe_unused]] std::array<f32, 2> jitter,
                                 [[maybe_unused]] u64 frame, [[maybe_unused]] bool reset,
                                 [[maybe_unused]] VkImage scene,
                                 [[maybe_unused]] VkExtent2D scene_extent) {
#ifdef _WIN32
    auto& state = *impl;
    state.prepared.reset();
    const auto area = state.game_area;
    if (state.failed || !scene || !area.extent.width || !area.extent.height ||
        area.offset.x < 0 || area.offset.y < 0 ||
        u64{static_cast<u32>(area.offset.x)} + area.extent.width > state.display.width ||
        u64{static_cast<u32>(area.offset.y)} + area.extent.height > state.display.height ||
        render_extent.width > area.extent.width || render_extent.height > area.extent.height ||
        (state.format != VK_FORMAT_B8G8R8A8_UNORM && state.format != VK_FORMAT_R8G8B8A8_UNORM)) {
        return;
    }
    const auto pose = GetOcarinaCameraPose(view_projection);
    if (!pose) {
        return;
    }
    if (!state.context || !SameExtent(state.render, render_extent) ||
        !SameExtent(state.configured_display, state.display) ||
        state.configured_format != state.format) {
        if (!state.Create(render_extent)) {
            state.failed = true;
            return;
        }
    }

    const auto now = Impl::Clock::now();
    const f32 elapsed = state.previous
        ? std::chrono::duration<f32, std::milli>(now - state.last_time).count()
        : 16.67f;
    state.reset = reset || !state.previous || *state.previous + 1 != frame || elapsed > 100.0f;
    state.last_time = now;
    state.prepared = frame;

    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.frameGenerationEnabled = true;
    config.flags = STANDALONE;
    config.generationRect = state.GenerationRect();
    config.HUDLessColor = Resource(*state.hudless, SCENE_FORMAT, state.display, 0,
                                   FFX_API_RESOURCE_STATE_COMPUTE_READ);
    config.frameID = frame;

    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = frame;
    prepare.flags = STANDALONE;
    prepare.renderSize = {render_extent.width, render_extent.height};
    prepare.jitterOffset = {jitter[0], jitter[1]};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeDelta = elapsed;
    prepare.reset = state.reset;
    prepare.cameraNear = static_cast<f32>(pose->near_plane);
    prepare.cameraFar = static_cast<f32>(pose->far_plane);
    prepare.cameraFovAngleVertical = static_cast<f32>(pose->vertical_fov);
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = Resource(depth, VK_FORMAT_D32_SFLOAT, render_extent,
                             FFX_API_RESOURCE_USAGE_DEPTHTARGET,
                             FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepare.motionVectors = Resource(motion, VK_FORMAT_R16G16_SFLOAT, render_extent, 0,
                                    FFX_API_RESOURCE_STATE_COMPUTE_READ);
    std::ranges::copy(pose->position, prepare.cameraPosition);
    std::ranges::copy(pose->right, prepare.cameraRight);
    std::ranges::copy(pose->up, prepare.cameraUp);
    std::ranges::copy(pose->forward, prepare.cameraForward);

    state.scheduler.RequestOutsideRenderPassOperationContext();
    state.scheduler.Record([&state, config, prepare, depth, motion, scene, scene_extent,
                            area, hudless = *state.hudless](vk::CommandBuffer cmdbuf) {
        if (state.failed ||
            !state.Check(state.api->configure(&state.context, &config.header), "configure")) {
            return;
        }
        const std::array blit_before{
            Barrier(scene, COLOR, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            Barrier(hudless, COLOR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, blit_before);
        const VkImageBlit region{
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0}, {static_cast<s32>(scene_extent.width),
                                       static_cast<s32>(scene_extent.height), 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{area.offset.x, area.offset.y, 0},
                           {area.offset.x + static_cast<s32>(area.extent.width),
                            area.offset.y + static_cast<s32>(area.extent.height), 1}},
        };
        cmdbuf.BlitImage(scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hudless,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region, VK_FILTER_LINEAR);
        const std::array blit_after{
            Barrier(scene, COLOR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
            Barrier(hudless, COLOR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, {}, {}, blit_after);
        const std::array before{
            Barrier(depth, DEPTH, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            Barrier(motion, COLOR, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, before);
        auto recorded = prepare;
        recorded.commandList = reinterpret_cast<void*>(*cmdbuf);
        state.Check(state.api->dispatch(&state.context, &recorded.header), "prepare");
        const std::array after{
            Barrier(depth, DEPTH, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
            Barrier(motion, COLOR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, after);
    });
    state.scheduler.InvalidateState();
#endif
}

bool AmdFrameGeneration::Interpolate([[maybe_unused]] VkImage color, [[maybe_unused]] u64 frame) {
#ifdef _WIN32
    auto& state = *impl;
    if (state.failed || state.prepared != frame) {
        return false;
    }
    state.prepared.reset();
    state.scheduler.RequestOutsideRenderPassOperationContext();
    state.scheduler.Record([&state, color, frame](vk::CommandBuffer cmdbuf) {
        if (state.failed) {
            return;
        }
        const std::array before{
            Barrier(color, COLOR, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            Barrier(*state.output, COLOR, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, before);
        ffxDispatchDescFrameGeneration dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        dispatch.commandList = reinterpret_cast<void*>(*cmdbuf);
        dispatch.presentColor = Resource(color, state.format, state.display, 0,
                                         FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.outputs[0] = Resource(*state.output, state.format, state.display,
                                       FFX_API_RESOURCE_USAGE_UAV,
                                       FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.numGeneratedFrames = 1;
        dispatch.reset = state.reset;
        dispatch.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.generationRect = state.GenerationRect();
        dispatch.frameID = frame;
        state.Check(state.api->dispatch(&state.context, &dispatch.header), "interpolate");
        const std::array after{
            Barrier(color, COLOR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, after);
    });
    state.scheduler.WaitWorker();
    state.scheduler.InvalidateState();
    if (state.failed) {
        return false;
    }
    state.previous = frame;
    if (!state.reset && !state.generated) {
        LOG_INFO(Render_Vulkan, "AMD FSR Frame Generation: first interpolated frame recorded");
        state.generated = true;
    }
    return !state.reset;
#else
    return false;
#endif
}

void AmdFrameGeneration::CopyOutput([[maybe_unused]] VkImage destination,
                                    [[maybe_unused]] VkImage real_frame) {
#ifdef _WIN32
    auto& state = *impl;
    state.scheduler.Record([source = *state.output, destination, real_frame,
                            extent = state.display, area = state.game_area](vk::CommandBuffer cmdbuf) {
        const bool has_margins = !SameExtent(extent, area.extent);
        const std::array before{
            Barrier(source, COLOR, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            Barrier(destination, COLOR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);
        const VkImageSubresourceLayers layer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        if (has_margins) {
            // SDK output outside generationRect is not defined. Keep the real frame's margins.
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {},
                Barrier(real_frame, COLOR, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
            const VkImageCopy full{layer, {}, layer, {}, {extent.width, extent.height, 1}};
            cmdbuf.CopyImage(real_frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, full);
            const VkMemoryBarrier copied{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, copied);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {},
                Barrier(real_frame, COLOR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT));
        }
        const VkOffset3D offset{area.offset.x, area.offset.y, 0};
        const VkImageCopy region{layer, offset, layer, offset,
                                 {area.extent.width, area.extent.height, 1}};
        cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
        const std::array after{
            Barrier(source, COLOR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT),
            Barrier(destination, COLOR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, after);
    });
#endif
}

bool AmdFrameGeneration::Failed() const {
#ifdef _WIN32
    return impl->failed;
#else
    return true;
#endif
}

} // namespace Vulkan
