// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

class AmdFrameGeneration {
public:
    AmdFrameGeneration(const Device& device, Scheduler& scheduler, MemoryAllocator& allocator);
    ~AmdFrameGeneration();

    void SetOutput(VkExtent2D extent, VkFormat format, VkRect2D game_area = {});
    void Prepare(VkImage depth, VkImage motion, VkExtent2D render_extent,
                 const std::array<f32, 16>& view_projection, std::array<f32, 2> jitter, u64 frame,
                 bool reset, VkImage scene, VkExtent2D scene_extent);
    [[nodiscard]] bool Interpolate(VkImage color, u64 frame);
    void CopyOutput(VkImage destination, VkImage real_frame);
    [[nodiscard]] bool Failed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
