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

// A setting swept runs from nothing up to `top`; one not swept is what it was.
[[nodiscard]] float ValueOf(float top, float base, const SweepSpec& spec, const Digits& digits, SweepParameter parameter) noexcept
{
    if (!IsSwept(spec, parameter))
        return base;
    return top * FractionOf(DigitOf(digits, parameter), SweepValuesOf(spec, parameter));
}

// Skin following local structure is the model's -1, so what skin runs up to is then what local structure has.
[[nodiscard]] float SkinTop(const NrTuning& t) noexcept
{
    if (t.skinStructure.Get() < 0.0f)
        return t.localStructure.Get();
    return t.skinStructure.Get();
}

[[nodiscard]] NrTuning TuningOf(const NrTuning& base, const SweepSpec& spec, const Digits& digits) noexcept
{
    static constexpr auto StyleOf = [] [[nodiscard]] (const NrTuning& base, const SweepSpec& spec, const Digits& digits) noexcept -> NrStyle {
        constexpr std::array<NrStyle, kStyleValues> styles{ NrStyle::Standard, NrStyle::Natural, NrStyle::Cinematic };
        if (!IsSwept(spec, SweepParameter::Style))
            return base.style;
        return styles[std::min<std::size_t>(DigitOf(digits, SweepParameter::Style), styles.size() - 1)];
    };

    static constexpr auto MaskOf = [] [[nodiscard]] (const NrTuning& base, const SweepSpec& spec, const Digits& digits) noexcept -> bool {
        if (!IsSwept(spec, SweepParameter::AutoMask))
            return base.autoMask;
        return DigitOf(digits, SweepParameter::AutoMask) == 1;
    };
    const float intensity = ValueOf(base.intensity.Get(), base.intensity.Get(), spec, digits, SweepParameter::Intensity);
    const float structure = ValueOf(base.localStructure.Get(), base.localStructure.Get(), spec, digits, SweepParameter::LocalStructure);
    const float tone = ValueOf(base.localTone.Get(), base.localTone.Get(), spec, digits, SweepParameter::LocalTone);
    const float skin = ValueOf(SkinTop(base), base.skinStructure.Get(), spec, digits, SweepParameter::SkinStructure);
    return NrTuning{ base.preset,
                     NrIntensityTag::Parse(intensity).value_or(base.intensity),
                     StyleOf(base, spec, digits),
                     StrengthTag::Parse(structure).value_or(base.localStructure),
                     StrengthTag::Parse(tone).value_or(base.localTone),
                     SkinStrengthTag::Parse(skin).value_or(base.skinStructure),
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
