#include "interior/sweep.h"

#include "infrastructure/array_util.h"

#include <algorithm>
#include <ranges>

namespace interior {
namespace {

// Each setting's digit of the index, in as many values as the setting runs through.
using Digits = std::array<std::uint32_t, kSweepParameterCount>;

struct Counting
{
    Digits digits;
    std::uint32_t rest;
};

[[nodiscard]] bool IsSwept(const SweepSpec& spec, SweepParameter parameter) noexcept
{
    return spec[static_cast<std::size_t>(parameter)].on;
}

[[nodiscard]] std::uint32_t DigitOf(const Digits& digits, SweepParameter parameter) noexcept
{
    return digits[static_cast<std::size_t>(parameter)];
}

[[nodiscard]] Digits DigitsOf(const SweepSpec& spec, std::uint32_t index) noexcept
{
    static constexpr auto Taken = [] [[nodiscard]] (const Counting& so, std::size_t p, const SweepSpec& spec) noexcept -> Counting {
        const std::uint32_t values = SweepValuesOf(spec, static_cast<SweepParameter>(p));
        return Counting{ infra::WithElement(so.digits, p, so.rest % values), so.rest / values };
    };
    const auto parameters = std::views::iota(std::size_t{ 0 }, kSweepParameterCount);
    return std::ranges::fold_left(parameters, Counting{ Digits{}, index }, [&spec](const Counting& so, std::size_t p) { return Taken(so, p, spec); }).digits;
}

// How far along its range a value stands: the first is the bottom and the last is the top.
[[nodiscard]] float FractionOf(std::uint32_t digit, std::uint32_t values) noexcept
{
    if (values <= 1)
        return 1.0f;
    return static_cast<float>(digit) / static_cast<float>(values - 1);
}

// Where a swept setting stands: how far along its range this combination puts it.
[[nodiscard]] float SweptTo(float top, const SweepSpec& spec, const Digits& digits, SweepParameter parameter) noexcept
{
    return top * FractionOf(DigitOf(digits, parameter), SweepValuesOf(spec, parameter));
}

// A setting not swept is the one the base holds, untouched; one swept runs from nothing up to what the base holds.
[[nodiscard]] NrIntensity IntensityOf(const NrTuning& base, const SweepSpec& spec, const Digits& digits) noexcept
{
    if (!IsSwept(spec, SweepParameter::Intensity))
        return base.intensity;
    return NrIntensityTag::Parse(SweptTo(base.intensity.Get(), spec, digits, SweepParameter::Intensity)).value_or(base.intensity);
}

[[nodiscard]] Strength StrengthOf(Strength held, const SweepSpec& spec, const Digits& digits, SweepParameter parameter) noexcept
{
    if (!IsSwept(spec, parameter))
        return held;
    return StrengthTag::Parse(SweptTo(held.Get(), spec, digits, parameter)).value_or(held);
}

// Skin following local structure is the model's -1, so what skin runs up to is then what local structure has.
[[nodiscard]] SkinStrength SkinOf(const NrTuning& base, const SweepSpec& spec, const Digits& digits) noexcept
{
    static constexpr auto TopOf = [] [[nodiscard]] (const NrTuning& t) noexcept -> float {
        if (t.skinStructure.Get() < 0.0f)
            return t.localStructure.Get();
        return t.skinStructure.Get();
    };
    if (!IsSwept(spec, SweepParameter::SkinStructure))
        return base.skinStructure;
    return SkinStrengthTag::Parse(SweptTo(TopOf(base), spec, digits, SweepParameter::SkinStructure)).value_or(base.skinStructure);
}

[[nodiscard]] NrStyle StyleOf(const NrTuning& base, const SweepSpec& spec, const Digits& digits) noexcept
{
    constexpr std::array<NrStyle, kStyleValues> styles{ NrStyle::Standard, NrStyle::Natural, NrStyle::Cinematic };
    if (!IsSwept(spec, SweepParameter::Style))
        return base.style;
    return styles[std::min<std::size_t>(DigitOf(digits, SweepParameter::Style), styles.size() - 1)];
}

[[nodiscard]] bool MaskOf(const NrTuning& base, const SweepSpec& spec, const Digits& digits) noexcept
{
    if (!IsSwept(spec, SweepParameter::AutoMask))
        return base.autoMask;
    return DigitOf(digits, SweepParameter::AutoMask) == 1;
}

[[nodiscard]] NrTuning TuningOf(const NrTuning& base, const SweepSpec& spec, const Digits& digits) noexcept
{
    return NrTuning{ base.preset,
                     IntensityOf(base, spec, digits),
                     StyleOf(base, spec, digits),
                     StrengthOf(base.localStructure, spec, digits, SweepParameter::LocalStructure),
                     StrengthOf(base.localTone, spec, digits, SweepParameter::LocalTone),
                     SkinOf(base, spec, digits),
                     MaskOf(base, spec, digits),
                     base.uiCorrection };
}

[[nodiscard]] PassCount PassesOf(const LiveSettings& base, const SweepSpec& spec, const Digits& digits) noexcept
{
    if (!IsSwept(spec, SweepParameter::Passes))
        return base.passes;
    return PassCountTag::Parse(DigitOf(digits, SweepParameter::Passes) + 1).value_or(base.passes);
}

} // namespace

std::uint32_t SweepValuesOf(const SweepSpec& spec, SweepParameter parameter) noexcept
{
    const SweepAxis& axis = spec[static_cast<std::size_t>(parameter)];
    if (!axis.on)
        return 1;
    if (parameter == SweepParameter::Style)
        return kStyleValues;
    if (parameter == SweepParameter::AutoMask)
        return kMaskValues;
    return std::clamp(axis.values, kMinSweepValues, kMaxSweepValues);
}

std::uint32_t SweepCount(const SweepSpec& spec) noexcept
{
    // Fifty values on every counted setting, times the three styles and the two mask states, still fits the count.
    static_assert(kMaxSweepValues * kMaxSweepValues * kMaxSweepValues * kMaxSweepValues * kMaxSweepValues * static_cast<std::uint64_t>(kStyleValues) * kMaskValues <= UINT32_MAX);
    if (std::ranges::none_of(spec, [](const SweepAxis& axis) { return axis.on; }))
        return 0;
    const auto parameters = std::views::iota(std::size_t{ 0 }, kSweepParameterCount);
    return std::ranges::fold_left(parameters, std::uint32_t{ 1 }, [&spec](std::uint32_t so, std::size_t p) { return so * SweepValuesOf(spec, static_cast<SweepParameter>(p)); });
}

LiveSettings SweepCombination(const LiveSettings& base, const SweepSpec& spec, std::uint32_t index) noexcept
{
    const Digits digits = DigitsOf(spec, index);
    return LiveSettings{ true, TuningOf(base.tuning, spec, digits), PassesOf(base, spec, digits), base.depthInverted, base.mvScaleX, base.mvScaleY, base.vsync, base.resetThreshold, base.depth };
}

} // namespace interior
