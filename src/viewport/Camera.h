#pragma once
#include "math/Math.h"

#include <QString>

namespace facet {

enum class ViewKind { Persp, Top, Front, Side };

// Maya-style camera: orbits a centre of interest (COI). Ortho views are locked to an axis.
struct Camera {
    ViewKind kind = ViewKind::Persp;
    Vec3 coi{0, 0, 0};
    float distance = 45.0f;
    float azimuth = 45.0f;    // degrees around +Y
    float elevation = 27.9f;  // degrees above the XZ plane
    float fovY = 37.8f;       // ~35mm lens on a 16:9 gate
    float orthoHeight = 20.0f;

    static Camera make(ViewKind k)
    {
        Camera c;
        c.kind = k;
        if (k != ViewKind::Persp) { c.distance = 1000.0f; c.orthoHeight = 20.0f; }
        return c;
    }

    QString name() const
    {
        switch (kind) {
        case ViewKind::Top: return "top";
        case ViewKind::Front: return "front";
        case ViewKind::Side: return "side";
        default: return "persp";
        }
    }
    bool ortho() const { return kind != ViewKind::Persp; }

    Vec3 forward() const
    {
        switch (kind) {
        case ViewKind::Top: return {0, -1, 0};
        case ViewKind::Front: return {0, 0, -1};
        case ViewKind::Side: return {-1, 0, 0};
        default: {
            float az = radians(azimuth), el = radians(elevation);
            return -Vec3{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
        }
        }
    }
    Vec3 worldUp() const { return kind == ViewKind::Top ? Vec3{0, 0, -1} : Vec3{0, 1, 0}; }
    Vec3 right() const
    {
        Vec3 r = cross(forward(), worldUp());
        if (length(r) < 1e-6f) r = {1, 0, 0};
        return normalize(r);
    }
    Vec3 up() const { return cross(right(), forward()); }
    Vec3 eye() const { return coi - forward() * distance; }

    Mat4 view() const { return Mat4::lookAt(eye(), coi, up()); }
    float nearClip() const { return ortho() ? 0.1f : std::max(0.01f, distance * 0.002f); }
    float farClip() const { return ortho() ? 5000.0f : distance * 200.0f + 1000.0f; }
    Mat4 proj(float aspect) const
    {
        if (ortho()) {
            float h = orthoHeight * 0.5f, w = h * aspect;
            return Mat4::ortho(-w, w, -h, h, nearClip(), farClip());
        }
        return Mat4::perspective(fovY, aspect, nearClip(), farClip());
    }
    // World units per pixel at the COI depth (or at `depth` along the view direction).
    float worldPerPixel(float viewHeightPx, float depth = -1.0f) const
    {
        if (ortho()) return orthoHeight / viewHeightPx;
        float d = depth > 0 ? depth : distance;
        return 2.0f * d * std::tan(radians(fovY) * 0.5f) / viewHeightPx;
    }
    // Ray through normalized device coords (-1..1).
    Ray ray(float ndcX, float ndcY, float aspect) const
    {
        Vec3 f = forward(), r = right(), u = up();
        if (ortho()) {
            float h = orthoHeight * 0.5f;
            Vec3 o = eye() + r * (ndcX * h * aspect) + u * (ndcY * h);
            return {o, f};
        }
        float t = std::tan(radians(fovY) * 0.5f);
        Vec3 d = normalize(f + r * (ndcX * t * aspect) + u * (ndcY * t));
        return {eye(), d};
    }

    void tumble(float dx, float dy)
    {
        if (ortho()) return;
        azimuth -= dx * 0.3f;
        elevation = std::clamp(elevation + dy * 0.3f, -89.5f, 89.5f);
    }
    void track(float dx, float dy, float viewHeightPx)
    {
        float k = worldPerPixel(viewHeightPx);
        coi = coi - right() * (dx * k) + up() * (dy * k);
    }
    void dolly(float amount)
    {
        float f = std::exp(-amount);
        if (ortho()) orthoHeight = std::clamp(orthoHeight * f, 1e-3f, 1e5f);
        else distance = std::clamp(distance * f, 1e-3f, 1e6f);
    }
    void frame(const AABB& box, float aspect)
    {
        if (!box.valid()) return;
        coi = box.center();
        float radius = std::max(length(box.size()) * 0.5f, 1e-3f);
        if (ortho()) {
            orthoHeight = radius * 2.2f / std::min(1.0f, aspect);
        } else {
            float half = radians(fovY) * 0.5f;
            if (aspect < 1.0f) half = std::atan(std::tan(half) * aspect);
            distance = radius / std::sin(half) * 1.05f;
        }
    }
};

} // namespace facet
