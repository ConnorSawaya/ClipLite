#include "cliplite/ui/library_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>

#include "cliplite/log.h"
#include "cliplite/library/export_clip.h"
#include "cliplite/library/thumbnails.h"
#include "cliplite/util/win_utf8.h"

namespace cliplite::ui {

namespace {

constexpr UINT IDC_LIST = 100;
constexpr UINT IDC_PLAY = 101;
constexpr UINT IDC_OPEN_FOLDER = 102;
constexpr UINT IDC_DELETE = 103;
constexpr UINT IDC_SEARCH = 104;
constexpr UINT IDC_REFRESH = 105;
constexpr UINT IDC_TRIM_BTN = 107;

constexpr UINT IDM_PLAY = 200;
constexpr UINT IDM_OPEN = 201;
constexpr UINT IDM_COPY = 202;
constexpr UINT IDM_DELETE = 203;
constexpr UINT IDM_RENAME = 204;
constexpr UINT IDM_PROPERTIES = 205;

constexpr UINT IDD_RENAME = 300;
constexpr UINT IDC_RENAME_EDIT = 301;
constexpr UINT IDD_TRIM = 302;
constexpr UINT IDC_TRIM_START = 310;
constexpr UINT IDC_TRIM_END = 311;
constexpr UINT IDC_TRIM_INFO = 312;

constexpr UINT TIMER_ID = 1;

const wchar_t* kWindowClass = L"ClipLiteLibraryWindow";
const wchar_t* kVideoClass = L"ClipLiteVideoView";

const COLORREF kBg = RGB(10, 10, 10);
const COLORREF kPanel = RGB(16, 16, 16);
const COLORREF kRowA = RGB(14, 14, 14);
const COLORREF kRowB = RGB(17, 17, 17);
const COLORREF kHoverFill = RGB(32, 32, 32);
const COLORREF kSelRow = RGB(28, 28, 28);
const COLORREF kLine = RGB(38, 38, 38);
const COLORREF kInput = RGB(13, 13, 13);
const COLORREF kText = RGB(240, 240, 240);
const COLORREF kTextSub = RGB(170, 170, 170);
const COLORREF kMuted = RGB(140, 140, 140);
const COLORREF kDisabledText = RGB(90, 90, 90);
const COLORREF kPrimary = RGB(245, 245, 245);
const COLORREF kPrimaryHot = RGB(255, 255, 255);
const COLORREF kSecondary = RGB(22, 22, 22);
const COLORREF kVideoBg = RGB(0, 0, 0);

std::wstring lower_copy(std::wstring value) {
    for (wchar_t& c : value) c = static_cast<wchar_t>(towlower(c));
    return value;
}

bool contains_case_insensitive(const std::wstring& value, const std::wstring& query) {
    return lower_copy(value).find(lower_copy(query)) != std::wstring::npos;
}

void set_font(HWND hwnd, HFONT font) {
    if (hwnd && font) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HBRUSH brush_once(COLORREF c) {
    static HBRUSH row_a = CreateSolidBrush(kRowA);
    static HBRUSH row_b = CreateSolidBrush(kRowB);
    static HBRUSH sel = CreateSolidBrush(kSelRow);
    static HBRUSH bar = CreateSolidBrush(kPrimary);
    if (c == kRowA) return row_a;
    if (c == kRowB) return row_b;
    if (c == kSelRow) return sel;
    return bar;
}

LPARAM row_param(HWND list, int row) {
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = row;
    if (!ListView_GetItem(list, &it)) return -1;
    return it.lParam;
}

int get_dlg_int(HWND dlg, int id, int fallback) {
    wchar_t buf[32]{};
    GetDlgItemTextW(dlg, id, buf, _countof(buf));
    wchar_t* endp = nullptr;
    const long v = wcstol(buf, &endp, 10);
    return (endp == buf) ? fallback : static_cast<int>(v);
}

// Parses trim edit boxes as seconds (float allowed, e.g. "1.5" or "61.9") and
// returns milliseconds. Integer input ("5") still means 5000ms, preserving
// backward compat while fixing the previous seconds-only truncation that lost
// up to 999ms of precision.
int64_t get_dlg_ms(HWND dlg, int id, int64_t fallback_ms) {
    wchar_t buf[32]{};
    GetDlgItemTextW(dlg, id, buf, _countof(buf));
    wchar_t* endp = nullptr;
    const double v = wcstod(buf, &endp);
    if (endp == buf || v < 0) return fallback_ms;
    return static_cast<int64_t>(v * 1000.0);
}

std::wstring format_duration(int64_t ms) {
    wchar_t buf[32];
    swprintf_s(buf, L"%02lld:%02lld", static_cast<long long>(ms / 60000),
               static_cast<long long>((ms / 1000) % 60));
    return buf;
}

std::wstring date_label_from_filename(const std::wstring& filename) {
    const auto us = filename.find(L'_');
    if (us == std::wstring::npos) return filename;
    const std::wstring stamp = filename.substr(us + 1);
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (swscanf_s(stamp.c_str(), L"%d-%d-%d_%d-%d-%d", &y, &mo, &d, &hh, &mm, &ss) != 6) {
        return stamp;
    }
    static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                      L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t buf[32];
    swprintf_s(buf, L"%s %d %02d:%02d", months[(mo - 1) % 12], d, hh, mm);
    return buf;
}

INT_PTR CALLBACK rename_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        auto* name = reinterpret_cast<std::wstring*>(lp);
        SetDlgItemTextW(dlg, IDC_RENAME_EDIT, name->c_str());
        SendDlgItemMessageW(dlg, IDC_RENAME_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_RENAME_EDIT));
        return FALSE;
    }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[512]{};
            GetDlgItemTextW(dlg, IDC_RENAME_EDIT, buf, 512);
            auto* name = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dlg, DWLP_USER));
            if (name) *name = buf;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

struct TrimParams {
    std::wstring path;
    int64_t duration_ms = 0;
};

std::wstring unique_trim_path(const std::filesystem::path& src) {
    std::filesystem::path out = src.parent_path() / (src.stem().wstring() + L"_trimmed.mp4");
    int n = 1;
    while (std::filesystem::exists(out)) {
        out = src.parent_path() /
              (src.stem().wstring() + L"_trimmed_" + std::to_wstring(n++) + L".mp4");
        if (n > 99) break;
    }
    return out.wstring();
}

INT_PTR CALLBACK trim_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        auto* tp = reinterpret_cast<TrimParams*>(lp);
        wchar_t buf[32]{};
        // Default to the exact duration (was floor(duration/1000), losing ms).
        swprintf_s(buf, L"%.3f", static_cast<double>(tp->duration_ms) / 1000.0);
        SetDlgItemTextW(dlg, IDC_TRIM_START, L"0");
        SetDlgItemTextW(dlg, IDC_TRIM_END, buf);
        swprintf_s(buf, L"Clip length: %.1f s", static_cast<double>(tp->duration_ms) / 1000.0);
        SetDlgItemTextW(dlg, IDC_TRIM_INFO, buf);
        SetFocus(GetDlgItem(dlg, IDC_TRIM_START));
        return FALSE;
    }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) {
            auto* tp = reinterpret_cast<TrimParams*>(GetWindowLongPtrW(dlg, DWLP_USER));
            const int64_t start_ms = get_dlg_ms(dlg, IDC_TRIM_START, -1);
            int64_t end_ms = get_dlg_ms(dlg, IDC_TRIM_END, -1);
            if (!tp || start_ms < 0 || end_ms < 0 || end_ms <= start_ms ||
                end_ms > tp->duration_ms + 1) {
                // +1ms tolerance for float rounding (e.g. "61.900" -> 61900).
                if (!(tp && end_ms > tp->duration_ms &&
                      end_ms <= tp->duration_ms + 1)) {
                    MessageBoxW(dlg, L"Enter a valid range within the clip length.", L"Trim",
                                MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                end_ms = tp->duration_ms;
            }

            const std::filesystem::path src(tp->path);
            const std::wstring out = unique_trim_path(src);
            if (cliplite::library::export_trimmed(tp->path, out, start_ms,
                                                  end_ms)) {
                const std::wstring name = std::filesystem::path(out).filename().wstring();
                MessageBoxW(dlg, (L"Saved " + name).c_str(), L"Trim", MB_OK | MB_ICONINFORMATION);
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxW(dlg, L"Export failed. See cliplite.log for details.", L"Trim",
                            MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    if (msg == WM_CLOSE) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

LibraryWindow::~LibraryWindow() {
    player_.close();
    if (hwnd_) DestroyWindow(hwnd_);
    if (himl_) ImageList_Destroy(himl_);
    if (dark_brush_) DeleteObject(dark_brush_);
    if (panel_brush_) DeleteObject(panel_brush_);
    if (input_brush_) DeleteObject(input_brush_);
    if (border_brush_) DeleteObject(border_brush_);
    if (video_brush_) DeleteObject(video_brush_);
    if (title_font_) DeleteObject(title_font_);
    if (body_font_) DeleteObject(body_font_);
    if (small_font_) DeleteObject(small_font_);
    if (button_font_) DeleteObject(button_font_);
}

bool LibraryWindow::create(HINSTANCE hInstance, const std::wstring& clip_dir) {
    hinst_ = hInstance;
    clip_dir_ = clip_dir;
    dark_brush_ = CreateSolidBrush(kBg);
    panel_brush_ = CreateSolidBrush(kPanel);
    input_brush_ = CreateSolidBrush(kInput);
    border_brush_ = CreateSolidBrush(kLine);
    video_brush_ = CreateSolidBrush(kVideoBg);
    title_font_ = CreateFontW(24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    body_font_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    small_font_ = CreateFontW(11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    button_font_ = CreateFontW(12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW vc{};
    vc.cbSize = sizeof(vc);
    vc.lpfnWndProc = VideoViewProc;
    vc.hInstance = hinst_;
    vc.lpszClassName = kVideoClass;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = video_brush_;
    RegisterClassExW(&vc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst_;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = dark_brush_;
    wc.hIcon = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    wc.hIconSm = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, kWindowClass, L"ClipLite", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 980, 620, nullptr, nullptr, hinst_, this);
    if (!hwnd_) return false;

    set_dark_mode();
    return true;
}

void LibraryWindow::set_dark_mode() {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &kBg, sizeof(kBg));
    if (list_) {
        ListView_SetBkColor(list_, kPanel);
        ListView_SetTextBkColor(list_, kPanel);
        ListView_SetTextColor(list_, kText);
        SetWindowTheme(list_, L"DarkMode_Explorer", nullptr);
    }
    if (search_) SetWindowTheme(search_, L"DarkMode_Explorer", nullptr);
}

HWND LibraryWindow::button_by_id(int control_id) const {
    switch (control_id) {
        case IDC_PLAY: return btn_play_;
        case IDC_OPEN_FOLDER: return btn_open_;
        case IDC_DELETE: return btn_delete_;
        case IDC_TRIM_BTN: return btn_trim_;
        case IDC_REFRESH: return btn_refresh_;
        default: return nullptr;
    }
}

void LibraryWindow::set_hovered(int control_id) {
    if (hovered_id_ == control_id) return;
    if (HWND old = button_by_id(hovered_id_)) InvalidateRect(old, nullptr, TRUE);
    hovered_id_ = control_id;
    if (HWND now = button_by_id(control_id)) InvalidateRect(now, nullptr, TRUE);
}

LRESULT CALLBACK LibraryWindow::ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self =
        reinterpret_cast<LibraryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (msg) {
            case WM_MOUSEMOVE: {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                self->set_hovered(GetDlgCtrlID(hwnd));
                break;
            }
            case WM_MOUSELEAVE:
                self->set_hovered(0);
                break;
            default:
                break;
        }
    }
    return CallWindowProcW(self ? self->btn_base_proc_
                                : reinterpret_cast<WNDPROC>(DefWindowProcW),
                           hwnd, msg, wp, lp);
}

void LibraryWindow::show() {
    if (!hwnd_) return;
    refresh_list();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void LibraryWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool LibraryWindow::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void LibraryWindow::refresh_clips() {
    if (hwnd_) refresh_list();
}

void LibraryWindow::refresh_list() {
    library_.scan(clip_dir_);
    ListView_DeleteAllItems(list_);

    const auto& clips = library_.clips();
    std::vector<size_t> visible;
    visible.reserve(clips.size());
    for (size_t i = 0; i < clips.size(); ++i) {
        const auto& c = clips[i];
        const std::wstring game = cliplite::util::utf8_to_wide(c.game);
        if (!filter_.empty() && !contains_case_insensitive(c.filename, filter_) &&
            !contains_case_insensitive(game, filter_)) {
            continue;
        }
        visible.push_back(i);
    }

    HIMAGELIST fresh = ImageList_Create(
        112, 63, ILC_COLOR32, static_cast<int>(std::max<size_t>(1, visible.size())), 4);
    HIMAGELIST old = ListView_SetImageList(list_, fresh, LVSIL_SMALL);
    if (old && old != fresh) ImageList_Destroy(old);

    int row = 0;
    for (const size_t i : visible) {
        const auto& c = clips[i];
        std::wstring gname = cliplite::util::utf8_to_wide(c.game);

        int image = -1;
        HBITMAP thumb = cliplite::library::generate_thumbnail(c.path, 112, 63);
        if (thumb) {
            image = ImageList_Add(fresh, thumb, nullptr);
            DeleteObject(thumb);
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = row++;
        item.iImage = image;
        item.pszText = gname.data();
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(list_, &item);

        const int64_t dur = cliplite::library::ClipLibrary::probe_duration_ms(c.path);
        const std::wstring d = format_duration(dur);
        const std::wstring date = date_label_from_filename(c.filename);
        ListView_SetItemText(list_, item.iItem, 1, const_cast<LPWSTR>(d.c_str()));
        ListView_SetItemText(list_, item.iItem, 2, const_cast<LPWSTR>(date.c_str()));
    }
    himl_ = fresh;

    selected_ = -1;
    if (label_count_) {
        const std::wstring count = std::to_wstring(visible.size()) +
                                   (visible.size() == 1 ? L" clip" : L" clips");
        SetWindowTextW(label_count_, count.c_str());
    }
    empty_visible_ = visible.empty();
    ShowWindow(list_, empty_visible_ ? SW_HIDE : SW_SHOW);
    update_action_state();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void LibraryWindow::play_selected() {
    if (selected_ < 0 || static_cast<size_t>(selected_) >= library_.clips().size()) return;
    const auto& clip = library_.clips()[static_cast<size_t>(selected_)];
    if (player_.open(video_, clip.path)) {
        SetWindowTextW(btn_play_, L"Pause");
        SetFocus(video_);
        update_position();
    }
}

void LibraryWindow::toggle_play() {
    if (!player_.opened()) {
        play_selected();
        return;
    }
    player_.toggle_play();
    SetWindowTextW(btn_play_, player_.playing() ? L"Pause" : L"Play");
}

void LibraryWindow::delete_selected() {
    if (selected_ < 0 || static_cast<size_t>(selected_) >= library_.clips().size()) return;
    const std::wstring path = library_.clips()[static_cast<size_t>(selected_)].path;

    if (player_.current_path() == path) player_.close();

    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = hwnd_;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOCONFIRMATION;
    const int r = SHFileOperationW(&op);
    CL_INFO("Library", "delete clip (recycle bin) rc=" + std::to_string(r));
    refresh_list();
}

void LibraryWindow::open_file_location() {
    if (selected_ < 0 || static_cast<size_t>(selected_) >= library_.clips().size()) {
        ShellExecuteW(hwnd_, L"open", clip_dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    const std::wstring path = library_.clips()[static_cast<size_t>(selected_)].path;
    const std::wstring cmd = L"/select,\"" + path + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
}

void LibraryWindow::update_action_state() {
    const bool has_selection = selected_ >= 0 &&
                               static_cast<size_t>(selected_) < library_.clips().size();
    if (btn_play_) EnableWindow(btn_play_, has_selection ? TRUE : FALSE);
    if (btn_delete_) EnableWindow(btn_delete_, has_selection ? TRUE : FALSE);
    if (btn_trim_) EnableWindow(btn_trim_, has_selection ? TRUE : FALSE);
}

void LibraryWindow::draw_button(const DRAWITEMSTRUCT* item) {
    if (!item) return;
    RECT rc = item->rcItem;
    const bool primary = item->CtlID == IDC_PLAY;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool hot = hovered_id_ == static_cast<int>(item->CtlID) && !disabled && !pressed;

    COLORREF fill = primary ? kPrimary : kSecondary;
    if (pressed) fill = primary ? RGB(210, 210, 210) : RGB(12, 12, 12);
    else if (hot) fill = primary ? kPrimaryHot : kHoverFill;
    if (disabled) fill = RGB(24, 24, 24);

    HBRUSH brush = CreateSolidBrush(fill);
    const bool no_pen = primary;
    HPEN pen = no_pen ? static_cast<HPEN>(GetStockObject(NULL_PEN))
                      : disabled ? CreatePen(PS_SOLID, 1, RGB(30, 30, 30))
                                 : CreatePen(PS_SOLID, 1, kLine);
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
    SelectObject(item->hDC, old_pen);
    SelectObject(item->hDC, old_brush);
    DeleteObject(brush);
    if (!no_pen) DeleteObject(pen);

    wchar_t text[128]{};
    GetWindowTextW(item->hwndItem, text, _countof(text));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, disabled ? kDisabledText
                                     : (primary ? RGB(10, 10, 10) : kText));
    DrawTextW(item->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void LibraryWindow::draw_empty_state(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    constexpr int kSide = 320;
    constexpr int kTopBar = 96;
    RECT area{kSide, kTopBar, rc.right, rc.bottom};

    const int cx = (area.left + area.right) / 2;
    const int cy = (area.top + area.bottom) / 2 - 24;

    POINT tri[3] = {{cx - 22, cy - 34}, {cx - 22, cy + 34}, {cx + 34, cy}};
    HBRUSH tri_brush = CreateSolidBrush(RGB(60, 60, 60));
    HPEN tri_pen = CreatePen(PS_SOLID, 2, RGB(60, 60, 60));
    HGDIOBJ ob = SelectObject(dc, tri_brush);
    HGDIOBJ op = SelectObject(dc, tri_pen);
    Polygon(dc, tri, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(tri_brush);
    DeleteObject(tri_pen);

    SetBkMode(dc, TRANSPARENT);
    HFONT old = static_cast<HFONT>(SelectObject(dc, body_font_));
    RECT t1{area.left, cy + 56, area.right, cy + 82};
    SetTextColor(dc, kText);
    DrawTextW(dc, library_.size() > 0 ? L"No matches" : L"No clips yet", -1, &t1,
              DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, small_font_);
    RECT t2{area.left, cy + 84, area.right, cy + 106};
    SetTextColor(dc, kMuted);
    DrawTextW(dc, library_.size() > 0 ? L"Try a different search"
                                      : L"Press F8 in game to save a replay",
              -1, &t2, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

void LibraryWindow::copy_selected() {
    if (selected_ < 0 || static_cast<size_t>(selected_) >= library_.clips().size()) return;
    const std::wstring path = library_.clips()[static_cast<size_t>(selected_)].path;
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const size_t len = (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + len);
    if (hg) {
        DROPFILES* df = static_cast<DROPFILES*>(GlobalLock(hg));
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
        std::memcpy(dst, path.c_str(), path.size() * sizeof(wchar_t));
        dst[path.size()] = 0;
        dst[path.size() + 1] = 0;
        GlobalUnlock(hg);
        SetClipboardData(CF_HDROP, hg);
    }
    CloseClipboard();
}

void LibraryWindow::update_position() {
    if (!player_.opened()) return;
    player_.process_events();
    if (player_.ended()) SetWindowTextW(btn_play_, L"Play");
    const int64_t pos = player_.position_ms();
    const int64_t dur = player_.duration_ms();
    SetWindowTextW(label_pos_, (format_duration(pos) + L" / " + format_duration(dur)).c_str());
}

void LibraryWindow::show_context_menu(int x, int y) {
    if (selected_ < 0) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_PLAY, L"Play");
    AppendMenuW(menu, MF_STRING, IDM_RENAME, L"Rename");
    AppendMenuW(menu, MF_STRING, IDM_PROPERTIES, L"Properties");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open file location");
    AppendMenuW(menu, MF_STRING, IDM_COPY, L"Copy file");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_DELETE, L"Delete");
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void LibraryWindow::rename_selected() {
    if (selected_ < 0 || static_cast<size_t>(selected_) >= library_.clips().size()) return;
    const auto& clip = library_.clips()[static_cast<size_t>(selected_)];
    const std::filesystem::path p(clip.path);
    std::wstring newname = p.stem().wstring();
    if (DialogBoxParamW(hinst_, MAKEINTRESOURCEW(IDD_RENAME), hwnd_, rename_proc,
                        reinterpret_cast<LPARAM>(&newname)) == IDOK &&
        !newname.empty() && newname != p.stem().wstring()) {
        static const wchar_t* kBad = L"<>:\"/\\|?*";
        if (newname.find_first_of(kBad) != std::wstring::npos) {
            MessageBoxW(hwnd_, L"That name contains characters not allowed in file names.",
                        L"Rename", MB_OK | MB_ICONWARNING);
            return;
        }
        if (player_.current_path() == clip.path) player_.close();
        const std::wstring newpath = p.parent_path().wstring() + L"\\" + newname + L".mp4";
        if (MoveFileW(clip.path.c_str(), newpath.c_str())) {
            refresh_list();
        } else {
            MessageBoxW(hwnd_, L"Could not rename the clip.", L"Rename", MB_OK | MB_ICONERROR);
        }
    }
}

void LibraryWindow::show_properties() {
    if (selected_ < 0 || static_cast<size_t>(selected_) >= library_.clips().size()) return;
    const auto& clip = library_.clips()[static_cast<size_t>(selected_)];
    const int64_t dur = cliplite::library::ClipLibrary::probe_duration_ms(clip.path);
    std::wstring msg = L"File: " + clip.filename + L"\n";
    msg += L"Game: " + cliplite::util::utf8_to_wide(clip.game) + L"\n";
    msg += L"Duration: " + format_duration(dur) + L"\n";
    msg += L"Size: " + std::to_wstring(clip.file_size) + L" bytes\n";
    msg += L"Path: " + clip.path;
    MessageBoxW(hwnd_, msg.c_str(), L"Properties", MB_OK | MB_ICONINFORMATION);
}

void LibraryWindow::trim_selected() {
    if (selected_ < 0 || static_cast<size_t>(selected_) >= library_.clips().size()) return;
    TrimParams tp;
    tp.path = library_.clips()[static_cast<size_t>(selected_)].path;
    tp.duration_ms = cliplite::library::ClipLibrary::probe_duration_ms(tp.path);
    if (tp.duration_ms <= 0) {
        MessageBoxW(hwnd_, L"Could not determine the clip duration.", L"Trim",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (DialogBoxParamW(hinst_, MAKEINTRESOURCEW(IDD_TRIM), hwnd_, trim_proc,
                        reinterpret_cast<LPARAM>(&tp)) == IDOK) {
        refresh_list();
    }
}

LRESULT CALLBACK LibraryWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<LibraryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<LibraryWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE: {
            self->list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                          WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                              LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LIST,
                                          self->hinst_, nullptr);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = 112;
            col.pszText = const_cast<LPWSTR>(L"Game");
            ListView_InsertColumn(self->list_, 0, &col);
            col.cx = 56;
            col.pszText = const_cast<LPWSTR>(L"Length");
            ListView_InsertColumn(self->list_, 1, &col);
            col.cx = 152;
            col.pszText = const_cast<LPWSTR>(L"Date");
            ListView_InsertColumn(self->list_, 2, &col);
            ListView_SetExtendedListViewStyle(self->list_,
                                              LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            self->video_ = CreateWindowExW(
                0, kVideoClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd,
                nullptr, self->hinst_, nullptr);
            SetWindowLongPtrW(self->video_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));

            self->search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SEARCH,
                                            self->hinst_, nullptr);
            SendMessageW(self->search_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search clips");

            struct BtnDef {
                HWND* slot;
                const wchar_t* text;
                UINT id;
            };
            const BtnDef buttons[] = {
                {&self->btn_refresh_, L"Refresh", IDC_REFRESH},
                {&self->btn_play_, L"Play", IDC_PLAY},
                {&self->btn_trim_, L"Trim", IDC_TRIM_BTN},
                {&self->btn_open_, L"Open folder", IDC_OPEN_FOLDER},
                {&self->btn_delete_, L"Delete", IDC_DELETE},
            };
            for (const BtnDef& b : buttons) {
                *b.slot = CreateWindowExW(0, L"BUTTON", b.text,
                                          WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)b.id,
                                          self->hinst_, nullptr);
                SetWindowLongPtrW(*b.slot, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                if (!self->btn_base_proc_) {
                    self->btn_base_proc_ = reinterpret_cast<WNDPROC>(
                        SetWindowLongPtrW(*b.slot, GWLP_WNDPROC,
                                          reinterpret_cast<LONG_PTR>(&LibraryWindow::ButtonProc)));
                } else {
                    SetWindowLongPtrW(*b.slot, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(&LibraryWindow::ButtonProc));
                }
            }

            self->label_pos_ = CreateWindowExW(0, L"STATIC", L"00:00 / 00:00", WS_CHILD | WS_VISIBLE,
                                               0, 0, 0, 0, hwnd, nullptr, self->hinst_, nullptr);
            self->label_count_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                                 hwnd, nullptr, self->hinst_, nullptr);

            set_font(self->list_, self->body_font_);
            set_font(self->search_, self->body_font_);
            set_font(self->label_pos_, self->body_font_);
            set_font(self->label_count_, self->small_font_);
            for (const BtnDef& b : buttons) set_font(*b.slot, self->button_font_);

            self->set_dark_mode();
            SetTimer(hwnd, TIMER_ID, 200, nullptr);
            return 0;
        }
        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            constexpr int kSide = 320;
            constexpr int kTopBar = 96;
            MoveWindow(self->search_, 12, 52, kSide - 12 - 12 - 88, 28, TRUE);
            MoveWindow(self->btn_refresh_, kSide - 84, 51, 72, 30, TRUE);
            MoveWindow(self->list_, 12, kTopBar, kSide - 24, h - kTopBar - 44, TRUE);
            MoveWindow(self->label_count_, 12, h - 34, kSide - 24, 20, TRUE);

            const int right_x = kSide + 12;
            const int bar_y = h - 54;
            MoveWindow(self->video_, right_x, 12, w - right_x - 12, bar_y - 22, TRUE);
            const int trim_x = w - 310;
            const int label_w = (trim_x - 8) - (right_x + 104);
            MoveWindow(self->label_pos_, right_x + 104, bar_y + 9, label_w > 60 ? label_w : 60, 20,
                       TRUE);
            MoveWindow(self->btn_play_, right_x, bar_y, 96, 36, TRUE);
            MoveWindow(self->btn_trim_, trim_x, bar_y, 86, 36, TRUE);
            MoveWindow(self->btn_open_, w - 216, bar_y, 100, 36, TRUE);
            MoveWindow(self->btn_delete_, w - 106, bar_y, 94, 36, TRUE);
            return 0;
        }
        case WM_ERASEBKGND:
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_ID) self->update_position();
            return 0;
        case WM_NOTIFY: {
            const NMHDR* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == IDC_LIST) {
                if (hdr->code == NM_DBLCLK) {
                    self->play_selected();
                } else if (hdr->code == NM_RCLICK) {
                    POINT pt;
                    GetCursorPos(&pt);
                    self->show_context_menu(pt.x, pt.y);
                } else if (hdr->code == LVN_ITEMCHANGED) {
                    const int row = ListView_GetNextItem(self->list_, -1, LVNI_SELECTED);
                    self->selected_ = row >= 0
                                          ? static_cast<int>(row_param(self->list_, row))
                                          : -1;
                    self->update_action_state();
                } else if (hdr->code == NM_CUSTOMDRAW) {
                    auto* nmcd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lp);
                    const DWORD stage = nmcd->nmcd.dwDrawStage;
                    if (stage == CDDS_PREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
                    if ((stage & CDDS_ITEMPREPAINT) && !(stage & CDDS_SUBITEM)) {
                        const int idx = static_cast<int>(nmcd->nmcd.dwItemSpec);
                        const bool sel = (nmcd->nmcd.uItemState & CDIS_SELECTED) != 0;
                        HDC dc = nmcd->nmcd.hdc;
                        RECT rc = nmcd->nmcd.rc;
                        FillRect(dc, &rc, sel ? brush_once(kSelRow)
                                              : brush_once(idx % 2 ? kRowB : kRowA));
                        if (sel) {
                            RECT bar{rc.left, rc.top, rc.left + 3, rc.bottom};
                            FillRect(dc, &bar, brush_once(kPrimary));
                        }
                        SetTextColor(dc, sel ? kText : kTextSub);
                        SetBkColor(dc, sel ? kSelRow : (idx % 2 ? kRowB : kRowA));
                        return CDRF_DODEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_PLAY:
                    self->toggle_play();
                    break;
                case IDC_OPEN_FOLDER:
                    self->open_file_location();
                    break;
                case IDC_DELETE:
                    self->delete_selected();
                    break;
                case IDC_REFRESH:
                    self->refresh_list();
                    break;
                case IDC_TRIM_BTN:
                    self->trim_selected();
                    break;
                case IDM_PLAY:
                    self->play_selected();
                    break;
                case IDM_OPEN:
                    self->open_file_location();
                    break;
                case IDM_COPY:
                    self->copy_selected();
                    break;
                case IDM_DELETE:
                    self->delete_selected();
                    break;
                case IDM_RENAME:
                    self->rename_selected();
                    break;
                case IDM_PROPERTIES:
                    self->show_properties();
                    break;
                default:
                    if (LOWORD(wp) == IDC_SEARCH && HIWORD(wp) == EN_CHANGE) {
                        wchar_t buf[128]{};
                        GetWindowTextW(self->search_, buf, _countof(buf));
                        const std::wstring next(buf);
                        if (next != self->filter_) {
                            self->filter_ = next;
                            self->refresh_list();
                        }
                    }
                    break;
            }
            return 0;
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis && dis->CtlType == ODT_BUTTON) {
                self->draw_button(dis);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp), kText);
            SetBkColor(reinterpret_cast<HDC>(wp), kInput);
            return reinterpret_cast<LRESULT>(self->input_brush_);
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, reinterpret_cast<HWND>(lp) == self->label_pos_ ||
                                     reinterpret_cast<HWND>(lp) == self->label_count_
                                 ? kMuted
                                 : kText);
            SetBkColor(dc, kBg);
            return reinterpret_cast<LRESULT>(self->dark_brush_);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc = ps.rcPaint;
            FillRect(dc, &rc, self->dark_brush_);

            RECT side{0, 0, 320, rc.bottom};
            FillRect(dc, &side, self->panel_brush_);
            RECT divider{0, 92, 320, 93};
            FillRect(dc, &divider, self->border_brush_);

            HFONT old = static_cast<HFONT>(SelectObject(dc, self->title_font_));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kPrimary);
            TextOutW(dc, 20, 16, L"\x25AA", 1);
            TextOutW(dc, 40, 14, L"ClipLite", 8);
            SelectObject(dc, old);

            RECT vr;
            GetWindowRect(self->video_, &vr);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&vr), 2);
            InflateRect(&vr, 1, 1);
            FrameRect(dc, &vr, self->border_brush_);

            if (self->empty_visible_) self->draw_empty_state(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            self->player_.close();
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ID);
            self->player_.close();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK LibraryWindow::VideoViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<LibraryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            return 0;
        case WM_KEYDOWN:
            if (!self) break;
            switch (wp) {
                case VK_SPACE:
                    self->toggle_play();
                    return 0;
                case VK_LEFT:
                    self->player_.seek_ms(self->player_.position_ms() - 5000);
                    return 0;
                case VK_RIGHT:
                    self->player_.seek_ms(self->player_.position_ms() + 5000);
                    return 0;
                case 'M':
                    self->muted_ = !self->muted_;
                    self->player_.set_mute(self->muted_);
                    return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            FillRect(dc, &ps.rcPaint,
                     self ? self->video_brush_
                          : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace cliplite::ui
