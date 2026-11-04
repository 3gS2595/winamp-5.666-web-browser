// Zoom belongs to the site, as it does in Chrome. Zooming a page with Ctrl and the
// wheel, + or - sets it for the page's host: every tab on that host follows, and it
// comes back whenever the host is visited again. Ctrl+0 puts the site back to normal.
//
// Claude Code's tabs are left out of that. Their pages are always at the normal size,
// whatever the user has set for the site, because a zoomed page shows Claude a window
// of a different size than it asked for, and screenshots in other pixels than the ones
// it clicks in. A zoom set in one of them is that tab's alone, and lasts until its
// page changes.
#include "browser.h"

namespace exodium {

namespace {

[[nodiscard]] std::wstring TabHost(const Tab& tab) {
    return HostOf(SourceOf(tab.webview.Get()));
}

// A factor set here is the webview's own from then on, kept from page to page. One the
// user sets lasts only until the next page loads, which is why theirs is set again here.
void SetTabZoom(Tab& tab, double factor) {
    tab.zoom = factor;
    if (tab.controller) tab.controller->put_ZoomFactor(factor);
}

} // namespace

void ApplySiteZoom(Tab& tab) {
    const double factor = tab.forClaude ? SiteZoom::kNormal : g_app.zoom.FactorFor(TabHost(tab));
    if (!SameZoom(factor, tab.zoom)) SetTabZoom(tab, factor);
}

void SetSiteZoom(Tab& tab, double factor) {
    const std::wstring host = TabHost(tab);
    // A local file or the like, Claude's, or a stand-in while the tab's history is copied
    // (back_forward.cpp), whose site isn't the one the tab will show: this tab only.
    if (host.empty() || tab.forClaude || tab.copyingHistory) {
        SetTabZoom(tab, factor);
        return;
    }
    g_app.zoom.Set(host, factor);
    g_app.zoom.Save(ZoomFile());
    for (const auto& win : g_app.windows) {
        for (Tab& other : win->tabs) {
            if (other.webview && !other.forClaude && TabHost(other) == host) SetTabZoom(other, factor);
        }
    }
}

void ResetSiteZoom(BrowserWindow& win) {
    if (Tab* tab = ActiveTab(win)) SetSiteZoom(*tab, SiteZoom::kNormal);
}

} // namespace exodium
