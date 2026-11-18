#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <winsqlite/winsqlite3.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <cwchar>
#include <cmath>
#include <ctime>
#include "include/PluginInterface.h"

namespace {

struct Period {
    const wchar_t* label;
    int days;
};

// A zero day count selects a rolling window instead of local calendar dates.
constexpr Period periods[] = {{L"今日", 1}, {L"近24小时", 0}, {L"近7天", 7}, {L"近30天", 30}};
constexpr int periodCount = sizeof(periods) / sizeof(periods[0]);
constexpr int usdCommand = periodCount;
constexpr int cnyCommand = periodCount + 1;
struct RefreshInterval {
    int seconds;
    const wchar_t* label;
};

constexpr RefreshInterval refreshIntervals[] = {
    {5, L"5 秒"}, {15, L"15 秒"}, {30, L"30 秒"}, {60, L"1 分钟"},
    {120, L"2 分钟"}, {300, L"5 分钟"}, {600, L"10 分钟"}, {1800, L"30 分钟"}
};
constexpr int defaultRefreshInterval = 2;
constexpr int refreshIntervalCount = _countof(refreshIntervals);

struct CurrencySettings {
    bool preferCny{};
    double cnyPerUsd{};
};

CurrencySettings ReadCurrencySettings() {
    const wchar_t* profile = _wgetenv(L"USERPROFILE");
    if (!profile || !*profile)
        return {};
    std::ifstream file(std::filesystem::path(profile) / L".cc-switch" / L"settings.json",
                       std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= 0 || size > 1024 * 1024)
        return {};
    std::string json(static_cast<size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(json.data(), size))
        return {};

    sqlite3* handle = nullptr;
    const int opened = sqlite3_open(":memory:", &handle);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(handle, sqlite3_close);
    if (opened != SQLITE_OK)
        return {};

    constexpr char query[] = R"sql(
        SELECT json_extract(?1, '$.usageDisplay.currency') = 'CNY',
               CASE json_extract(?1, '$.usageDisplay.rateSource')
                   WHEN 'auto' THEN json_extract(?1, '$.usageDisplay.autoExchangeRates.CNY')
                   WHEN 'manual' THEN json_extract(?1, '$.usageDisplay.manualExchangeRates.CNY')
               END
    )sql";
    sqlite3_stmt* statement = nullptr;
    const int prepared = sqlite3_prepare_v2(handle, query, -1, &statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
    if (prepared != SQLITE_OK ||
        sqlite3_bind_text(statement, 1, json.data(), static_cast<int>(json.size()), SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW)
        return {};

    const double rate = sqlite3_column_double(statement, 1);
    return {sqlite3_column_int(statement, 0) != 0,
            std::isfinite(rate) && rate > 0 && rate <= 1000 ? rate : 0.0};
}

struct Totals {
    long long requests{};
    long long input{};
    long long output{};
    long long cacheRead{};
    long long cacheWrite{};
    double cost{};
};

constexpr ULONGLONG ticksPerSecond = 10000000ULL;
constexpr ULONGLONG ticksPerDay = 86400 * ticksPerSecond;
constexpr ULONGLONG unixEpoch = 116444736000000000ULL;

bool PeriodBoundary(int offset, sqlite3_int64& timestamp, char (&date)[11], SYSTEMTIME local) {
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&local, &fileTime))
        return false;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    ticks.QuadPart += static_cast<long long>(offset) * static_cast<long long>(ticksPerDay);
    fileTime = {ticks.LowPart, ticks.HighPart};

    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&fileTime, &local) ||
        !TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) ||
        !SystemTimeToFileTime(&utc, &fileTime))
        return false;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    timestamp = (ticks.QuadPart - unixEpoch) / ticksPerSecond;
    return sprintf_s(date, "%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay) == 10;
}

struct Provider {
    std::wstring app;
    std::wstring id;
    std::wstring label;

    bool operator==(const Provider& other) const {
        return app == other.app && id == other.id;
    }
};

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

Database OpenUsageDatabase() {
    Database database(nullptr, sqlite3_close);
    const wchar_t* profile = _wgetenv(L"USERPROFILE");
    if (!profile || !*profile)
        return database;
    const auto path = std::filesystem::path(profile) / L".cc-switch" / L"cc-switch.db";
    sqlite3* handle = nullptr;
    const int opened = sqlite3_open_v2(path.u8string().c_str(), &handle, SQLITE_OPEN_READONLY, nullptr);
    database.reset(handle);
    if (opened != SQLITE_OK)
        database.reset();
    else
        sqlite3_busy_timeout(handle, 50);
    return database;
}

std::wstring TextColumn(sqlite3_stmt* row, int column) {
    const auto* text = static_cast<const wchar_t*>(sqlite3_column_text16(row, column));
    return text ? text : L"";
}

std::vector<Provider> ReadProviders() {
    auto database = OpenUsageDatabase();
    if (!database)
        return {};
    constexpr char query[] = R"sql(
        WITH identities AS (
            SELECT app_type, id AS provider_id FROM providers
            UNION SELECT app_type, provider_id FROM proxy_request_logs
            UNION SELECT app_type, provider_id FROM usage_daily_rollups
        )
        SELECT i.app_type, i.provider_id, coalesce(nullif(p.name, ''), i.provider_id)
        FROM identities i LEFT JOIN providers p ON p.app_type = i.app_type AND p.id = i.provider_id
        WHERE i.provider_id <> '' AND i.app_type IS NOT NULL
        ORDER BY 3 COLLATE NOCASE, 1, 2
    )sql";
    sqlite3_stmt* statement = nullptr;
    const int prepared = sqlite3_prepare_v2(database.get(), query, -1, &statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
    if (prepared != SQLITE_OK)
        return {};
    std::vector<Provider> providers;
    int status;
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        auto app = TextColumn(statement, 0);
        auto id = TextColumn(statement, 1);
        auto label = TextColumn(statement, 2) + L" (" + app + L")";
        providers.push_back({std::move(app), std::move(id), std::move(label)});
    }
    return status == SQLITE_DONE ? std::move(providers) : std::vector<Provider>{};
}

bool ReadUsage(const Period& period, Totals& result, const std::vector<Provider>& providers = {}) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    sqlite3_int64 start{}, end{};
    char startDate[11], endDate[11];
    if (period.days == 0) {
        const auto now = std::time(nullptr);
        if (now == static_cast<std::time_t>(-1))
            return false;
        start = now - 24 * 60 * 60;
        end = now + 1;
        // Archived daily totals cannot be split at an hourly boundary.
        startDate[0] = endDate[0] = '\0';
    } else if (!PeriodBoundary(1 - period.days, start, startDate, local) ||
               !PeriodBoundary(1, end, endDate, local)) {
        return false;
    }
    auto database = OpenUsageDatabase();
    if (!database)
        return false;

    std::string filter;
    if (!providers.empty()) {
        filter = " AND (app_type, provider_id) IN (VALUES ";
        for (size_t index = 0; index < providers.size(); ++index) {
            if (index) filter += ", ";
            const auto parameter = 5 + index * 2;
            filter += "(?" + std::to_string(parameter) + ", ?" + std::to_string(parameter + 1) + ")";
        }
        filter += ")";
    }
    const std::string query = R"sql(
        WITH totals AS (
            SELECT count(*) AS requests, sum(input_tokens) AS input, sum(output_tokens) AS output,
                   sum(cache_read_tokens) AS cache_read, sum(cache_creation_tokens) AS cache_write,
                   sum(CAST(total_cost_usd AS REAL)) AS cost
            FROM proxy_request_logs
            WHERE created_at >= ?1
              AND created_at < ?2
              )sql" + filter + R"sql(
            UNION ALL
            SELECT sum(request_count), sum(input_tokens), sum(output_tokens),
                   sum(cache_read_tokens), sum(cache_creation_tokens), sum(CAST(total_cost_usd AS REAL))
            FROM usage_daily_rollups
            WHERE date >= ?3 AND date < ?4
              )sql" + filter + R"sql(
        )
        SELECT sum(requests), sum(input), sum(output), sum(cache_read), sum(cache_write), sum(cost)
        FROM totals
    )sql";
    sqlite3_stmt* statement = nullptr;
    const int prepared = sqlite3_prepare_v2(database.get(), query.c_str(), -1, &statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
    if (prepared != SQLITE_OK ||
        sqlite3_bind_int64(statement, 1, start) != SQLITE_OK ||
        sqlite3_bind_int64(statement, 2, end) != SQLITE_OK ||
        sqlite3_bind_text(statement, 3, startDate, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(statement, 4, endDate, -1, SQLITE_STATIC) != SQLITE_OK)
        return false;
    int parameter = 5;
    for (const auto& provider : providers) {
        if (sqlite3_bind_text16(statement, parameter++, provider.app.c_str(), -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text16(statement, parameter++, provider.id.c_str(), -1, SQLITE_STATIC) != SQLITE_OK)
            return false;
    }
    if (sqlite3_step(statement) != SQLITE_ROW)
        return false;

    result = {sqlite3_column_int64(statement, 0), sqlite3_column_int64(statement, 1),
              sqlite3_column_int64(statement, 2), sqlite3_column_int64(statement, 3),
              sqlite3_column_int64(statement, 4), sqlite3_column_double(statement, 5)};
    return true;
}

class UsageItem final : public IPluginItem {
public:
    UsageItem(const wchar_t* name, const wchar_t* id, const wchar_t* label, const wchar_t* sample, int slot, void (*activate)(bool))
        : name_(name), id_(id), label_(label), sample_(sample), slot_(slot), activate_(activate) {}

    const wchar_t* GetItemName() const override { return name_; }
    const wchar_t* GetItemId() const override { return id_; }
    const wchar_t* GetItemLableText() const override { return label_; }
    const wchar_t* GetItemValueSampleText() const override { return sample_; }

    const wchar_t* GetItemValueText() const override {
        thread_local wchar_t values[2][48];
        auto& copy = values[slot_];
        const std::lock_guard<std::mutex> lock(mutex_);
        wcscpy_s(copy, value_);
        return copy;
    }

    bool IsCustomDraw() const override { return true; }
    int GetItemWidth() const override { return 145; }
    int GetItemWidthEx(void* dc) const override {
        SIZE size{};
        const std::wstring value = GetItemValueText();
        const std::wstring text = std::wstring(label_) +
            (value.find(L"--") == std::wstring::npos ? value : sample_);
        if (!dc || !GetTextExtentPoint32W(static_cast<HDC>(dc), text.c_str(),
                                         static_cast<int>(text.size()), &size))
            return 0;
        return size.cx + 6;
    }
    void DrawItem(void* dc, int x, int y, int w, int h, bool dark) override {
        auto canvas = static_cast<HDC>(dc);
        const int saved = SaveDC(canvas);
        if (!saved) return;
        SetBkMode(canvas, TRANSPARENT);
        SetTextColor(canvas, slot_ == 0 ? (dark ? RGB(125, 211, 252) : RGB(3, 105, 161))
                                      : (dark ? RGB(252, 211, 77) : RGB(146, 64, 14)));
        const std::wstring text = DisplayText();
        RECT area{x + 2, y, x + w, y + h};
        DrawTextW(canvas, text.c_str(), -1, &area, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        RestoreDC(canvas, saved);
    }

    int OnMouseEvent(MouseEventType type, int, int, void*, int) override {
        if (type != MT_DBCLICKED)
            return 0;
        activate_(slot_ == 1 && (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 1;
    }

    void SetValue(const Period& period, const wchar_t* value) {
        const std::lock_guard<std::mutex> lock(mutex_);
        swprintf_s(value_, L"%ls %ls", period.label, value);
    }

private:
    std::wstring DisplayText() const {
        return std::wstring(label_) + GetItemValueText();
    }

    const wchar_t* name_;
    const wchar_t* id_;
    const wchar_t* label_;
    const wchar_t* sample_;
    const int slot_;
    void (*activate_)(bool);
    mutable std::mutex mutex_;
    wchar_t value_[48] = L"--";
};

class UsagePlugin final : public ITMPlugin {
public:
    static UsagePlugin& Instance() {
        static UsagePlugin plugin;
        return plugin;
    }

    IPluginItem* GetItem(int index) override {
        switch (index) {
        case 0: return &tokens_;
        case 1: return &cost_;
        default: return nullptr;
        }
    }

    void OnInitialize(ITrafficMonitor*) override {
        DataRequired();
    }

    void DataRequired() override {
        const std::lock_guard<std::mutex> refresh(refreshMutex_);
        Refresh(false);
    }

    OptionReturn ShowOptionsDialog(void* parent) override {
        Options options;
        {
            const std::lock_guard<std::mutex> lock(refreshMutex_);
            Refresh(false);
            options = {selected_, showCny_, billingRate_, refreshInterval_};
            options.selectedProviders[0] = tokenProviders_;
            options.selectedProviders[1] = costProviders_;
            try {
                options.providers = ReadProviders();
            } catch (...) {
                // Keep the existing filter if the database cannot be read.
            }
            options.providersAvailable = !options.providers.empty();
            for (const auto& selection : options.selectedProviders) {
                for (const auto& provider : selection) {
                    if (std::find(options.providers.begin(), options.providers.end(), provider) == options.providers.end())
                        options.providers.push_back(provider);
                }
            }
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        if (!InitCommonControlsEx(&controls))
            return OR_OPTION_UNCHANGED;
        static const int moduleAnchor = 0;
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&moduleAnchor), &module))
            return OR_OPTION_UNCHANGED;

        if (DialogBoxParamW(module, MAKEINTRESOURCEW(101), static_cast<HWND>(parent),
                            OptionsProcedure, reinterpret_cast<LPARAM>(&options)) != IDOK)
            return OR_OPTION_UNCHANGED;

        const std::lock_guard<std::mutex> lock(refreshMutex_);
        if (options.selectedProviders[0] == tokenProviders_ && options.selectedProviders[1] == costProviders_ &&
            options.period == selected_ && options.cny == showCny_ && options.rate == billingRate_ && options.refreshInterval == refreshInterval_ && preferencesSaved_)
            return OR_OPTION_UNCHANGED;
        if (!SavePreferences(options.period, options.cny, options.rate, options.refreshInterval,
                             options.selectedProviders[0], options.selectedProviders[1])) {
            MessageBoxW(static_cast<HWND>(parent), L"无法保存插件设置。", L"CC Switch Usage", MB_ICONERROR);
            return OR_OPTION_UNCHANGED;
        }
        preferencesSaved_ = true;
        tokenProviders_ = std::move(options.selectedProviders[0]);
        costProviders_ = std::move(options.selectedProviders[1]);
        billingRate_ = options.rate;
        refreshInterval_ = options.refreshInterval;
        selected_ = options.period;
        showCny_ = options.cny;
        currencyInitialized_ = true;
        Refresh(true);
        return OR_OPTION_CHANGED;
    }

    int GetCommandCount() override { return periodCount + 2; }

    const wchar_t* GetCommandName(int index) override {
        if (index == usdCommand) return L"费用显示为美元 (USD)";
        if (index == cnyCommand) return L"费用显示为人民币 (CNY)";
        return index >= 0 && index < periodCount ? periods[index].label : L"";
    }

    int IsCommandChecked(int index) override {
        const std::lock_guard<std::mutex> lock(refreshMutex_);
        if (index == usdCommand) return !showCny_;
        if (index == cnyCommand) return showCny_;
        return index == selected_;
    }

    void OnPluginCommand(int index, void*, void*) override {
        if (index < 0 || index > cnyCommand)
            return;
        const std::lock_guard<std::mutex> lock(refreshMutex_);
        const int period = index < periodCount ? index : selected_;
        const bool cny = index < periodCount ? showCny_ : index == cnyCommand;
        ApplyShortcut(period, cny);
        Refresh(true);
    }

private:
    static std::wstring SettingsPath() {
        const wchar_t* profile = _wgetenv(L"APPDATA");
        if (!profile || !*profile) return {};
        return (std::filesystem::path(profile) / L"TrafficMonitor" / L"CCSwitchUsage.ini").wstring();
    }

    static double LoadBillingRate() {
        wchar_t value[64]{};
        const auto path = SettingsPath();
        if (path.empty()) return 0;
        GetPrivateProfileStringW(L"Billing", L"CnyPerUsd", L"", value, _countof(value), path.c_str());
        wchar_t* end{};
        const double rate = wcstod(value, &end);
        return end != value && !*end && std::isfinite(rate) && rate > 0 && rate <= 1000 ? rate : 0;
    }

    static bool SavePreferences(int period, bool cny, double rate, int refreshInterval,
                                const std::vector<Provider>& tokens, const std::vector<Provider>& costs) {
        try {
            const auto legacy = SettingsPath();
            if (legacy.empty()) return false;
            auto path = std::filesystem::path(legacy).replace_extension(L".prefs");
            std::filesystem::create_directories(path.parent_path());
            auto pending = path;
            pending += L".tmp";
            std::wostringstream output;
            output.imbue(std::locale::classic());
            output << L"1 " << period << L' ' << cny << L' ' << std::setprecision(17) << rate;
            for (const auto* providers : {&tokens, &costs}) {
                output << L' ' << providers->size();
                for (const auto& provider : *providers)
                    output << L' ' << std::quoted(provider.app) << L' ' << std::quoted(provider.id)
                           << L' ' << std::quoted(provider.label);
            }
            output << L' ' << refreshIntervals[refreshInterval].seconds;
            const auto value = output.str();
            std::ofstream file(pending, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t));
            file.close();
            if (file && MoveFileExW(pending.c_str(), path.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            DeleteFileW(pending.c_str());
        } catch (...) {
            return false;
        }
        return false;
    }

    void RestorePreferences() {
        try {
            const auto legacy = SettingsPath();
            if (legacy.empty()) return;
            const auto path = std::filesystem::path(legacy).replace_extension(L".prefs");
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            const auto size = file.tellg();
            if (size <= 0 || size > 1024 * 1024 || size % sizeof(wchar_t) != 0) return;
            std::wstring value(static_cast<size_t>(size) / sizeof(wchar_t), L'\0');
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(value.data()), size)) return;
            std::wistringstream input(value);
            input.imbue(std::locale::classic());
            int version{}, period{}, cny{};
            double rate{};
            if (!(input >> version >> period >> cny >> rate) || version != 1 ||
                period < 0 || period >= periodCount || (cny != 0 && cny != 1) ||
                !std::isfinite(rate) || rate < 0 || rate > 1000)
                return;
            std::vector<Provider> tokens, costs;
            for (auto* providers : {&tokens, &costs}) {
                size_t count{};
                if (!(input >> count) || count > 4096) return;
                for (size_t index = 0; index < count; ++index) {
                    Provider provider;
                    if (!(input >> std::quoted(provider.app) >> std::quoted(provider.id) >>
                          std::quoted(provider.label)) || provider.app.empty() || provider.id.empty())
                        return;
                    providers->push_back(std::move(provider));
                }
            }
            int seconds{};
            if (input >> seconds) {
                for (int index = 0; index < refreshIntervalCount; ++index) {
                    if (refreshIntervals[index].seconds == seconds)
                        refreshInterval_ = index;
                }
            }
            selected_ = period;
            showCny_ = cny != 0;
            billingRate_ = rate;
            tokenProviders_ = std::move(tokens);
            costProviders_ = std::move(costs);
            currencyInitialized_ = true;
            preferencesSaved_ = true;
        } catch (...) {
            // Use defaults and the legacy billing rate if preferences cannot be read.
        }
    }

    void ApplyShortcut(int period, bool cny) {
        preferencesSaved_ = SavePreferences(period, cny, billingRate_, refreshInterval_, tokenProviders_, costProviders_);
        selected_ = period;
        showCny_ = cny;
        currencyInitialized_ = true;
    }

    UsagePlugin() { RestorePreferences(); }

    struct Options {
        int period{};
        bool cny{};
        double rate{};
        int refreshInterval = defaultRefreshInterval;
        std::vector<Provider> providers;
        std::vector<Provider> selectedProviders[2];
        bool providersAvailable{};
    };

    static INT_PTR CALLBACK OptionsProcedure(HWND dialog, UINT message, WPARAM command, LPARAM data) {
        constexpr int refreshControl = 1010;
        constexpr int periodControl = 1001;
        constexpr int currencyControl = 1002;
        constexpr int providerControls[] = {1004, 1005};
        constexpr int selectAllControls[] = {1006, 1008};
        constexpr int invertControls[] = {1007, 1009};
        if (message == WM_INITDIALOG) {
            auto* options = reinterpret_cast<Options*>(data);
            SetWindowLongPtrW(dialog, DWLP_USER, data);
            for (const auto& period : periods)
                SendDlgItemMessageW(dialog, periodControl, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(period.label));
            for (const auto* currency : {L"美元 (USD)", L"人民币 (CNY)"})
                SendDlgItemMessageW(dialog, currencyControl, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(currency));
            for (const auto& interval : refreshIntervals)
                SendDlgItemMessageW(dialog, refreshControl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(interval.label));
            SendDlgItemMessageW(dialog, refreshControl, CB_SETCURSEL, options->refreshInterval, 0);
            SendDlgItemMessageW(dialog, periodControl, CB_SETCURSEL, options->period, 0);
            SendDlgItemMessageW(dialog, currencyControl, CB_SETCURSEL, options->cny, 0);
            if (options->rate > 0) {
                wchar_t value[64];
                swprintf_s(value, L"%.15g", options->rate);
                SetDlgItemTextW(dialog, 1003, value);
            }
            SendDlgItemMessageW(dialog, 1003, EM_SETLIMITTEXT, 63, 0);
            for (int group = 0; group < 2; ++group) {
                const auto list = GetDlgItem(dialog, providerControls[group]);
                ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                RECT area{};
                GetClientRect(list, &area);
                LVCOLUMNW column{};
                column.mask = LVCF_WIDTH;
                column.cx = area.right - GetSystemMetrics(SM_CXVSCROLL) - 2;
                SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
                for (int index = 0; index < static_cast<int>(options->providers.size()); ++index) {
                    auto& provider = options->providers[index];
                    LVITEMW item{};
                    item.mask = LVIF_TEXT;
                    item.iItem = index;
                    item.pszText = provider.label.data();
                    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
                    const auto& selected = options->selectedProviders[group];
                    ListView_SetCheckState(list, index, std::find(selected.begin(), selected.end(), provider) != selected.end());
                }
                EnableWindow(list, options->providersAvailable);
                EnableWindow(GetDlgItem(dialog, selectAllControls[group]), options->providersAvailable);
                EnableWindow(GetDlgItem(dialog, invertControls[group]), options->providersAvailable);
            }
            return TRUE;
        }
        if (message != WM_COMMAND)
            return FALSE;
        const int control = LOWORD(command);
        for (int group = 0; group < 2; ++group) {
            const bool selectAll = control == selectAllControls[group];
            if (!selectAll && control != invertControls[group])
                continue;
            const auto list = GetDlgItem(dialog, providerControls[group]);
            for (int index = 0; index < ListView_GetItemCount(list); ++index)
                ListView_SetCheckState(list, index, selectAll || !ListView_GetCheckState(list, index));
            return TRUE;
        }
        if (control == IDOK) {
            auto* options = reinterpret_cast<Options*>(GetWindowLongPtrW(dialog, DWLP_USER));
            wchar_t value[64]{};
            GetDlgItemTextW(dialog, 1003, value, _countof(value));
            wchar_t* end{};
            options->rate = wcstod(value, &end);
            if (*value && (end == value || *end || !std::isfinite(options->rate) ||
                           options->rate <= 0 || options->rate > 1000)) {
                MessageBoxW(dialog, L"请输入大于 0 且不超过 1000 的数字，或留空使用 CC Switch 汇率。",
                            L"费用设置", MB_ICONWARNING);
                return TRUE;
            }
            const auto period = SendDlgItemMessageW(dialog, periodControl, CB_GETCURSEL, 0, 0);
            const auto refresh = SendDlgItemMessageW(dialog, refreshControl, CB_GETCURSEL, 0, 0);
            if (period < 0 || period >= periodCount || refresh < 0 || refresh >= refreshIntervalCount)
                return TRUE;
            options->period = static_cast<int>(period);
            options->refreshInterval = static_cast<int>(refresh);
            options->cny = SendDlgItemMessageW(dialog, currencyControl, CB_GETCURSEL, 0, 0) == 1;
            if (options->providersAvailable) {
                for (int group = 0; group < 2; ++group) {
                    auto& selected = options->selectedProviders[group];
                    selected.clear();
                    const auto list = GetDlgItem(dialog, providerControls[group]);
                    for (int index = 0; index < ListView_GetItemCount(list); ++index) {
                        if (ListView_GetCheckState(list, index))
                            selected.push_back(options->providers[index]);
                    }
                }
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (control == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    static void ActivateItem(bool toggleCurrency) {
        auto& plugin = Instance();
        const std::lock_guard<std::mutex> lock(plugin.refreshMutex_);
        const int period = toggleCurrency ? plugin.selected_ : (plugin.selected_ + 1) % periodCount;
        const bool cny = toggleCurrency ? !plugin.showCny_ : plugin.showCny_;
        plugin.ApplyShortcut(period, cny);
        plugin.Refresh(true);
    }

    void Refresh(bool force) {
        const auto now = GetTickCount64();
        if (!force && nextRefresh_ && now < nextRefresh_)
            return;
        nextRefresh_ = now + refreshIntervals[refreshInterval_].seconds * 1000ULL;

        const auto& period = periods[selected_];
        Totals usage, costUsage;
        CurrencySettings currency;
        bool available = false;
        bool costAvailable = false;
        try {
            currency = ReadCurrencySettings();
            available = ReadUsage(period, usage, tokenProviders_);
            if (tokenProviders_ == costProviders_) {
                costUsage = usage;
                costAvailable = available;
            } else {
                costAvailable = ReadUsage(period, costUsage, costProviders_);
            }
        } catch (...) {
            // Keep failures in the plugin from propagating into TrafficMonitor.
        }
        if (!currencyInitialized_) {
            showCny_ = currency.preferCny;
            currencyInitialized_ = true;
        }
        const std::lock_guard<std::mutex> lock(tooltipMutex_);
        if (!available && !costAvailable) {
            tokens_.SetValue(period, L"--");
            cost_.SetValue(period, L"--");
            wcscpy_s(tooltip_, L"CC Switch 用量暂不可读，将在下次刷新时重试。");
            return;
        }

        const auto total = usage.input + usage.output;
        wchar_t value[32];
        if (total >= 1000000000)
            swprintf_s(value, L"%.2fB", total / 1e9);
        else if (total >= 1000000)
            swprintf_s(value, L"%.2fM", total / 1e6);
        else if (total >= 1000)
            swprintf_s(value, L"%.1fK", total / 1e3);
        else
            swprintf_s(value, L"%lld", total);
        tokens_.SetValue(period, available ? value : L"--");

        wchar_t converted[128]{};
        const double rate = billingRate_ > 0 ? billingRate_ : currency.cnyPerUsd;
        if (!showCny_) {
            swprintf_s(value, L"$%.4f", costUsage.cost);
        } else if (rate > 0) {
            swprintf_s(value, L"¥%.4f", costUsage.cost * rate);
            swprintf_s(converted, L"折算 ≈ %.6f CNY（%ls %.8g 元/USD）\n",
                       costUsage.cost * rate, billingRate_ > 0 ? L"自定义实付" : L"CC Switch 汇率", rate);
        } else {
            wcscpy_s(value, L"¥--");
            wcscpy_s(converted, L"CC Switch 暂无可用人民币汇率。\n");
        }
        cost_.SetValue(period, costAvailable ? value : L"--");
        const auto describeScope = [](const std::vector<Provider>& providers) -> std::wstring {
            if (providers.empty()) return L"全部供应商";
            if (providers.size() == 1) return providers.front().label;
            return std::to_wstring(providers.size()) + L" 个供应商";
        };
        wchar_t tokenDetails[192] = L"Token 数据暂不可读";
        if (available)
            swprintf_s(tokenDetails, L"请求 %lld 次 · 输入 %lld · 输出 %lld Token\n缓存读取 %lld · 缓存写入 %lld Token",
                       usage.requests, usage.input, usage.output, usage.cacheRead, usage.cacheWrite);
        wchar_t costDetails[96] = L"费用数据暂不可读";
        if (costAvailable)
            swprintf_s(costDetails, L"记录费用 %.6f USD", costUsage.cost);
        swprintf_s(tooltip_,
            L"CC Switch %ls（本地时间）\nToken 范围 %.65ls\n%ls\n"
            L"费用范围 %.65ls\n%ls\n%ls双击切换周期，Shift + 双击费用切换币种",
            period.label, describeScope(tokenProviders_).c_str(), tokenDetails,
            describeScope(costProviders_).c_str(), costDetails, costAvailable ? converted : L"");
    }

public:
    const wchar_t* GetTooltipInfo() override {
        thread_local wchar_t copy[768];
        const std::lock_guard<std::mutex> lock(tooltipMutex_);
        wcscpy_s(copy, tooltip_);
        return copy;
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME: return L"CC Switch Usage";
        case TMI_DESCRIPTION: return L"显示 CC Switch Token 和费用，支持切换统计周期与显示币种。";
        case TMI_AUTHOR: return L"CC Switch Usage contributors";
        case TMI_VERSION: return L"0.1.14";
        default: return L"";
        }
    }

private:
    UsageItem tokens_{L"CC Switch Token", L"CCSwitchUsage.Tokens", L"Token ", L"近24小时 999.99M", 0, ActivateItem};
    UsageItem cost_{L"CC Switch 费用", L"CCSwitchUsage.Cost", L"费用 ", L"近24小时 ¥9999.9999", 1, ActivateItem};
    std::mutex refreshMutex_;
    std::mutex tooltipMutex_;
    std::vector<Provider> tokenProviders_;
    std::vector<Provider> costProviders_;
    int selected_{};
    bool showCny_{};
    double billingRate_ = LoadBillingRate();
    bool currencyInitialized_{};
    bool preferencesSaved_{};
    int refreshInterval_ = defaultRefreshInterval;
    ULONGLONG nextRefresh_{};
    wchar_t tooltip_[768] = L"正在读取 CC Switch 用量。";
};

} // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &UsagePlugin::Instance();
}
