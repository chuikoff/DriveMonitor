/* DriveMonitor - UI language. MIT: see LICENSE. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "lang.h"

static UI_LANG g_lang = UI_LANG_RU;

static const char* const kRu[STR__COUNT] = {
    "Файл", "Вид", "Язык", "Справка",
    "Сохранить отчёт", "Извлечь USB-диск", "Сохранить снимок\tCtrl+S", "Выход",
    "Увеличить\tCtrl++", "Уменьшить\tCtrl+-",
    "Поддержать...", "О программе",
    "ДИСКИ", "ДИСК / ОЦЕНКА", "Перечитать", "Отчёт", "Извлечь",
    "Носитель", "Интерфейс", "Температура", "Ресурс", "Механика",
    "Модель", "Бренд", "Контроллер", "Серийный №", "Прошивка", "Объём",
    "Наработка", "Переходник", "Протокол",
    "Параметр", "Значение", "Худший", "Порог", "Статус",
    "Диски не найдены", "Нет данных", "Диск", "Оценка",
    "Диск: %s", "Оценка: %s",
    "ХОРОШО", "РИСК", "ТРЕБУЕТ ВНИМАНИЯ", "ВНИМАНИЕ", "ПЛОХО", "КРИТИЧЕСКОЕ", "КРИТ.", "НЕИЗВЕСТНО",
    "Норма", "Повышена", "Высокая", "Критическая",
    "норма", "повышена", "высокая", "критическая",
    "НОРМА", "ПОВЫШЕНА", "КРИТИЧЕСКАЯ",
    "нет статуса моста", "нет SMART", "Неизвестно",
    "нет данных", "год", "года", "лет", "день", "дня", "дней", "час", "часа", "часов",
    "ОК", "ПЛОХО", "СБОЙ", "Внимание", "Жарко", "Не оценивается", "было",
    "контекст", "журнал питания", "Риск", "--",
    "Почему такая оценка", "Нет выбранного диска", "Недостаточно памяти для отчёта.",
    "Сохранить отчёт", "Markdown (*.md)\0*.md\0Все файлы (*.*)\0*.*\0",
    "Не удалось сохранить отчёт.", "Не удалось записать отчёт.", "Отчёт сохранён\n\n%s",
    "Этот диск не USB.",
    "Отключить «%s» и извлечь из USB?",
    "Не удалось извлечь диск (код %lu).",
    "Диск отключён. Шнур можно вынуть.",
    "Мониторинг состояния дисков и S.M.A.R.T. на низком уровне.",
    "Автор: chuikoff — MIT License",
    "Свободное ПО с открытым исходным кодом",
    "Поддержать",
    "DriveMonitor - Screenshot Saved",
    "DriveMonitor - Screenshot Error",
    "Почему такая оценка",
    "Атрибут %d (vendor-specific)",
    "# DriveMonitor — отчёт по диску\r\n\r\n",
    "## Диск\r\n\r\n",
    "Поле", "Значение",
    "## Экспертный отчёт\r\n\r\n",
    "## SMART / NVMe\r\n\r\n",
    "—"
};

static const char* const kEn[STR__COUNT] = {
    "File", "View", "Language", "Help",
    "Save report", "Eject USB drive", "Save screenshot\tCtrl+S", "Exit",
    "Zoom in\tCtrl++", "Zoom out\tCtrl+-",
    "Support...", "About",
    "DRIVES", "DRIVE / ASSESSMENT", "Reread", "Report", "Eject",
    "Media", "Interface", "Temperature", "Wear", "Mechanics",
    "Model", "Brand", "Controller", "Serial", "Firmware", "Capacity",
    "Power-on", "Bridge", "Protocol",
    "Attribute", "Value", "Worst", "Thresh", "Status",
    "No drives found", "No data", "Drive", "Assessment",
    "Drive: %s", "Assessment: %s",
    "GOOD", "WATCH", "NEEDS ATTENTION", "CAUTION", "BAD", "CRITICAL", "CRIT.", "UNKNOWN",
    "Normal", "Elevated", "High", "Critical",
    "normal", "elevated", "high", "critical",
    "NORMAL", "ELEVATED", "CRITICAL",
    "no bridge status", "no SMART", "Unknown",
    "no data", "year", "years", "years", "day", "days", "days", "hour", "hours", "hours",
    "OK", "BAD", "FAIL", "Caution", "Hot", "Not scored", "past",
    "context", "power log", "Watch", "--",
    "Why this assessment", "No drive selected", "Not enough memory for the report.",
    "Save report", "Markdown (*.md)\0*.md\0All files (*.*)\0*.*\0",
    "Could not save the report.", "Could not write the report.", "Report saved\n\n%s",
    "This is not a USB drive.",
    "Disconnect \"%s\" and eject from USB?",
    "Could not eject the drive (code %lu).",
    "Drive disconnected. You can unplug the cable.",
    "Low-level disk health and S.M.A.R.T. viewer.",
    "Author: chuikoff — MIT License",
    "Free and open-source software",
    "Support",
    "DriveMonitor - Screenshot Saved",
    "DriveMonitor - Screenshot Error",
    "Why this assessment",
    "Attribute %d (vendor-specific)",
    "# DriveMonitor — drive report\r\n\r\n",
    "## Drive\r\n\r\n",
    "Field", "Value",
    "## Expert notes\r\n\r\n",
    "## SMART / NVMe\r\n\r\n",
    "—"
};

static UI_LANG LangFromWindows(void)
{
    LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_RUSSIAN)
        return UI_LANG_RU;
    return UI_LANG_EN;
}

void UiLangInit(void)
{
    HKEY k;
    DWORD v = 0xFFFFFFFFu, sz = sizeof(v), t = 0;
    g_lang = LangFromWindows();
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "UiLang", NULL, &t, (LPBYTE)&v, &sz) == ERROR_SUCCESS &&
            t == REG_DWORD && (v == 0 || v == 1))
            g_lang = (UI_LANG)v;
        RegCloseKey(k);
    }
}

UI_LANG UiLang(void)
{
    return g_lang;
}

int UiLangIsEn(void)
{
    return g_lang == UI_LANG_EN;
}

void UiSetLang(UI_LANG lang)
{
    HKEY k;
    DWORD d;
    if (lang != UI_LANG_EN)
        lang = UI_LANG_RU;
    g_lang = lang;
    d = (DWORD)lang;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                        0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, "UiLang", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
        RegCloseKey(k);
    }
}

const char* Tr(int id)
{
    if (id < 0 || id >= STR__COUNT) return "";
    return g_lang == UI_LANG_EN ? kEn[id] : kRu[id];
}

const char* TrStatus(const char* s)
{
    if (!s || !s[0] || g_lang != UI_LANG_EN)
        return s ? s : "";
    if (strcmp(s, "ОК") == 0) return kEn[STR_ST_OK];
    if (strcmp(s, "ПЛОХО") == 0) return kEn[STR_ST_BAD];
    if (strcmp(s, "СБОЙ") == 0) return kEn[STR_ST_FAIL];
    if (strcmp(s, "Внимание") == 0) return kEn[STR_ST_WARN];
    if (strcmp(s, "Жарко") == 0) return kEn[STR_ST_HOT];
    if (strcmp(s, "Не оценивается") == 0) return kEn[STR_ST_SKIP];
    if (strcmp(s, "было") == 0) return kEn[STR_ST_PAST];
    if (strcmp(s, "контекст") == 0) return kEn[STR_ST_INFO];
    if (strcmp(s, "журнал питания") == 0) return kEn[STR_ST_POWERLOG];
    if (strcmp(s, "Риск") == 0) return kEn[STR_ST_RISK];
    if (strcmp(s, "--") == 0) return kEn[STR_ST_DIM];
    if (strcmp(s, "INFO") == 0) return kEn[STR_ST_INFO];
    return s;
}
