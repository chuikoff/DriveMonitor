# DriveMonitor

Русский просмотрщик S.M.A.R.T. для Windows.

English: a one-shot Windows SMART viewer. No tray, no live polling, no Health% formula. Russian UI.

**Текущий выпуск:** [1.7.0](https://github.com/chuikoff/DriveMonitor/releases/tag/1.7.0) — скачать `DriveMonitor.exe`, запустить от администратора.

Windows 10 / 11. Один снимок SMART (старт, hotplug, «Перечитать»), без трея, графика и теста поверхности. Интерфейс следует DPI монитора; масштаб 100–200% в меню «Вид» (Ctrl+±, Ctrl+0, Ctrl+колёсико).

## Что внутри

Читает сырые SMART-данные через `DeviceIoControl` и показывает таблицу: ID / Параметр / Значение / Худший / Порог / RAW / Статус.

| Шина | Как читается |
|------|----------------|
| SATA | `IOCTL_ATA_PASS_THROUGH_DIRECT` |
| USB-бокс (Realtek, JMicron, ASMedia) | SAT сначала (SATA за мостом), затем один vendor-passthrough (NVMe). Нативного NVMe IOCTL на USB нет |
| Внутренний NVMe | SCSI miniport / `IOCTL_STORAGE_QUERY_PROPERTY`. **Не** `IOCTL_STORAGE_PROTOCOL_COMMAND` — на `nvme.sys` это давало BSOD |

Имя USB-переходника берётся из VID/PID (например Realtek `0BDA:9201`), не из модели диска.

**Диск** — SMART RETURN STATUS / NVMe Critical Warning. **Оценка** — ATA-3: prefail Value≤порог сейчас, In the past, usage на пороге, RAW носителя (05/197/198/187), self-test. Механика, температура и CRC — только в своих осях и лекции, **не** в overall (это не «худший из четырёх каналов»). Ресурс SSD тоже не здоровье: 11–20% остатка — ось «РИСК», overall не трогает; ≤5% может поднять оценку не выше **ВНИМАНИЕ**. Шкала **ХОРОШО → РИСК → ТРЕБУЕТ ВНИМАНИЯ → ПЛОХО → КРИТИЧЕСКОЕ**.

Не формула Health%. Неизвестный vendor RAW не оценивается как поломка (например ECC 195 у не-Seagate и 187 Hitachi/HGST с packing в старших байтах — «не оценивается», не ОК). ATA 190/194 — RAW[0] датчика, не инвертированный Value 100−T. Наработка — контекст, не штраф. C0 на SSD и небезопасные выключения NVMe — журнал питания, не поломка. Для NVMe пороги температуры — WCTEMP/CCTEMP диска, иначе ~50/60/70 °C. Минуты при WCTEMP/CCTEMP и небезопасные выключения — контекст (история троттлинга), не поломка. USB-диск можно извлечь кнопкой «Извлечь». В текстовом отчёте есть версия программы.

Имена и RAW атрибутов зависят от производителя (Seagate, WD, Samsung, Kingston/Phison, ADATA, Toshiba, Micron, Hynix, Intel).

## Требования к сборке

- Windows 10 / 11, x64
- **MinGW-w64** GCC 11 или новее (или LLVM-MinGW). TDM-GCC тоже подходит.
- Готовый `DriveMonitor.exe` нужно запускать **от имени администратора**: SMART читается через `\\.\PhysicalDriveN`, без прав администратора диски не откроются.

MSYS2 / MinGW-w64:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make
```

## Сборка

```bash
make
```

На Windows без `make` в PATH: `mingw32-make`. Результат: `bin/DriveMonitor.exe` (статический x64, без DLL рантайма).

```bash
make clean
```

## Лицензия

MIT. Copyright: Ari Sohandri Putra / ARImetic Inc. Изменения: [chuikoff](https://github.com/chuikoff).

Поддержать: https://boosty.to/chuikoff
