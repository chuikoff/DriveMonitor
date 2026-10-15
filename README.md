# DriveMonitor

[Русский](#русский) · [English](#english)

---

## Русский

Русский и английский просмотрщик S.M.A.R.T. для Windows. Язык: меню **Язык** (запоминается).

**Текущий выпуск:** [1.7.23](https://github.com/chuikoff/DriveMonitor/releases/tag/1.7.23) (сборка 23) — скачать `DriveMonitor.exe`, запустить от администратора.

**Бета:** [3.0-beta](https://github.com/chuikoff/DriveMonitor/releases/tag/3.0-beta) (GPL-2.0-or-later). Исходники этого тега: [дерево 3.0-beta](https://github.com/chuikoff/DriveMonitor/tree/3.0-beta).

Windows 10 / 11. Снимок SMART на старте, при hotplug и по «Перечитать». «Вид → Тема» — как в системе, светлая или тёмная. «Вид → Мониторинг» (Ctrl+M) раз в минуту перечитывает диски; по умолчанию выключен. Пока мониторинг включён, в трее по иконке на диск с температурой, а кнопка «Свернуть» убирает окно в трей и чтение продолжается. Без мониторинга иконок в трее нет. Закрытие окна завершает программу. Без графика и теста поверхности. Интерфейс следует DPI монитора; масштаб 100–200% в меню «Вид» (Ctrl+±, Ctrl+0, Ctrl+колёсико).

### Что внутри

Читает сырые SMART-данные через `DeviceIoControl` и показывает таблицу: ID / Параметр / Значение / Худший / Порог / RAW / Расшифровка / Статус. RAW — 12 шестнадцатеричных цифр, чтение в «Расшифровке».

| Шина | Как читается |
|------|----------------|
| SATA | `IOCTL_ATA_PASS_THROUGH_DIRECT` |
| USB-бокс (Realtek, JMicron, ASMedia) | SAT сначала (SATA за мостом), затем один vendor-passthrough (NVMe). Нативного NVMe IOCTL на USB нет |
| Внутренний NVMe | `IOCTL_STORAGE_QUERY_PROPERTY` на `stornvme`. Intel RST/VMD — только `IntelNvm`. **Не** `IOCTL_STORAGE_PROTOCOL_COMMAND` и не ATA/SCSI passthrough на RAID: это давало BSOD 0x139 |

Имя USB-переходника берётся из VID/PID (например Realtek `0BDA:9201`), не из модели диска.

**Диск** — SMART RETURN STATUS / NVMe Critical Warning. **Оценка** начинается с носителя: prefail, self-test и RAW носителя. Подтверждённый CRC (атрибут 199, с единицы), таймауты команд, повторы раскрутки, перегрев, механика HDD и почти исчерпанный ресурс SSD поднимают чистый носитель до **ВНИМАНИЕ** и не дальше. «Риск» на боковой оси итог не меняет: 11–20% остатка ресурса остаются на оси износа. Подсказка только по имени атрибута из drivedb тоже остаётся «Риск» на своей строке и в итог не входит: SMART Modular, атрибут 1 при RAW 5, строка «Риск», итог «Хорошо». Подтверждённый «Риск» носителя итог берёт как есть, в том числе сквозные ошибки 184 на HDD со счёта 1. Строка CRC со 100 — «ПЛОХО», итог при чистом носителе всё равно **ВНИМАНИЕ**, и лекция говорит, что носитель в норме. Так же в 1.7.23. Шкала **ХОРОШО → РИСК → ТРЕБУЕТ ВНИМАНИЯ → ПЛОХО → КРИТИЧЕСКОЕ**.

Не формула Health%. Неизвестный vendor RAW не оценивается как поломка (например ECC 195 у не-Seagate и 187 Hitachi/HGST с packing в старших байтах — «не оценивается», не ОК). У Crucial и Micron атрибут 187 — неисправимые ошибки: «Внимание» с единицы. Общий 187 на SSD не оценивается. На HDD и у Samsung 187 по-прежнему «ПЛОХО» с единицы. ATA 190/194 — RAW[0] датчика, не инвертированный Value 100−T. Наработка — контекст, не штраф. C0 на SSD и небезопасные выключения NVMe — журнал питания, не поломка. Для NVMe пороги температуры — WCTEMP/CCTEMP диска, иначе ~50/60/70 °C.

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

DriveMonitor — Copyright (C) 2026 [chuikoff](https://github.com/chuikoff). С версии 1.7.27 это дерево распространяется под GPL-2.0-or-later.

Опубликованный GPL-файл — бета [3.0-beta](https://github.com/chuikoff/DriveMonitor/releases/tag/3.0-beta). Исходники этого тега: [дерево](https://github.com/chuikoff/DriveMonitor/tree/3.0-beta), [tar.gz](https://github.com/chuikoff/DriveMonitor/archive/refs/tags/3.0-beta.tar.gz), [zip](https://github.com/chuikoff/DriveMonitor/archive/refs/tags/3.0-beta.zip).

Имена семейств и атрибутов известных моделей — из smartmontools `drivedb.h` (Christian Franke и соавторы, GPL-2.0-or-later), файл `third_party/smartmontools/drivedb.h`.

Поддержать: https://boosty.to/chuikoff

---

## English

A one-shot Windows S.M.A.R.T. viewer with Russian and English UI. Language: **Language** menu (persisted).

**Current release:** [1.7.23](https://github.com/chuikoff/DriveMonitor/releases/tag/1.7.23) (build 23) — download `DriveMonitor.exe` and run it as Administrator.

**Beta:** [3.0-beta](https://github.com/chuikoff/DriveMonitor/releases/tag/3.0-beta) (GPL-2.0-or-later). Source for this tag: [tree 3.0-beta](https://github.com/chuikoff/DriveMonitor/tree/3.0-beta).

Windows 10 / 11. One SMART snapshot at startup, on hotplug, and on Reread. **View → Theme** follows the system, or stays light or dark. **View → Monitor** (Ctrl+M) rereads once a minute; it is off by default. While monitoring is on, the tray shows one icon per disk with its temperature, and Minimize hides the window there while reading continues. With monitoring off there are no tray icons. Closing the window exits. No Health% formula. The UI follows monitor DPI; zoom 100–200% is under **View** (Ctrl+±, Ctrl+0, Ctrl+wheel).

### What it does

It reads raw SMART via `DeviceIoControl` and shows: ID / Attribute / Value / Worst / Thresh / RAW / Decoded / Status. RAW is 12 hex digits; the reading is in Decoded.

| Bus | How it is read |
|------|----------------|
| SATA | `IOCTL_ATA_PASS_THROUGH_DIRECT` |
| USB enclosure (Realtek, JMicron, ASMedia) | SAT first (SATA behind the bridge), then one vendor NVMe passthrough. There is no native NVMe IOCTL on USB |
| Internal NVMe | `IOCTL_STORAGE_QUERY_PROPERTY` on `stornvme`. Intel RST/VMD uses `IntelNvm` only. **Not** `IOCTL_STORAGE_PROTOCOL_COMMAND`, and no ATA/SCSI passthrough on RAID — that bugchecked 0x139 |

The USB adapter name comes from VID/PID (for example Realtek `0BDA:9201`), not from the drive model.

**Drive** is SMART RETURN STATUS / NVMe Critical Warning. **Assessment** starts from the media: prefail, self-test, and media RAW. Confirmed CRC (attribute 199, from 1), command timeouts, spin retries, overheating, HDD mechanics, and nearly exhausted SSD endurance raise a clean medium to **NEEDS ATTENTION** and no further. **WATCH** on a side axis leaves the headline alone: 11–20% life remaining stays on the wear axis. A meaning taken only from a drivedb attribute name also stays **WATCH** on that row and leaves the headline alone: SMART Modular attribute 1 with RAW 5 is **WATCH** on the row and **GOOD** overall. Confirmed media **WATCH** sets the headline, including HDD end-to-end errors (attribute 184) from a count of 1. A CRC row at 100 is **BAD**; with a clean medium the headline is still **NEEDS ATTENTION**, and the lecture says the medium is fine. The 1.7.23 release did the same. Scale: **GOOD → WATCH → NEEDS ATTENTION → BAD → CRITICAL**.

No Health% formula. Unknown vendor RAW is not scored as failure (for example ECC 195 on non-Seagate, and Hitachi/HGST 187 packing — **Not scored**, not OK). On Crucial and Micron, attribute 187 is uncorrectable errors: **NEEDS ATTENTION** from 1. A generic SSD 187 is not scored. HDD 187 and Samsung 187 stay **BAD** from 1. ATA 190/194 use sensor RAW[0], not inverted Value 100−T. Power-on hours are context, not a penalty. SSD C0 and NVMe unsafe shutdowns are a **power log**, not a fault. NVMe temperature limits come from the drive WCTEMP/CCTEMP, otherwise ~50/60/70 °C.

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

DriveMonitor is Copyright (C) 2026 [chuikoff](https://github.com/chuikoff). From version 1.7.27 this tree is distributed under GPL-2.0-or-later.

The published GPL executable is the [3.0-beta](https://github.com/chuikoff/DriveMonitor/releases/tag/3.0-beta) prerelease. Source for this tag: [tree](https://github.com/chuikoff/DriveMonitor/tree/3.0-beta), [tar.gz](https://github.com/chuikoff/DriveMonitor/archive/refs/tags/3.0-beta.tar.gz), [zip](https://github.com/chuikoff/DriveMonitor/archive/refs/tags/3.0-beta.zip).

Family and attribute names for known models come from smartmontools `drivedb.h` (Christian Franke and others, GPL-2.0-or-later), file `third_party/smartmontools/drivedb.h`.

Support: https://boosty.to/chuikoff
