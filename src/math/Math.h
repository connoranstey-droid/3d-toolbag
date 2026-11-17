#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace facet {

constexpr float kPi = 3.14159265358979323846f;
inline float radians(float d) { return d * (kPi / 180.0f); }
inline float degrees(float r) { return r * (180.0f / kPi); }

struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(float a, float b) : x(a), y(b) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    float length() const { return std::sqrt(x * x + y * y); }
};
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3& o) const { return !(*this == o); }
};
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline Vec3 mul(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline float length2(const Vec3& v) { return dot(v, v); }
inline Vec3 normalize(const Vec3& v)
{
    float l = length(v);
    return l > 1e-20f ? v / l : Vec3{0, 0, 0};
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    Vec3 xyz() const { return {x, y, z}; }
};

// Column-major 4x4 matrix (OpenGL convention): element(row, col) = m[col*4+row].
struct Mat4 {
    float m[16];
    Mat4() { *this = identity(); }
    static Mat4 identity()
    {
        Mat4 r(0);
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    explicit Mat4(int) { std::fill(m, m + 16, 0.0f); }
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
    const float* data() const { return m; }

    Mat4 operator*(const Mat4& b) const
    {
        Mat4 r(0);
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(rr, k) * b.at(k, c);
                r.at(rr, c) = s;
            }
        return r;
    }
    Vec4 operator*(const Vec4& v) const
    {
        return {at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3) * v.w,
                at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3) * v.w,
                at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3) * v.w,
                at(3, 0) * v.x + at(3, 1) * v.y + at(3, 2) * v.z + at(3, 3) * v.w};
    }
    Vec3 transformPoint(const Vec3& p) const
    {
        return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3),
                at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3),
                at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3)};
    }
    Vec3 transformDir(const Vec3& d) const
    {
        return {at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z,
                at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
                at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
    }
    // Applies the full projective transform including perspective divide.
    Vec3 project(const Vec3& p) const
    {
        Vec4 r = (*this) * Vec4(p.x, p.y, p.z, 1.0f);
        float w = std::abs(r.w) > 1e-20f ? r.w : 1e-20f;
        return {r.x / w, r.y / w, r.z / w};
    }
    Mat4 transposed() const
    {
        Mat4 r(0);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.at(i, j) = at(j, i);
        return r;
    }
    Mat4 inverse() const;

    static Mat4 translate(const Vec3& t)
    {
        Mat4 r;
        r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s)
    {
        Mat4 r;
        r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
        return r;
    }
    static Mat4 rotateX(float deg)
    {
        float c = std::cos(radians(deg)), s = std::sin(radians(deg));
        Mat4 r;
        r.at(1, 1) = c; r.at(1, 2) = -s; r.at(2, 1) = s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 rotateY(float deg)
    {
        float c = std::cos(radians(deg)), s = std::sin(radians(deg));
        Mat4 r;
        r.at(0, 0) = c; r.at(0, 2) = s; r.at(2, 0) = -s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 rotateZ(float deg)
    {
        float c = std::cos(radians(deg)), s = std::sin(radians(deg));
        Mat4 r;
        r.at(0, 0) = c; r.at(0, 1) = -s; r.at(1, 0) = s; r.at(1, 1) = c;
        return r;
    }
    // Rotation applied in X, then Y, then Z order (Maya's default "xyz" rotate order).
    static Mat4 rotateXYZ(const Vec3& deg) { return rotateZ(deg.z) * rotateY(deg.y) * rotateX(deg.x); }
    static Mat4 perspective(float fovyDeg, float aspect, float zn, float zf)
    {
        float f = 1.0f / std::tan(radians(fovyDeg) * 0.5f);
        Mat4 r(0);
        r.at(0, 0) = f / aspect;
        r.at(1, 1) = f;
        r.at(2, 2) = (zf + zn) / (zn - zf);
        r.at(2, 3) = 2.0f * zf * zn / (zn - zf);
        r.at(3, 2) = -1.0f;
        return r;
    }
    static Mat4 ortho(float l, float r_, float b, float t, float zn, float zf)
    {
        Mat4 r;
        r.at(0, 0) = 2.0f / (r_ - l);
        r.at(1, 1) = 2.0f / (t - b);
        r.at(2, 2) = -2.0f / (zf - zn);
        r.at(0, 3) = -(r_ + l) / (r_ - l);
        r.at(1, 3) = -(t + b) / (t - b);
        r.at(2, 3) = -(zf + zn) / (zf - zn);
        return r;
    }
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up)
    {
        Vec3 f = normalize(target - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r;
        r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z;
        r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z;
        r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z;
        r.at(0, 3) = -dot(s, eye);
        r.at(1, 3) = -dot(u, eye);
        r.at(2, 3) = dot(f, eye);
        return r;
    }
};

inline Mat4 Mat4::inverse() const
{
    const float* a = m;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    Mat4 r(0);
    if (std::abs(det) < 1e-30f) return Mat4::identity();
    float id = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * id;
    return r;
}

// Rotation of `deg` degrees about a unit axis (right-handed).
inline Mat4 rotateAxis(const Vec3& axis, float deg)
{
    Vec3 a = normalize(axis);
    float c = std::cos(radians(deg)), s = std::sin(radians(deg)), t = 1 - c;
    Mat4 r;
    r.at(0, 0) = t * a.x * a.x + c;       r.at(0, 1) = t * a.x * a.y - s * a.z; r.at(0, 2) = t * a.x * a.z + s * a.y;
    r.at(1, 0) = t * a.x * a.y + s * a.z; r.at(1, 1) = t * a.y * a.y + c;       r.at(1, 2) = t * a.y * a.z - s * a.x;
    r.at(2, 0) = t * a.x * a.z - s * a.y; r.at(2, 1) = t * a.y * a.z + s * a.x; r.at(2, 2) = t * a.z * a.z + c;
    return r;
}

// Inverse of Mat4::rotateXYZ for a pure rotation matrix (degrees).
inline Vec3 eulerXYZ(const Mat4& m)
{
    float sy = std::clamp(-m.at(2, 0), -1.0f, 1.0f);
    float y = std::asin(sy), x, z;
    if (std::abs(sy) < 0.99999f) {
        x = std::atan2(m.at(2, 1), m.at(2, 2));
        z = std::atan2(m.at(1, 0), m.at(0, 0));
    } else {
        x = std::atan2(-m.at(1, 2), m.at(1, 1));
        z = 0;
    }
    return {degrees(x), degrees(y), degrees(z)};
}

struct AABB {
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    void expand(const Vec3& p) { lo = vmin(lo, p); hi = vmax(hi, p); }
    void expand(const AABB& b) { if (b.valid()) { lo = vmin(lo, b.lo); hi = vmax(hi, b.hi); } }
    bool valid() const { return lo.x <= hi.x; }
    Vec3 center() const { return (lo + hi) * 0.5f; }
    Vec3 size() const { return hi - lo; }
    AABB transformed(const Mat4& m) const
    {
        AABB r;
        if (!valid()) return r;
        for (int i = 0; i < 8; ++i)
            r.expand(m.transformPoint({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z}));
        return r;
    }
};

struct Ray {
    Vec3 origin, dir;
};

} // namespace facet
