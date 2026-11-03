#include "site_zoom.h"

#include "text_util.h"

#include <cmath>
#include <ranges>

namespace exodium {

namespace {

constexpr int kFactorDecimals = 4;
constexpr long long kFactorScale = 10'000;

// A factor to four decimal places, as "1.2500".
std::wstring FormatFactor(double factor) {
    const long long scaled = std::llround(factor * kFactorScale);
    const std::wstring decimals = std::to_wstring(scaled % kFactorScale);
    return std::to_wstring(scaled / kFactorScale) + L'.' + std::wstring(kFactorDecimals - decimals.size(), L'0') +
           decimals;
}

} // namespace

bool SameZoom(double a, double b) noexcept {
    return std::fabs(a - b) < kZoomTolerance;
}

double SiteZoom::FactorFor(std::wstring_view host) const {
    const auto found = factors_.find(host);
    return found == factors_.end() ? kNormal : found->second;
}

void SiteZoom::Set(std::wstring_view host, double factor) {
    if (SameZoom(factor, kNormal)) {
        if (const auto found = factors_.find(host); found != factors_.end()) factors_.erase(found);
    } else {
        factors_.insert_or_assign(std::wstring(host), factor);
    }
}

SiteZoom SiteZoom::Parse(std::wstring_view text) {
    SiteZoom zoom;
    for (const auto lineRange : text | std::views::split(L'\n')) {
        const std::wstring_view line(lineRange);
        const auto tab = line.find(L'\t');
        if (tab == std::wstring_view::npos || tab == 0) continue;
        const double factor = ParseNumber<double>(line.substr(tab + 1)).value_or(0.0);
        if (factor >= kMin && factor <= kMax) zoom.factors_.insert_or_assign(std::wstring(line.substr(0, tab)), factor);
    }
    return zoom;
}

std::wstring SiteZoom::Serialize() const {
    std::wstring text;
    for (const auto& [host, factor] : factors_) {
        text.append(host).append(1, L'\t').append(FormatFactor(factor)).append(1, L'\n');
    }
    return text;
}

void SiteZoom::Load(const std::filesystem::path& file) {
    const auto text = ReadTextFile(file);
    *this = text ? Parse(*text) : SiteZoom();
}

bool SiteZoom::Save(const std::filesystem::path& file) const {
    return WriteTextFile(file, Serialize());
}

} // namespace exodium
