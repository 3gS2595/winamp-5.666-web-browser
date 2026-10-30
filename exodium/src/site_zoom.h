// Zoom belongs to the site, as it does in Chrome: a factor set for a host comes
// back whenever that host is visited again.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace exodium {

// Two zoom factors closer than this are the same one.
inline constexpr double kZoomTolerance = 0.001;
[[nodiscard]] bool SameZoom(double a, double b) noexcept;

class SiteZoom {
public:
    static constexpr double kNormal = 1.0;
    static constexpr double kMin = 0.25;
    static constexpr double kMax = 5.0;

    // The factor for a host given in lower case; kNormal when none has been set.
    [[nodiscard]] double FactorFor(std::wstring_view host) const;
    // A site put back to the normal size has no entry.
    void Set(std::wstring_view host, double factor);

    // One site to a line: the host, a tab, the factor.
    [[nodiscard]] static SiteZoom Parse(std::wstring_view text);
    [[nodiscard]] std::wstring Serialize() const;

    void Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

private:
    std::map<std::wstring, double, std::less<>> factors_;
};

} // namespace exodium
