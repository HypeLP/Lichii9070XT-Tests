// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <optional>

#include "common/common_types.h"

namespace Vulkan {

using Matrix4 = std::array<std::array<f64, 4>, 4>;
using Vector3 = std::array<f64, 3>;

struct OcarinaCameraPose {
    Matrix4 view_projection;
    Vector3 position;
    Vector3 right;
    Vector3 up;
    Vector3 forward;
    f64 near_plane;
    f64 far_plane;
    f64 vertical_fov;
};

namespace OcarinaCameraMath {

inline Matrix4 ToMatrix(const std::array<f32, 16>& values) {
    Matrix4 matrix{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            matrix[row][col] = values[row * 4 + col];
        }
    }
    return matrix;
}

inline f64 Dot(const Vector3& a, const Vector3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline f64 Determinant(const std::array<Vector3, 3>& rows) {
    return rows[0][0] * (rows[1][1] * rows[2][2] - rows[1][2] * rows[2][1]) -
           rows[0][1] * (rows[1][0] * rows[2][2] - rows[1][2] * rows[2][0]) +
           rows[0][2] * (rows[1][0] * rows[2][1] - rows[1][1] * rows[2][0]);
}

inline std::optional<Vector3> Normalize(const Vector3& vector) {
    const f64 length = std::sqrt(Dot(vector, vector));
    if (!std::isfinite(length) || length < 1e-9) {
        return std::nullopt;
    }
    return Vector3{vector[0] / length, vector[1] / length, vector[2] / length};
}

/// Takes out of a vector its part along a unit direction.
inline Vector3 Without(Vector3 vector, const Vector3& direction) {
    const f64 amount = Dot(vector, direction);
    for (size_t i = 0; i < 3; ++i) {
        vector[i] -= amount * direction[i];
    }
    return vector;
}

} // namespace OcarinaCameraMath

inline std::optional<OcarinaCameraPose> GetOcarinaCameraPose(const std::array<f32, 16>& matrix) {
    using namespace OcarinaCameraMath;
    Matrix4 view_projection = ToMatrix(matrix);
    const f64 scale =
        std::hypot(view_projection[3][0], view_projection[3][1], view_projection[3][2]);
    if (!std::isfinite(scale) || scale < 1e-12) {
        return std::nullopt;
    }
    // Clip w is then the distance along the view direction.
    for (auto& row : view_projection) {
        for (f64& value : row) {
            value /= scale;
        }
    }
    const auto axis = [&](size_t row) {
        return Vector3{view_projection[row][0], view_projection[row][1], view_projection[row][2]};
    };
    const Vector3 forward = axis(3);
    const Vector3 clip_x = axis(0);
    const Vector3 clip_y = axis(1);
    // The eye is where clip x, y and w are all zero (Cramer's rule).
    const std::array<Vector3, 3> system{clip_x, clip_y, forward};
    const Vector3 rhs{-view_projection[0][3], -view_projection[1][3], -view_projection[3][3]};
    const f64 det = Determinant(system);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) {
        return std::nullopt;
    }
    Vector3 position{};
    for (size_t column = 0; column < 3; ++column) {
        std::array<Vector3, 3> replaced = system;
        for (size_t i = 0; i < 3; ++i) {
            replaced[i][column] = rhs[i];
        }
        position[column] = Determinant(replaced) / det;
    }
    const std::optional<Vector3> right = Normalize(Without(clip_x, forward));
    if (!right) {
        return std::nullopt;
    }
    const std::optional<Vector3> up = Normalize(Without(Without(clip_y, forward), *right));
    if (!up) {
        return std::nullopt;
    }

    const Vector3 clip_z = axis(2);
    const f64 scale_x = Dot(clip_x, *right);
    const f64 scale_y = Dot(clip_y, *up);
    const f64 a = 0.5 * (Dot(clip_z, forward) + 1.0);
    const f64 b = 0.5 * (Dot(clip_z, position) + view_projection[2][3]);
    const f64 near_plane = -b / a;
    f64 far_plane = b / (1.0 - a);
    if (!std::isfinite(near_plane) || near_plane <= 0.0 || scale_x <= 0.0 || scale_y <= 0.0) {
        return std::nullopt;
    }
    if (!std::isfinite(far_plane) || far_plane <= near_plane) {
        far_plane = near_plane * 100000.0;
    }
    return OcarinaCameraPose{view_projection, position, *right, *up, forward,
                             near_plane, far_plane, 2.0 * std::atan(1.0 / scale_y)};
}

} // namespace Vulkan
