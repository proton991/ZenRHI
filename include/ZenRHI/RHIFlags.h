#pragma once
#include <cstdint>
#include <type_traits>
#include "ZenFoundation/Templates/BitField.h"

namespace zen
{
// Flag sets are specific to their enum. Keep the numeric representation for native API conversion.
template <class E> class RHIFlags
{
public:
    constexpr RHIFlags() = default;
    constexpr RHIFlags(E value) : m_bits(value) {}
    constexpr explicit RHIFlags(int64_t value) : m_bits(value) {}
    RHIFlags& operator=(int64_t value)
    {
        m_bits = value;
        return *this;
    }

    template <class U> RHIFlags(RHIFlags<U>)            = delete;
    template <class U> RHIFlags& operator=(RHIFlags<U>) = delete;
    operator int64_t() const
    {
        return m_bits;
    }
    bool IsEmpty() const
    {
        return m_bits.IsEmpty();
    }
    bool HasFlag(E flag) const
    {
        return m_bits.HasFlag(flag);
    }
    template <class... Flags> bool HasAnyFlag(Flags... flags) const
    {
        return m_bits.HasAnyFlag(flags...);
    }
    RHIFlags& SetFlag(E flag)
    {
        m_bits.SetFlag(flag);
        return *this;
    }
    RHIFlags& SetFlag(RHIFlags flags)
    {
        m_bits.SetFlag(flags.m_bits);
        return *this;
    }
    template <class... Flags> RHIFlags& SetFlags(Flags... flags)
    {
        m_bits.SetFlags(flags...);
        return *this;
    }
    void ClearFlag(E flag)
    {
        m_bits.ClearFlag(flag);
    }
    void Clear()
    {
        m_bits.Clear();
    }
    friend RHIFlags operator|(RHIFlags a, RHIFlags b)
    {
        return RHIFlags(int64_t(a) | int64_t(b));
    }
    friend RHIFlags operator^(RHIFlags a, RHIFlags b)
    {
        return RHIFlags(int64_t(a) ^ int64_t(b));
    }

private:
    BitField<E> m_bits;
};

enum class RHIAccelerationStructureInstanceFlagBits : uint32_t;
enum class RHIAccessFlagBits : uint32_t;
enum class RHIBufferUsageFlagBits : uint32_t;
enum class RHIColorComponent : uint32_t;
enum class RHIPipelineStageFlagBits : uint32_t;
enum class RHIShaderStageFlagBits : uint32_t;
enum class RHITextureAspectFlagBits : uint32_t;
enum class RHITextureUsageFlagBits : uint32_t;

template <class E>
    requires(std::is_same_v<E, RHIAccelerationStructureInstanceFlagBits> || std::is_same_v<E, RHIAccessFlagBits>
             || std::is_same_v<E, RHIBufferUsageFlagBits> || std::is_same_v<E, RHIColorComponent>
             || std::is_same_v<E, RHIPipelineStageFlagBits> || std::is_same_v<E, RHIShaderStageFlagBits>
             || std::is_same_v<E, RHITextureAspectFlagBits> || std::is_same_v<E, RHITextureUsageFlagBits>)
RHIFlags<E> operator|(E lhs, E rhs)
{
    return RHIFlags<E>(lhs) | RHIFlags<E>(rhs);
}
} // namespace zen
