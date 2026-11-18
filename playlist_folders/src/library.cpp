#include "library.h"

namespace folders {

using namespace ml;

LRESULT Library::Ipc(LPARAM command, WPARAM param) const {
    if (!library_) return 0;
    return ::SendMessageW(library_, WM_ML_IPC, param, command);
}

HNAVITEM Library::FindById(int id) const {
    return reinterpret_cast<HNAVITEM>(Ipc(kIpcNavFindItemById, static_cast<WPARAM>(id)));
}

HNAVITEM Library::Child(HNAVITEM item) const {
    return item ? reinterpret_cast<HNAVITEM>(Ipc(kIpcItemGetChild, item)) : nullptr;
}

HNAVITEM Library::Next(HNAVITEM item) const {
    return item ? reinterpret_cast<HNAVITEM>(Ipc(kIpcItemGetNext, item)) : nullptr;
}

HNAVITEM Library::Previous(HNAVITEM item) const {
    return item ? reinterpret_cast<HNAVITEM>(Ipc(kIpcItemGetPrevious, item)) : nullptr;
}

HNAVITEM Library::Parent(HNAVITEM item) const {
    return item ? reinterpret_cast<HNAVITEM>(Ipc(kIpcItemGetParent, item)) : nullptr;
}

int Library::IdOf(HNAVITEM item) const {
    if (!item) return 0;
    NavItem info{};
    info.cbSize = sizeof(info);
    info.hItem = item;
    info.mask = kNimfItemId;
    return GetInfo(info) ? info.id : 0;
}

std::wstring Library::TextOf(HNAVITEM item) const {
    if (!item) return {};
    wchar_t text[1024] = {};
    NavItem info{};
    info.cbSize = sizeof(info);
    info.hItem = item;
    info.mask = kNimfText;
    info.pszText = text;
    info.cchTextMax = static_cast<int>(std::size(text));
    return GetInfo(info) ? std::wstring(text) : std::wstring();
}

bool Library::GetInfo(NavItem& info) const {
    return Ipc(kIpcItemGetInfo, &info) != 0;
}

bool Library::SetText(HNAVITEM item, const std::wstring& text) const {
    NavItem info{};
    info.cbSize = sizeof(info);
    info.hItem = item;
    info.mask = kNimfText;
    info.pszText = const_cast<wchar_t*>(text.c_str());
    Own own;
    return Ipc(kIpcItemSetInfo, &info) != 0;
}

HNAVITEM Library::Insert(NavInsert& insert) const {
    Own own;
    insert.item.cbSize = sizeof(NavItem);
    return reinterpret_cast<HNAVITEM>(Ipc(kIpcNavInsertItem, &insert));
}

bool Library::Delete(HNAVITEM item) const {
    if (!item) return false;
    Own own;
    return Ipc(kIpcNavDeleteItem, item) != 0;
}

bool Library::MoveAmongSiblings(HNAVITEM item, HNAVITEM after) const {
    if (!item) return false;
    Own own;
    NavItemMove move{};
    move.hItem = item;
    if (after) {
        move.hItemDest = after;
        move.fAfter = TRUE;
    } else {
        // First: before the first sibling.
        move.hItemDest = Child(Parent(item));
        move.fAfter = FALSE;
        if (move.hItemDest == item) return true;
    }
    return Ipc(kIpcItemMove, &move) != 0;
}

HNAVITEM Library::Selection() const {
    return reinterpret_cast<HNAVITEM>(Ipc(kIpcNavGetSelection, WPARAM{0}));
}

bool Library::Select(HNAVITEM item) const {
    return item && Ipc(kIpcItemSelect, item) != 0;
}

bool Library::Expand(HNAVITEM item, UINT command) const {
    if (!item) return false;
    NavItemExpand expand{item, command};
    return Ipc(kIpcItemExpand, &expand) != 0;
}

bool Library::IsExpanded(HNAVITEM item) const {
    return item && (static_cast<UINT>(Ipc(kIpcItemGetState, item)) & kNisExpanded) != 0;
}

bool Library::EnsureVisible(HNAVITEM item) const {
    return item && Ipc(kIpcItemEnsureVisible, item) != 0;
}

bool Library::EditTitle(HNAVITEM item) const {
    return item && Ipc(kIpcItemEditTitle, item) != 0;
}

bool Library::GetRect(HNAVITEM item, RECT& rect) const {
    NavItemGetRect get{};
    get.hItem = item;
    get.fItem = FALSE;
    if (!item || !Ipc(kIpcItemGetRect, &get)) return false;
    rect = get.rc;
    return true;
}

HWND Library::TreeWindow() const {
    return reinterpret_cast<HWND>(Ipc(kIpcNavGetHwnd, WPARAM{0}));
}

HNAVITEM Library::HitTest(POINT screen) const {
    const HWND tree = TreeWindow();
    if (!tree) return nullptr;
    NavHitTest test{};
    test.pt = screen;
    ::ScreenToClient(tree, &test.pt);
    const auto item = reinterpret_cast<HNAVITEM>(Ipc(kIpcNavHitTest, &test));
    if (!(test.flags & (kNavHtOnItem | kNavHtOnItemButton | kNavHtOnItemIndent | kNavHtOnItemRight))) return nullptr;
    return item ? item : test.hItem;
}

void Library::BeginUpdate() const {
    Ipc(kIpcNavBeginUpdate, kNufLockTop);
}

void Library::EndUpdate() const {
    Ipc(kIpcNavEndUpdate, WPARAM{0});
}

int Library::TrackPopup(HMENU menu, UINT flags, int x, int y, HWND owner) const {
    SkinnedPopup popup{};
    popup.cbSize = sizeof(popup);
    popup.hmenu = menu;
    popup.fuFlags = flags | TPM_RETURNCMD;
    popup.x = x;
    popup.y = y;
    popup.hwnd = owner;
    popup.skinStyle = kSmsUseSkinFont;
    Own own;
    return static_cast<int>(Ipc(kIpcTrackSkinnedPopupEx, &popup));
}

void Library::Skin(HWND window, UINT type, UINT style) const {
    SkinWindow skin{window, type, style};
    Ipc(kIpcSkinWindow, &skin);
}

} // namespace folders
