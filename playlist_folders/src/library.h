// The media library's tree (its "navigation control"), through the messages gen_ml takes.
#pragma once

#include "ml_api.h"

#include <windows.h>

#include <string>

namespace folders {

class Library {
public:
    void Attach(HWND library) { library_ = library; }
    [[nodiscard]] HWND Window() const { return library_; }

    LRESULT Ipc(LPARAM command, WPARAM param) const;
    LRESULT Ipc(LPARAM command, const void* param) const { return Ipc(command, reinterpret_cast<WPARAM>(param)); }

    [[nodiscard]] ml::HNAVITEM FindById(int id) const;
    [[nodiscard]] ml::HNAVITEM Child(ml::HNAVITEM item) const;
    [[nodiscard]] ml::HNAVITEM Next(ml::HNAVITEM item) const;
    [[nodiscard]] ml::HNAVITEM Previous(ml::HNAVITEM item) const;
    [[nodiscard]] ml::HNAVITEM Parent(ml::HNAVITEM item) const;
    [[nodiscard]] int IdOf(ml::HNAVITEM item) const;
    [[nodiscard]] std::wstring TextOf(ml::HNAVITEM item) const;
    bool GetInfo(ml::NavItem& info) const;
    bool SetText(ml::HNAVITEM item, const std::wstring& text) const;

    ml::HNAVITEM Insert(ml::NavInsert& insert) const;
    bool Delete(ml::HNAVITEM item) const;
    bool MoveAmongSiblings(ml::HNAVITEM item, ml::HNAVITEM after) const;

    [[nodiscard]] ml::HNAVITEM Selection() const;
    bool Select(ml::HNAVITEM item) const;
    bool Expand(ml::HNAVITEM item, UINT command) const;
    [[nodiscard]] bool IsExpanded(ml::HNAVITEM item) const;
    bool EnsureVisible(ml::HNAVITEM item) const;
    bool EditTitle(ml::HNAVITEM item) const;
    bool GetRect(ml::HNAVITEM item, RECT& rect) const; // the whole line, in the tree's client area

    [[nodiscard]] HWND TreeWindow() const;
    // The item under a point on the screen, or null.
    [[nodiscard]] ml::HNAVITEM HitTest(POINT screen) const;

    void BeginUpdate() const;
    void EndUpdate() const;

    // A menu drawn in the library's skin (TPM_RETURNCMD is always added).
    int TrackPopup(HMENU menu, UINT flags, int x, int y, HWND owner) const;
    void Skin(HWND window, UINT type, UINT style) const;

    // While one lives, what the plugin itself sends the library isn't taken for
    // ml_playlists' doing (see Tree::OnLibraryIpc).
    class Own {
    public:
        Own() { ++depth_; }
        ~Own() { --depth_; }
        Own(const Own&) = delete;
        Own& operator=(const Own&) = delete;
        [[nodiscard]] static bool Active() { return depth_ > 0; }

    private:
        static inline int depth_ = 0;
    };

private:
    HWND library_ = nullptr;
};

} // namespace folders
