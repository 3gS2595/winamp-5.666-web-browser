// Zoom belongs to the site, as it does in Chrome. Zooming a page with Ctrl and the
// wheel, + or - sets it for the page's host: every tab on that host follows, and it
// comes back whenever the host is visited again. Ctrl+0 puts the site back to normal.
//
// Claude Code's tabs are left out of that. Their pages are always at the normal size,
// whatever the user has set for the site, because a zoomed page shows Claude a window
// of a different size than it asked for, and screenshots in other pixels than the ones
// it clicks in. A zoom set in one of them is that tab's alone, and lasts until its
// page changes.
//
// While the page in view is at anything but 100%, the toolbar says so, left of the star:
// the percentage, between a − that makes the page smaller and a + that makes it larger,
// by the steps Ctrl with − and + take. A click on the percentage puts the page back to
// 100%, as Ctrl+0 does. At 100% none of the three is there, and the address bar has
// their room.
#include "browser.h"

#include <cmath>

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
    if (const TabLocation at = FindTab(tab.id); at && at.isActive()) UpdateZoomButtons(*at.window);
}

} // namespace

void ApplySiteZoom(Tab& tab) {
    const double factor = tab.forClaude ? SiteZoom::kNormal : g_app.zoom.FactorFor(TabHost(tab));
    if (!SameZoom(factor, tab.zoom)) SetTabZoom(tab, factor);
}

void SetSiteZoom(Tab& tab, double factor) {
    const std::wstring host = TabHost(tab);
    // A local file or the like, Claude's, a private window's, which leaves no word of the
    // sites it was on, or a stand-in while the tab's history is copied (back_forward.cpp),
    // whose site isn't the one the tab will show: this tab only.
    if (host.empty() || tab.forClaude || tab.isPrivate || tab.copyingHistory) {
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

void StepSiteZoom(BrowserWindow& win, bool larger) {
    if (Tab* const tab = ActiveTab(win)) SetSiteZoom(*tab, NextZoomStep(tab->zoom, larger));
}

void ForgetTabZoom(Tab& tab) {
    SetTabZoom(tab, SiteZoom::kNormal);
}

void UpdateZoomButtons(BrowserWindow& win) {
    if (!win.zoomLevelButton) return;
    const Tab* const tab = ActiveTab(win);
    const double factor = tab ? tab->zoom : SiteZoom::kNormal;
    const std::wstring percent = std::to_wstring(std::lround(factor * 100)) + L"%";
    if (WindowText(win.zoomLevelButton) != percent) ::SetWindowTextW(win.zoomLevelButton, percent.c_str());
    // They come with a zoom and go with it, which is the layout's to see to: it knows
    // where they go, and whether the window has room for them.
    if (SameZoom(factor, SiteZoom::kNormal) == HasStyle(win.zoomLevelButton, WS_VISIBLE)) FitAddressBar(win);
}

} // namespace exodium
