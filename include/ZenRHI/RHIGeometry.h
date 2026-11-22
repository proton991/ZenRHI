#pragma once
#include <cstdint>
#include <concepts>
#include <cassert>
namespace zen
{
struct RHIOffset3D
{
    int32_t x{}, y{}, z{};
    constexpr RHIOffset3D() = default;
    explicit constexpr RHIOffset3D(int32_t value) : x(value), y(value), z(value) {}
    template <std::integral X, std::integral Y, std::integral Z> constexpr RHIOffset3D(X xValue, Y yValue, Z zValue) :
        x(static_cast<int32_t>(xValue)), y(static_cast<int32_t>(yValue)), z(static_cast<int32_t>(zValue))
    {}
    constexpr int32_t operator[](unsigned axis) const
    {
        assert(axis < 3);
        return axis == 0 ? x : axis == 1 ? y : z;
    }
    bool operator==(const RHIOffset3D&) const = default;
};
template <class T> struct Rect2
{
    T minX, maxX;
    T minY, maxY;

    Rect2() : minX(0), maxX(0), minY(0), maxY(0) {}

    Rect2(T width, T height) : minX(0), maxX(width), minY(0), maxY(height) {}

    Rect2(T _minX, T _maxX, T _minY, T _maxY) : minX(_minX), maxX(_maxX), minY(_minY), maxY(_maxY) {}

    bool operator==(const Rect2& b) const
    {
        return minX == b.minX && minY == b.minY && maxX == b.maxX && maxY == b.maxY;
    }

    bool operator!=(const Rect2& b) const
    {
        return !(*this == b);
    }

    [[nodiscard]] T Width() const
    {
        return maxX - minX;
    }

    [[nodiscard]] T Height() const
    {
        return maxY - minY;
    }
};

} // namespace zen
