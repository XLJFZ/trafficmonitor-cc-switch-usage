#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <winsqlite/winsqlite3.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <array>
#include <sstream>
#include <iomanip>
#include <string>
#include <memory>
#include <mutex>
#include <cwchar>
#include <cmath>
#include <ctime>
#include <thread>
#include <atomic>
#include "include/PluginInterface.h"

namespace {

struct Period {
    const wchar_t* label;
    int days;
};

// A zero day count selects a rolling window instead of local calendar dates.
constexpr Period periods[] = {{L"今日", 1}, {L"近24小时", 0}, {L"近7天", 7}, {L"近30天", 30}, {L"总计", -1}};
constexpr int periodCount = sizeof(periods) / sizeof(periods[0]);
constexpr int usdCommand = periodCount;
constexpr int cnyCommand = periodCount + 1;
constexpr int detailsCommand = periodCount + 2;
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

bool PeriodBoundary(int offset, sqlite3_int64& timestamp, char (&date)[11], SYSTEMTIME local) {
    std::tm calendar{};
    calendar.tm_year = static_cast<int>(local.wYear) - 1900;
    calendar.tm_mon = static_cast<int>(local.wMonth) - 1;
    calendar.tm_mday = static_cast<int>(local.wDay) + offset;
    calendar.tm_isdst = -1;
    const auto localMidnight = std::mktime(&calendar);
    if (localMidnight == static_cast<std::time_t>(-1))
        return false;
    timestamp = static_cast<sqlite3_int64>(localMidnight);
    return sprintf_s(date, "%04d-%02d-%02d", calendar.tm_year + 1900,
                     calendar.tm_mon + 1, calendar.tm_mday) == 10;
}

struct Provider {
    std::wstring app;
    std::wstring id;
    std::wstring label;
    std::wstring storedKey;

    std::wstring Key() const {
        if (!storedKey.empty()) return storedKey;
        const auto encode = [](const std::wstring& text) {
            std::wstring result;
            for (const auto character : text) {
                wchar_t hex[5];
                swprintf_s(hex, L"%04x", static_cast<unsigned int>(character));
                result += hex;
            }
            return result;
        };
        return encode(app) + L"." + encode(id);
    }

    bool operator==(const Provider& other) const {
        return Key() == other.Key();
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

struct ProviderUsage {
    Provider provider;
    Totals totals;
};

bool ReadUsage(const Period& period, std::vector<ProviderUsage>& usage) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    sqlite3_int64 start{}, end{};
    char startDate[11], endDate[11];
    if (period.days <= 0) {
        const auto now = std::time(nullptr);
        if (now == static_cast<std::time_t>(-1)) return false;
        end = now + 1;
        start = period.days == 0 ? now - 24 * 60 * 60 : 0;
        // Daily totals have no timestamp for a rolling window.
        strcpy_s(startDate, period.days == 0 ? "" : "0000-01-01");
        strcpy_s(endDate, period.days == 0 ? "" : "9999-12-31");
    } else if (!PeriodBoundary(1 - period.days, start, startDate, local) ||
               !PeriodBoundary(1, end, endDate, local)) {
        return false;
    }
    auto database = OpenUsageDatabase();
    if (!database) return false;
    constexpr char schemaQuery[] =
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
        "('proxy_request_logs','usage_daily_rollups')";
    sqlite3_stmt* schemaStatement = nullptr;
    if (sqlite3_prepare_v2(database.get(), schemaQuery, -1, &schemaStatement, nullptr) != SQLITE_OK)
        return false;
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> schema(schemaStatement, sqlite3_finalize);
    if (sqlite3_step(schemaStatement) != SQLITE_ROW || sqlite3_column_int(schemaStatement, 0) < 2)
        return false;
    constexpr char query[] = R"sql(
        WITH parts AS (
            SELECT app_type, provider_id, count(*) AS requests, sum(input_tokens) AS input,
                   sum(output_tokens) AS output, sum(cache_read_tokens) AS cache_read,
                   sum(cache_creation_tokens) AS cache_write,
                   sum(CAST(total_cost_usd AS REAL) * CAST(cost_multiplier AS REAL)) AS cost
            FROM proxy_request_logs WHERE created_at >= ?1 AND created_at < ?2
            GROUP BY app_type, provider_id
            UNION ALL
            SELECT app_type, provider_id, sum(request_count), sum(input_tokens), sum(output_tokens),
                   sum(cache_read_tokens), sum(cache_creation_tokens), sum(CAST(total_cost_usd AS REAL))
            FROM usage_daily_rollups r
            WHERE date >= ?3 AND date < ?4
              AND NOT EXISTS (
                  SELECT 1 FROM proxy_request_logs l
                  WHERE l.app_type = r.app_type AND l.provider_id = r.provider_id
                    AND date(l.created_at, 'unixepoch', 'localtime') = r.date
              )
            GROUP BY app_type, provider_id
        ), totals AS (
            SELECT app_type, provider_id, sum(requests) AS requests, sum(input) AS input,
                   sum(output) AS output, sum(cache_read) AS cache_read, sum(cache_write) AS cache_write,
                   sum(cost) AS cost
            FROM parts GROUP BY app_type, provider_id
        )
        SELECT t.app_type, t.provider_id, coalesce(nullif(p.name, ''), t.provider_id),
               t.requests, t.input, t.output, t.cache_read, t.cache_write, t.cost
        FROM totals t LEFT JOIN providers p ON p.app_type = t.app_type AND p.id = t.provider_id
        ORDER BY 3 COLLATE NOCASE, 1, 2
    )sql";
    sqlite3_stmt* statement = nullptr;
    const int prepared = sqlite3_prepare_v2(database.get(), query, -1, &statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
    if (prepared != SQLITE_OK ||
        sqlite3_bind_int64(statement, 1, start) != SQLITE_OK ||
        sqlite3_bind_int64(statement, 2, end) != SQLITE_OK ||
        sqlite3_bind_text(statement, 3, startDate, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(statement, 4, endDate, -1, SQLITE_STATIC) != SQLITE_OK)
        return false;
    int status{};
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        auto app = TextColumn(statement, 0);
        auto id = TextColumn(statement, 1);
        auto label = id.empty() ? std::wstring(L"未标记供应商") : TextColumn(statement, 2);
        if (!app.empty()) label += L" (" + app + L")";
        usage.push_back({{std::move(app), std::move(id), std::move(label)},
            {sqlite3_column_int64(statement, 3), sqlite3_column_int64(statement, 4),
             sqlite3_column_int64(statement, 5), sqlite3_column_int64(statement, 6),
             sqlite3_column_int64(statement, 7), sqlite3_column_double(statement, 8)}});
    }
    return status == SQLITE_DONE;
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
        const auto value = GetItemValueText();
        const std::wstring text = std::wstring(label_) + (wcsstr(value, L"--") ? sample_ : value);
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
    UsagePlugin() {
        const auto path = SettingsPath();
        if (path.empty()) return;
        LoadLegacyPreferences(path);
        providerFiltersInitialized_ = ReadSetting(path, L"ProviderFilters", L"Migrated") == L"1";
        selected_ = GetPrivateProfileIntW(L"Display", L"Period", selected_, path.c_str());
        if (selected_ < 0 || selected_ >= periodCount) selected_ = 0;
        const auto savedCurrency = GetPrivateProfileIntW(L"Display", L"Cny",
            currencyInitialized_ ? static_cast<int>(showCny_) : 2, path.c_str());
        currencyInitialized_ = savedCurrency <= 1;
        showCny_ = savedCurrency == 1;
        const auto rate = ReadSetting(path, L"Billing", L"CnyPerUsd");
        if (rate != L"~") billingRate_ = ParseRate(rate.c_str());
        const double multiplier = ParseRate(ReadSetting(path, L"Display", L"DefaultMultiplier").c_str());
        if (multiplier > 0) defaultMultiplier_ = multiplier;
        for (int group = 0; group < 2; ++group) {
            auto& selection = group == 0 ? tokenProviders_ : costProviders_;
            const auto stable = ReadSetting(path, L"ProviderFilters", FilterName(group));
            const auto legacy = ReadSetting(path, L"Providers", FilterName(group));
            if (stable != L"~") selection = ReadFilter(stable, false);
            else if (legacy != L"~") selection = ReadFilter(legacy, true);
        }
    }

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

    void DataRequired() override {
        bool expected = false;
        if (!refreshRunning_.compare_exchange_strong(expected, true)) return;
        std::thread([this] {
            {
                const std::lock_guard<std::mutex> refresh(refreshMutex_);
                Refresh(false);
            }
            refreshRunning_ = false;
        }).detach();
    }

    OptionReturn ShowOptionsDialog(void* parent) override {
        Options options;
        {
            const std::lock_guard<std::mutex> lock(refreshMutex_);
            Refresh(false);
            options.period = selected_;
            options.cny = showCny_;
            options.rate = billingRate_;
            options.defaultMultiplier = defaultMultiplier_;
            options.selectedProviders[0] = tokenProviders_;
            options.selectedProviders[1] = costProviders_;
            options.providers = ReadProviders();
            options.providersAvailable = !options.providers.empty();
            options.knownProviders = options.providers.size();
            const auto path = SettingsPath();
            for (const auto& provider : options.providers) {
                options.providerRateTexts.push_back(NumberText(ReadProviderNumber(path, L"ProviderRates", provider)));
                options.providerMultiplierTexts.push_back(NumberText(ReadProviderNumber(path, L"ProviderMultipliers", provider)));
            }
            for (const auto& selection : options.selectedProviders)
                for (const auto& provider : selection)
                    if (std::find(options.providers.begin(), options.providers.end(), provider) == options.providers.end())
                        options.providers.push_back(provider);
        }
        if (!InitControls()) return OR_OPTION_UNCHANGED;
        return DialogBoxParamW(Module(), MAKEINTRESOURCEW(101), static_cast<HWND>(parent),
                               OptionsProcedure, reinterpret_cast<LPARAM>(&options)) == IDOK
            ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
    }

    int GetCommandCount() override { return periodCount + 3; }

    const wchar_t* GetCommandName(int index) override {
        if (index == detailsCommand) return L"供应商费用明细";
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

    void OnPluginCommand(int index, void* parent, void*) override {
        if (index == detailsCommand) {
            ShowDetails(static_cast<HWND>(parent));
            return;
        }
        if (index < 0 || index > cnyCommand) return;
        bool saved;
        {
            const std::lock_guard<std::mutex> lock(refreshMutex_);
            saved = ApplyShortcut(index < periodCount ? index : selected_,
                                  index < periodCount ? showCny_ : index == cnyCommand);
        }
        if (!saved) ShowSaveError(static_cast<HWND>(parent));
    }

private:
    static std::wstring SettingsPath() {
        const wchar_t* profile = _wgetenv(L"APPDATA");
        if (!profile || !*profile) return {};
        return (std::filesystem::path(profile) / L"TrafficMonitor" / L"CCSwitchUsage.ini").wstring();
    }

    static std::wstring ReadSetting(const std::wstring& path, const wchar_t* section,
                                    const wchar_t* key) {
        if (path.empty()) return L"~";
        std::wstring value(256, L'\0');
        for (;;) {
            const auto size = GetPrivateProfileStringW(section, key, L"~", value.data(),
                                                        static_cast<DWORD>(value.size()), path.c_str());
            if (size < value.size() - 1) {
                value.resize(size);
                return value;
            }
            value.resize(value.size() * 2);
        }
    }

    static double ParseRate(const wchar_t* value) {
        wchar_t* end{};
        const double rate = wcstod(value, &end);
        return end != value && !*end && std::isfinite(rate) && rate > 0 && rate <= 1000 ? rate : 0;
    }

    static std::wstring NumberText(double number) {
        if (number == 0) return {};
        wchar_t value[64]{};
        swprintf_s(value, L"%.15g", number);
        return value;
    }

    static double ReadProviderNumber(const std::wstring& path, const wchar_t* section,
                                     const Provider& provider) {
        return ParseRate(ReadSetting(path, section, provider.Key().c_str()).c_str());
    }

    static const wchar_t* FilterName(int group) { return group == 0 ? L"Token" : L"Cost"; }

    static std::vector<Provider> ReadFilter(const std::wstring& text, bool legacy) {
        std::vector<Provider> selection;
        std::wistringstream entries(text);
        std::wstring entry;
        while (std::getline(entries, entry, L'|')) {
            if (entry.empty()) continue;
            if (legacy) entry.insert(0, L"?");
            const auto label = entry.front() == L'?' ? entry.substr(1)
                : L"供应商 " + entry;
            selection.push_back({{}, {}, label, entry});
        }
        return selection;
    }

    static std::wstring WriteFilter(const std::vector<Provider>& selection) {
        std::wstring value;
        for (const auto& provider : selection) {
            if (!value.empty()) value += L"|";
            value += provider.Key();
        }
        return value;
    }

    void LoadLegacyPreferences(const std::wstring& ini) {
        try {
            if (ReadSetting(ini, L"ProviderFilters", L"Token") != L"~") return;
            auto path = std::filesystem::path(ini).replace_extension(L".prefs");
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            const auto size = file.tellg();
            if (size <= 0 || size > 1024 * 1024 || size % sizeof(wchar_t) != 0) return;
            std::wstring text(static_cast<size_t>(size) / sizeof(wchar_t), L'\0');
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(text.data()), size)) return;
            std::wistringstream data(text);
            data.imbue(std::locale::classic());
            int version{}, period{}, cny{};
            double rate{};
            if (!(data >> version >> period >> cny >> rate) || version != 1 ||
                period < 0 || period >= 4 || (cny != 0 && cny != 1) ||
                !std::isfinite(rate) || rate < 0 || rate > 1000) return;
            std::vector<Provider> filters[2];
            for (auto& filter : filters) {
                size_t count{};
                if (!(data >> count) || count > 4096) return;
                for (size_t index = 0; index < count; ++index) {
                    Provider provider;
                    if (!(data >> std::quoted(provider.app) >> std::quoted(provider.id) >>
                          std::quoted(provider.label)) || provider.id.empty()) return;
                    filter.push_back(std::move(provider));
                }
            }
            selected_ = period;
            showCny_ = cny != 0;
            currencyInitialized_ = true;
            billingRate_ = rate;
            tokenProviders_ = std::move(filters[0]);
            costProviders_ = std::move(filters[1]);
        } catch (...) {
            // The INI and default settings are still available.
        }
    }

    struct Setting {
        const wchar_t* section;
        std::wstring key;
        std::wstring value;
    };

    static bool SaveSettings(const std::wstring& path, const std::vector<Setting>& settings) {
        if (path.empty()) return false;
        wchar_t pending[MAX_PATH]{};
        try {
            const auto directory = std::filesystem::path(path).parent_path();
            std::filesystem::create_directories(directory);
            if (!GetTempFileNameW(directory.c_str(), L"ccu", 0, pending)) return false;
            bool saved = true;
            if (std::filesystem::exists(path)) {
                saved = CopyFileW(path.c_str(), pending, FALSE) != FALSE;
            } else {
                std::ofstream file(pending, std::ios::binary | std::ios::trunc);
                const wchar_t bom = 0xfeff;
                file.write(reinterpret_cast<const char*>(&bom), sizeof(bom));
                file.close();
                saved = static_cast<bool>(file);
            }
            for (const auto& setting : settings) {
                if (!saved) break;
                saved = WritePrivateProfileStringW(setting.section, setting.key.c_str(),
                                                   setting.value.c_str(), pending) != FALSE;
            }
            if (saved) {
                WritePrivateProfileStringW(nullptr, nullptr, nullptr, pending);
                saved = MoveFileExW(pending, path.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                if (saved) {
                    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
                    return true;
                }
            }
        } catch (...) {
            // The existing settings remain available if a staging operation fails.
        }
        if (*pending) DeleteFileW(pending);
        return false;
    }

    static void ShowSaveError(HWND parent) {
        MessageBoxW(parent, L"设置未保存。请检查配置目录权限后重试。",
                    L"CC Switch Usage", MB_ICONERROR);
    }

    static HMODULE Module() {
        static const int anchor = 0;
        HMODULE module{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&anchor), &module);
        return module;
    }

    static bool InitControls() {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        return InitCommonControlsEx(&controls) != FALSE;
    }

    bool ApplyShortcut(int period, bool cny) {
        if (!SaveSettings(SettingsPath(), {{L"Display", L"Period", std::to_wstring(period)},
                                           {L"Display", L"Cny", cny ? L"1" : L"0"}}))
            return false;
        selected_ = period;
        showCny_ = cny;
        currencyInitialized_ = true;
        Refresh(true);
        return true;
    }

    void MigrateProviders(const std::vector<Provider>& providers, const std::wstring& path) {
        if (providers.empty()) return;
        auto tokens = tokenProviders_;
        auto costs = costProviders_;
        std::vector<Setting> settings;
        for (auto* selection : {&tokens, &costs}) {
            for (auto& saved : *selection) {
                const auto key = saved.Key();
                if (key.empty() || key.front() != L'?') continue;
                const auto label = key.substr(1);
                const auto match = std::find_if(providers.begin(), providers.end(), [&](const Provider& p) {
                    return p.label == label;
                });
                if (match != providers.end() &&
                    std::count_if(providers.begin(), providers.end(), [&](const Provider& p) {
                        return p.label == label;
                    }) == 1) saved = *match;
            }
        }
        for (const auto& provider : providers) {
            const double multiplier = ParseRate(ReadSetting(path, L"Multipliers", provider.label.c_str()).c_str());
            if (multiplier <= 0 || ReadSetting(path, L"ProviderMultipliers", provider.Key().c_str()) != L"~")
                continue;
            if (std::count_if(providers.begin(), providers.end(), [&](const Provider& p) {
                    return p.label == provider.label;
                }) == 1)
                settings.push_back({L"ProviderMultipliers", provider.Key(), NumberText(multiplier)});
        }
        settings.push_back({L"ProviderFilters", L"Token", WriteFilter(tokens)});
        settings.push_back({L"ProviderFilters", L"Cost", WriteFilter(costs)});
        settings.push_back({L"ProviderFilters", L"Migrated", L"1"});
        if (SaveSettings(path, settings)) providerFiltersInitialized_ = true;
        // Resolved identities remain valid in this session even if the migration cannot be saved.
        tokenProviders_ = std::move(tokens);
        costProviders_ = std::move(costs);
    }

    struct Options {
        int period{};
        bool cny{};
        double rate{};
        double defaultMultiplier{1.0};
        double selectedMultiplier{};
        std::vector<Provider> providers;
        std::vector<Provider> selectedProviders[2];
        bool providersAvailable{};
        size_t knownProviders{};
        std::vector<std::wstring> providerRateTexts;
        std::vector<std::wstring> providerMultiplierTexts;
        int editingProvider{-1};
    };

    bool SaveOptions(const Options& options) {
        const std::lock_guard<std::mutex> lock(refreshMutex_);
        std::vector<Setting> settings{
            {L"Display", L"Period", std::to_wstring(options.period)},
            {L"Display", L"Cny", options.cny ? L"1" : L"0"},
            {L"Display", L"DefaultMultiplier", NumberText(options.defaultMultiplier)},
            {L"Billing", L"CnyPerUsd", NumberText(options.rate)},
            {L"ProviderFilters", L"Token", WriteFilter(options.selectedProviders[0])},
            {L"ProviderFilters", L"Cost", WriteFilter(options.selectedProviders[1])}
        };
        for (size_t index = 0; index < options.knownProviders; ++index) {
            const auto& provider = options.providers[index];
            const auto& selected = options.selectedProviders[1];
            const bool batch = options.selectedMultiplier > 0 &&
                (selected.empty() || std::find(selected.begin(), selected.end(), provider) != selected.end());
            settings.push_back({L"ProviderRates", provider.Key(), options.providerRateTexts[index]});
            settings.push_back({L"ProviderMultipliers", provider.Key(), batch ?
                NumberText(options.selectedMultiplier) : options.providerMultiplierTexts[index]});
        }
        if (!SaveSettings(SettingsPath(), settings)) return false;
        selected_ = options.period;
        showCny_ = options.cny;
        currencyInitialized_ = true;
        billingRate_ = options.rate;
        defaultMultiplier_ = options.defaultMultiplier;
        tokenProviders_ = options.selectedProviders[0];
        costProviders_ = options.selectedProviders[1];
        Refresh(true);
        return true;
    }

    static void ShowProviderRate(HWND dialog, Options& options, int index) {
        options.editingProvider = -1;
        SetDlgItemTextW(dialog, 1013, options.providerRateTexts[index].c_str());
        SetDlgItemTextW(dialog, 1014, options.providerMultiplierTexts[index].c_str());
        options.editingProvider = index;
    }

    static INT_PTR CALLBACK OptionsProcedure(HWND dialog, UINT message, WPARAM command, LPARAM data) {
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
            SendDlgItemMessageW(dialog, periodControl, CB_SETCURSEL, options->period, 0);
            SendDlgItemMessageW(dialog, currencyControl, CB_SETCURSEL, options->cny, 0);
            if (options->selectedMultiplier > 0) {
                wchar_t selected[64];
                swprintf_s(selected, L"%.15g", options->selectedMultiplier);
                SetDlgItemTextW(dialog, 1011, selected);
            }
            if (options->defaultMultiplier > 0) {
                wchar_t multiplier[64];
                swprintf_s(multiplier, L"%.15g", options->defaultMultiplier);
                SetDlgItemTextW(dialog, 1010, multiplier);
            }
            if (options->rate > 0) {
                wchar_t value[64];
                swprintf_s(value, L"%.15g", options->rate);
                SetDlgItemTextW(dialog, 1003, value);
            }
            SendDlgItemMessageW(dialog, 1003, EM_SETLIMITTEXT, 63, 0);
            SendDlgItemMessageW(dialog, 1011, EM_SETLIMITTEXT, 63, 0);
            SendDlgItemMessageW(dialog, 1013, EM_SETLIMITTEXT, 63, 0);
            SendDlgItemMessageW(dialog, 1014, EM_SETLIMITTEXT, 63, 0);
            SendDlgItemMessageW(dialog, 1010, EM_SETLIMITTEXT, 63, 0);
            for (size_t index = 0; index < options->knownProviders; ++index) {
                const auto label = DisplayProvider(options->providers[index], options->providers);
                SendDlgItemMessageW(dialog, 1012, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            EnableWindow(GetDlgItem(dialog, 1012), options->providersAvailable);
            EnableWindow(GetDlgItem(dialog, 1013), options->providersAvailable);
            EnableWindow(GetDlgItem(dialog, 1014), options->providersAvailable);
            EnableWindow(GetDlgItem(dialog, 1011), options->providersAvailable);
            if (options->providersAvailable) {
                SendDlgItemMessageW(dialog, 1012, CB_SETCURSEL, 0, 0);
                ShowProviderRate(dialog, *options, 0);
            }
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
                    auto label = DisplayProvider(provider, options->providers);
                    item.pszText = label.data();
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
        auto* options = reinterpret_cast<Options*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (control == 1012 && HIWORD(command) == CBN_SELCHANGE) {
            const auto index = SendDlgItemMessageW(dialog, 1012, CB_GETCURSEL, 0, 0);
            if (index != CB_ERR) ShowProviderRate(dialog, *options, static_cast<int>(index));
            return TRUE;
        }
        if (control == 1015) {
            Instance().ShowDetails(dialog);
            return TRUE;
        }
        if ((control == 1013 || control == 1014) && HIWORD(command) == EN_CHANGE &&
            options->editingProvider >= 0) {
            wchar_t value[64]{};
            GetDlgItemTextW(dialog, control, value, _countof(value));
            auto& texts = control == 1013 ? options->providerRateTexts : options->providerMultiplierTexts;
            texts[options->editingProvider] = value;
            return TRUE;
        }
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
            for (const int field : {1013, 1014}) {
                const auto& texts = field == 1013 ? options->providerRateTexts : options->providerMultiplierTexts;
                for (int index = 0; index < static_cast<int>(texts.size()); ++index) {
                    const auto& value = texts[index];
                    if (value.empty() || ParseRate(value.c_str()) > 0) continue;
                    SendDlgItemMessageW(dialog, 1012, CB_SETCURSEL, index, 0);
                    ShowProviderRate(dialog, *options, index);
                    MessageBoxW(dialog, L"请输入大于 0 且不超过 1000 的数值，或留空使用默认值。",
                                L"供应商设置", MB_ICONWARNING);
                    SetFocus(GetDlgItem(dialog, field));
                    return TRUE;
                }
            }
            wchar_t batchMultiplier[64]{};
            GetDlgItemTextW(dialog, 1011, batchMultiplier, _countof(batchMultiplier));
            options->selectedMultiplier = ParseRate(batchMultiplier);
            if (*batchMultiplier && options->selectedMultiplier == 0) {
                MessageBoxW(dialog, L"请输入大于 0 且不超过 1000 的批量倍率。",
                            L"倍率设置", MB_ICONWARNING);
                return TRUE;
            }
            wchar_t multiplier[64]{};
            GetDlgItemTextW(dialog, 1010, multiplier, _countof(multiplier));
            wchar_t* multiplierEnd{};
            options->defaultMultiplier = wcstod(multiplier, &multiplierEnd);
            if (*multiplier && (multiplierEnd == multiplier || *multiplierEnd || !std::isfinite(options->defaultMultiplier) || options->defaultMultiplier <= 0 || options->defaultMultiplier > 1000)) {
                MessageBoxW(dialog, L"请输入大于 0 且不超过 1000 的默认倍率。", L"倍率设置", MB_ICONWARNING);
                return TRUE;
            }
            if (!*multiplier) options->defaultMultiplier = 1.0;
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
            if (period < 0 || period >= periodCount)
                return TRUE;
            options->period = static_cast<int>(period);
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
            if (options->selectedMultiplier > 0 && !options->selectedProviders[1].empty() &&
                std::none_of(options->providers.begin(), options->providers.begin() + options->knownProviders,
                    [&](const Provider& provider) {
                        const auto& selection = options->selectedProviders[1];
                        return std::find(selection.begin(), selection.end(), provider) != selection.end();
                    })) {
                MessageBoxW(dialog, L"当前勾选范围内没有可设置倍率的供应商。", L"批量倍率", MB_ICONWARNING);
                return TRUE;
            }
            if (!Instance().SaveOptions(*options)) {
                ShowSaveError(dialog);
                return TRUE;
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

    static std::wstring DisplayProvider(const Provider& provider, const std::vector<Provider>& providers) {
        if (!provider.storedKey.empty()) return provider.label + L"（未匹配）";
        if (std::count_if(providers.begin(), providers.end(), [&](const Provider& candidate) {
            return candidate.label == provider.label;
        }) > 1) return provider.label + L" [" + provider.id + L"]";
        return provider.label;
    }

    static void ActivateItem(bool toggleCurrency) {
        auto& plugin = Instance();
        bool saved;
        {
            const std::lock_guard<std::mutex> lock(plugin.refreshMutex_);
            saved = plugin.ApplyShortcut(toggleCurrency ? plugin.selected_ : (plugin.selected_ + 1) % periodCount,
                                         toggleCurrency ? !plugin.showCny_ : plugin.showCny_);
        }
        if (!saved) ShowSaveError(nullptr);
    }

    struct CostDetail {
        Provider provider;
        double rawUsd{};
        double multiplier{};
        double rate{};
        const wchar_t* rateSource{};
        double cny{};
    };

    struct Details {
        std::wstring summary;
        std::vector<CostDetail> rows;
    };

    void ShowDetails(HWND parent) {
        Details data;
        {
            const std::lock_guard<std::mutex> lock(refreshMutex_);
            Refresh(false);
            data = details_;
        }
        if (InitControls())
            DialogBoxParamW(Module(), MAKEINTRESOURCEW(102), parent, DetailsProcedure,
                            reinterpret_cast<LPARAM>(&data));
    }

    static INT_PTR CALLBACK DetailsProcedure(HWND dialog, UINT message, WPARAM command, LPARAM data) {
        if (message == WM_COMMAND && (LOWORD(command) == IDOK || LOWORD(command) == IDCANCEL)) {
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (message != WM_INITDIALOG) return FALSE;
        const auto& details = *reinterpret_cast<const Details*>(data);
        SetDlgItemTextW(dialog, 1020, details.summary.c_str());
        const auto list = GetDlgItem(dialog, 1021);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* headings[] = {L"供应商", L"原始 USD", L"倍率", L"元/USD", L"汇率来源", L"折算 CNY"};
        const int widths[] = {29, 14, 9, 13, 17, 18};
        RECT area{};
        GetClientRect(list, &area);
        const int width = area.right - GetSystemMetrics(SM_CXVSCROLL) - 2;
        for (int index = 0; index < _countof(headings); ++index) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            column.pszText = const_cast<wchar_t*>(headings[index]);
            column.cx = width * widths[index] / 100;
            column.fmt = index == 0 || index == 4 ? LVCFMT_LEFT : LVCFMT_RIGHT;
            SendMessageW(list, LVM_INSERTCOLUMNW, index, reinterpret_cast<LPARAM>(&column));
        }
        const auto detailNumber = [](double number) {
            wchar_t text[64];
            swprintf_s(text, L"%.10g", number);
            return std::wstring(text);
        };
        std::vector<Provider> providers;
        for (const auto& row : details.rows) providers.push_back(row.provider);
        for (int index = 0; index < static_cast<int>(details.rows.size()); ++index) {
            const auto& row = details.rows[index];
            std::array<std::wstring, 6> cells = {
                DisplayProvider(row.provider, providers), detailNumber(row.rawUsd), detailNumber(row.multiplier),
                row.rate > 0 ? detailNumber(row.rate) : L"—", row.rateSource,
                row.rate > 0 || row.rawUsd == 0 ? detailNumber(row.cny) : L"—"
            };
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = cells[0].data();
            const auto inserted = SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            if (inserted < 0) continue;
            for (int column = 1; column < static_cast<int>(cells.size()); ++column) {
                item.iSubItem = column;
                item.pszText = cells[column].data();
                SendMessageW(list, LVM_SETITEMTEXTW, inserted, reinterpret_cast<LPARAM>(&item));
            }
        }
        return TRUE;
    }

    void Refresh(bool force) {
        const auto now = GetTickCount64();
        if (!force && nextRefresh_ && now < nextRefresh_)
            return;
        nextRefresh_ = now + 2000;

        const auto& period = periods[selected_];
        Totals usage;
        bool available = false;
        bool missingRate = false;
        double rawCost = 0;
        double adjustedCost = 0;
        double convertedCost = 0;
        CurrencySettings currency;
        details_ = {std::wstring(period.label) + L" · 用量暂不可读", {}};
        try {
            currency = ReadCurrencySettings();
            const auto path = SettingsPath();
            if (!providerFiltersInitialized_) MigrateProviders(ReadProviders(), path);
            const double defaultRate = billingRate_ > 0 ? billingRate_ : currency.cnyPerUsd;
            std::vector<ProviderUsage> rows;
            available = ReadUsage(period, rows);
            if (available) {
                const auto includes = [](const std::vector<Provider>& selection, const Provider& provider) {
                    return selection.empty() || std::find(selection.begin(), selection.end(), provider) != selection.end();
                };
                for (const auto& row : rows) {
                    const auto& part = row.totals;
                    if (includes(tokenProviders_, row.provider)) {
                        usage.requests += part.requests;
                        usage.input += part.input;
                        usage.output += part.output;
                        usage.cacheRead += part.cacheRead;
                        usage.cacheWrite += part.cacheWrite;
                    }
                    if (!includes(costProviders_, row.provider)) continue;
                    const double overrideMultiplier = ReadProviderNumber(path, L"ProviderMultipliers", row.provider);
                    const double multiplier = overrideMultiplier > 0 ? overrideMultiplier : defaultMultiplier_;
                    const double overrideRate = ReadProviderNumber(path, L"ProviderRates", row.provider);
                    const double rate = overrideRate > 0 ? overrideRate : defaultRate;
                    const wchar_t* source = overrideRate > 0 ? L"供应商" : billingRate_ > 0 ? L"插件默认"
                        : rate > 0 ? L"CC Switch" : L"缺汇率";
                    const double usd = part.cost * multiplier;
                    const double cny = usd * rate;
                    rawCost += part.cost;
                    adjustedCost += usd;
                    convertedCost += cny;
                    if (usd != 0 && rate == 0) missingRate = true;
                    details_.rows.push_back({row.provider, part.cost, multiplier, rate, source, cny});
                }
            }
        } catch (...) {
            available = false;
            details_.rows.clear();
        }
        if (!currencyInitialized_) {
            showCny_ = currency.preferCny;
            currencyInitialized_ = true;
        }
        const auto unresolved = [](const std::vector<Provider>& selection) {
            return std::any_of(selection.begin(), selection.end(), [](const Provider& provider) {
                return !provider.storedKey.empty() && provider.storedKey.front() == L'?';
            });
        };
        const bool tokensAvailable = available && !unresolved(tokenProviders_);
        const bool costAvailable = available && !unresolved(costProviders_);
        const std::lock_guard<std::mutex> lock(tooltipMutex_);
        if (!available) {
            tokens_.SetValue(period, L"--");
            cost_.SetValue(period, L"--");
            wcscpy_s(tooltip_, L"CC Switch 用量暂不可读，15 秒后重试。");
            return;
        }
        nextRefresh_ = now + 15000;

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
        tokens_.SetValue(period, tokensAvailable ? value : L"--");

        wchar_t converted[128]{};

        if (!showCny_) {
            swprintf_s(value, L"$%.4f", adjustedCost);
        } else if (!missingRate) {
            swprintf_s(value, L"¥%.4f", convertedCost);
            swprintf_s(converted, L"按供应商汇率折算 ≈ %.6f CNY\n", convertedCost);
        } else {
            wcscpy_s(value, L"¥--");
            wcscpy_s(converted, L"部分费用缺少可用的人民币汇率。\n");
        }
        cost_.SetValue(period, costAvailable ? value : L"--");
        const auto describeScope = [](const std::vector<Provider>& providers) -> std::wstring {
            if (providers.empty()) return L"全部供应商";
            if (providers.size() == 1) return providers.front().label;
            return std::to_wstring(providers.size()) + L" 个供应商";
        };
        wchar_t tokenDetails[192] = L"Token 数据暂不可读";
        if (tokensAvailable)
            swprintf_s(tokenDetails, L"请求 %lld 次 · 输入 %lld · 输出 %lld Token\n缓存读取 %lld · 缓存写入 %lld Token",
                       usage.requests, usage.input, usage.output, usage.cacheRead, usage.cacheWrite);
        wchar_t costDetails[160] = L"费用供应商未匹配，请在选项中重新选择";
        if (costAvailable)
            swprintf_s(costDetails, L"原始 %.6g USD · 倍率调整后 %.6g USD", rawCost, adjustedCost);
        if (!tokensAvailable) wcscpy_s(tokenDetails, L"Token 供应商未匹配，请在选项中重新选择");
        wchar_t summary[256];
        const auto cnyTotal = missingRate ? std::wstring(L"—（部分费用缺汇率）")
                                         : NumberText(convertedCost);
        if (costAvailable) {
            swprintf_s(summary, L"%ls · 原始 %.6g USD · 倍率调整后 %.6g USD · 折算 %ls CNY",
                       period.label, rawCost, adjustedCost, cnyTotal.empty() ? L"0" : cnyTotal.c_str());
        } else {
            swprintf_s(summary, L"%ls · %ls", period.label, costDetails);
            details_.rows.clear();
        }
        details_.summary = summary;
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
        case TMI_VERSION: return L"0.1.17";
        default: return L"";
        }
    }

private:
    UsageItem tokens_{L"CC Switch Token", L"CCSwitchUsage.Tokens", L"Token ", L"近24小时 999.99M", 0, ActivateItem};
    UsageItem cost_{L"CC Switch 费用", L"CCSwitchUsage.Cost", L"费用 ", L"近24小时 ¥9999.9999", 1, ActivateItem};
    std::mutex refreshMutex_;
    std::mutex tooltipMutex_;
    std::atomic_bool refreshRunning_{};
    std::vector<Provider> tokenProviders_;
    std::vector<Provider> costProviders_;
    int selected_{};
    bool showCny_{};
    double billingRate_{};
    double defaultMultiplier_ = 1.0;
    Details details_;
    bool currencyInitialized_{};
    bool providerFiltersInitialized_{};
    ULONGLONG nextRefresh_{};
    wchar_t tooltip_[768] = L"正在读取 CC Switch 用量。";
};

} // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &UsagePlugin::Instance();
}
