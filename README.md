# DriveMonitor

[Русский](#русский) · [English](#english)

---

## Русский

Русский и английский просмотрщик S.M.A.R.T. для Windows. Язык: меню **Язык** (запоминается).

**Текущий выпуск:** [1.7.6](https://github.com/chuikoff/DriveMonitor/releases/tag/1.7.6) (сборка 6) — скачать `DriveMonitor.exe`, запустить от администратора.

Windows 10 / 11. Один снимок SMART (старт, hotplug, «Перечитать»), без трея, графика и теста поверхности. Интерфейс следует DPI монитора; масштаб 100–200% в меню «Вид» (Ctrl+±, Ctrl+0, Ctrl+колёсико).

### Что внутри

Читает сырые SMART-данные через `DeviceIoControl` и показывает таблицу: ID / Параметр / Значение / Худший / Порог / RAW / Статус.

| Шина | Как читается |
|------|----------------|
| SATA | `IOCTL_ATA_PASS_THROUGH_DIRECT` |
| USB-бокс (Realtek, JMicron, ASMedia) | SAT сначала (SATA за мостом), затем один vendor-passthrough (NVMe). Нативного NVMe IOCTL на USB нет |
| Внутренний NVMe | `IOCTL_STORAGE_QUERY_PROPERTY` на `stornvme`. Intel RST/VMD — только `IntelNvm`. **Не** `IOCTL_STORAGE_PROTOCOL_COMMAND` и не ATA/SCSI passthrough на RAID: это давало BSOD 0x139 |

Имя USB-переходника берётся из VID/PID (например Realtek `0BDA:9201`), не из модели диска.

**Диск** — SMART RETURN STATUS / NVMe Critical Warning. **Оценка** — ATA-3: prefail Value≤порог сейчас, In the past, usage на пороге, RAW носителя (05/197/198/187), self-test. Механика, температура и CRC — только в своих осях и лекции, **не** в overall. Ресурс SSD тоже не здоровье: 11–20% остатка — ось «РИСК», overall не трогает; ≤5% может поднять оценку не выше **ВНИМАНИЕ**. Шкала **ХОРОШО → РИСК → ТРЕБУЕТ ВНИМАНИЯ → ПЛОХО → КРИТИЧЕСКОЕ**.

Не формула Health%. Неизвестный vendor RAW не оценивается как поломка (например ECC 195 у не-Seagate и 187 Hitachi/HGST с packing в старших байтах — «не оценивается», не ОК). ATA 190/194 — RAW[0] датчика, не инвертированный Value 100−T. Наработка — контекст, не штраф. C0 на SSD и небезопасные выключения NVMe — журнал питания, не поломка. Для NVMe пороги температуры — WCTEMP/CCTEMP диска, иначе ~50/60/70 °C.

### Требования к сборке

- Windows 10 / 11, x64
- **MinGW-w64** GCC 11 или новее (или LLVM-MinGW). TDM-GCC тоже подходит.
- Готовый `DriveMonitor.exe` нужно запускать **от имени администратора**: SMART читается через `\\.\PhysicalDriveN`.

MSYS2 / MinGW-w64:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make
```

### Сборка

```bash
make
```

На Windows без `make` в PATH: `mingw32-make`. Результат: `bin/DriveMonitor.exe` (статический x64).

```bash
make clean
```

### Лицензия

MIT. Copyright: Ari Sohandri Putra / ARImetic Inc. Изменения: [chuikoff](https://github.com/chuikoff).

Поддержать: https://boosty.to/chuikoff

---

## English

A one-shot Windows S.M.A.R.T. viewer with Russian and English UI. Language: **Language** menu (persisted).

**Current release:** [1.7.6](https://github.com/chuikoff/DriveMonitor/releases/tag/1.7.6) (build 6) — download `DriveMonitor.exe` and run it as Administrator.

Windows 10 / 11. One SMART snapshot (startup, hotplug, Reread). No tray, no live polling, no Health% formula. The UI follows monitor DPI; zoom 100–200% is under **View** (Ctrl+±, Ctrl+0, Ctrl+wheel).

### What it does

It reads raw SMART via `DeviceIoControl` and shows: ID / Attribute / Value / Worst / Thresh / RAW / Status.

| Bus | How it is read |
|------|----------------|
| SATA | `IOCTL_ATA_PASS_THROUGH_DIRECT` |
| USB enclosure (Realtek, JMicron, ASMedia) | SAT first (SATA behind the bridge), then one vendor NVMe passthrough. There is no native NVMe IOCTL on USB |
| Internal NVMe | `IOCTL_STORAGE_QUERY_PROPERTY` on `stornvme`. Intel RST/VMD uses `IntelNvm` only. **Not** `IOCTL_STORAGE_PROTOCOL_COMMAND`, and no ATA/SCSI passthrough on RAID — that bugchecked 0x139 |

The USB adapter name comes from VID/PID (for example Realtek `0BDA:9201`), not from the drive model.

**Drive** is SMART RETURN STATUS / NVMe Critical Warning. **Assessment** is ATA-3: prefail Value≤threshold now, In the past, usage at threshold, media RAW (05/197/198/187), self-test. Mechanics, temperature and CRC stay on their own axes and in the lecture — **not** in overall. SSD remaining life is wear, not health: 11–20% remaining is the Wear axis (WATCH) and does not move overall; ≤5% can raise overall no higher than **NEEDS ATTENTION**. Scale: **GOOD → WATCH → NEEDS ATTENTION → BAD → CRITICAL**.

No Health% formula. Unknown vendor RAW is not scored as failure (for example ECC 195 on non-Seagate, and Hitachi/HGST 187 packing — **Not scored**, not OK). ATA 190/194 use sensor RAW[0], not inverted Value 100−T. Power-on hours are context, not a penalty. SSD C0 and NVMe unsafe shutdowns are a **power log**, not a fault. NVMe temperature limits come from the drive WCTEMP/CCTEMP, otherwise ~50/60/70 °C.

### Build requirements

- Windows 10 / 11, x64
- **MinGW-w64** GCC 11 or newer (or LLVM-MinGW). TDM-GCC also works.
- Run `DriveMonitor.exe` **as Administrator**: SMART is read through `\\.\PhysicalDriveN`.

MSYS2 / MinGW-w64:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make
```

### Build

```bash
make
```

On Windows without `make` in PATH: `mingw32-make`. Output: `bin/DriveMonitor.exe` (static x64).

```bash
make clean
```

### License

MIT. Copyright: Ari Sohandri Putra / ARImetic Inc. Changes: [chuikoff](https://github.com/chuikoff).

Support: https://boosty.to/chuikoff
