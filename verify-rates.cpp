#include <windows.h>
#include <cwchar>
#include "include/PluginInterface.h"

bool edited = false;

void CALLBACK EditRates(HWND, UINT, UINT_PTR timer, DWORD) {
    const auto dialog = GetActiveWindow();
    if (!dialog || !GetDlgItem(dialog, 1013)) return;
    KillTimer(nullptr, timer);
    SetDlgItemTextW(dialog, 1013, L"0.2");
    SendDlgItemMessageW(dialog, 1012, CB_SETCURSEL, 1, 0);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(1012, CBN_SELCHANGE), 0);
    SetDlgItemTextW(dialog, 1013, L"0.4");
    edited = true;
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
}

int wmain(int argc, wchar_t** argv) {
    const auto module = LoadLibraryW(argv[1]);
    if (!module) return 1;
    const auto entry = reinterpret_cast<ITMPlugin*(*)()>(GetProcAddress(module, "TMPluginGetInstance"));
    if (!entry) return 2;
    const auto plugin = entry();
    plugin->DataRequired();
    if (argc > 3) {
        if (!SetTimer(nullptr, 0, 100, EditRates)) return 4;
        if (plugin->ShowOptionsDialog(nullptr) != ITMPlugin::OR_OPTION_CHANGED || !edited) return 5;
    }
    const wchar_t* actual = plugin->GetItem(1)->GetItemValueText();
    if (!wcsstr(actual, argv[2])) {
        wprintf(L"Expected %ls, got %ls\n", argv[2], actual);
        return 3;
    }
    return 0;
}
