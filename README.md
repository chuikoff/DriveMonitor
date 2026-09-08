# DriveMonitor

Русский просмотрщик S.M.A.R.T. для Windows. Форк [HDDHealth Monitor](https://github.com/arisohandriputra/HDD-Health-Monitor) (MIT).

English: a one-shot Windows SMART viewer. No tray, no live polling, no Health% formula. Russian UI.

**Текущий выпуск:** [1.6](https://github.com/chuikoff/DriveMonitor/releases/tag/1.6) — скачать `DriveMonitor.exe`, запустить от администратора.

Windows 10 / 11. Не CrystalDiskInfo и не Victoria: один снимок SMART (старт, hotplug, «Перечитать»), без трея, графика и теста поверхности.

## Что внутри

Читает сырые SMART-данные через `DeviceIoControl` и показывает таблицу Victoria: ID / Параметр / Значение / Худший / Порог / RAW / Статус.

| Шина | Как читается |
|------|----------------|
| SATA | `IOCTL_ATA_PASS_THROUGH_DIRECT` |
| USB-бокс (Realtek, JMicron, ASMedia) | SAT сначала (SATA за мостом), затем один vendor-passthrough (NVMe). Нативного NVMe IOCTL на USB нет |
| Внутренний NVMe | SCSI miniport / `IOCTL_STORAGE_QUERY_PROPERTY`. **Не** `IOCTL_STORAGE_PROTOCOL_COMMAND` — на `nvme.sys` это давало BSOD |

Имя USB-переходника берётся из VID/PID (например Realtek `0BDA:9201`), не из модели диска.

**Диск** — SMART RETURN STATUS / NVMe Critical Warning. **Оценка** — ATA-3 как в smartctl (без GPL-кода): prefail Value≤порог сейчас, In the past, usage на пороге, RAW носителя (05/197/198/187), self-test. Механика, температура и CRC — в таблице и лекции, не в overall. Шкала **ХОРОШО → РИСК → ТРЕБУЕТ ВНИМАНИЯ → ПЛОХО → КРИТИЧЕСКОЕ**.

Не формула Health%. Неизвестный vendor RAW не оценивается как поломка. Наработка — контекст, не штраф. Для NVMe пороги температуры — WCTEMP/CCTEMP диска, иначе ~50/60/70 °C. USB-диск можно извлечь кнопкой «Извлечь». В текстовом отчёте есть версия программы.

Имена и RAW атрибутов зависят от производителя (Seagate, WD, Samsung, Kingston/Phison, ADATA, Toshiba, Micron, Hynix, Intel).

## Сборка

Нужен **MinGW-w64** или **TDM-GCC**.

```bash
make
```

Результат: `bin/DriveMonitor.exe` (статический x64, без DLL рантайма).

```bash
make clean
```

## Лицензия

MIT. Оригинальный copyright: Ari Sohandri Putra / ARImetic Inc. Изменения форка: [chuikoff](https://github.com/chuikoff).

Поддержать форк: https://boosty.to/chuikoff  
Апстрим: https://github.com/sponsors/arisohandriputra/
