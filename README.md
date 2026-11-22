# Sensors for the Omarchy bar

An [Omarchy](https://omarchy.org/) shell plugin that shows how hot your
machine runs. A thermometer sits in the bar with the processor temperature
next to it; its column rises with the temperature and turns the theme's
urgent colour when something overheats. Click it for a popup with every
temperature sensor and fan.

[Русская версия ниже](#по-русски)

## What it shows

| Group | Sensors (hwmon driver) |
|---|---|
| Processor | `k10temp`, `zenpower` (AMD), `coretemp` (Intel), `cpu_thermal` (ARM) |
| Graphics | `amdgpu`, `radeon`, `nouveau`, `i915`, `xe` |
| Memory | `spd5118` (DDR5 modules), `jc42` (DDR4 modules with a sensor) |
| Drive | `nvme`, `drivetemp` (SATA, once the module is loaded) |
| System | ACPI thermal zones, laptop embedded controllers (`thinkpad`, `dell_smm`, ...), Super I/O chips |
| Wi-Fi, network | `iwlwifi`, MediaTek `mt7xxx`, Atheros, Realtek, ... |
| Fans | every `fan*_input` the kernel exposes |

Each reading has a small gauge. A sensor counts as too hot at the limit its
driver reports (drives and memory), or at the warning temperature you set
(processor and graphics, 90 °C by default). Hot readings and the bar icon
turn the theme's urgent colour, and you get one notification each time a
sensor crosses its limit. It counts as normal again 3 °C below.

Memory temperature appears when the kernel can read it. Soldered memory has
no sensor, and many laptops keep the SMBus of their DDR5 modules to the
firmware; the popup then says so instead of showing an empty group.

Everything is read locally from `/sys/class/hwmon`. The plugin makes no
network requests and needs no root. Its helper is a single C++17 file using
only the standard library; on first start `run.sh` compiles it with the
system's `c++` (part of `base-devel`, which every Omarchy install has) into
`~/.cache/omarchy-sensors-monitor/`, and rebuilds it after updates. The build
takes a few seconds, once.

## Install

```bash
omarchy plugin add https://github.com/nydevel/omarchy-sensors-monitor.git --enable
```

or manually:

```bash
git clone https://github.com/nydevel/omarchy-sensors-monitor.git ~/.config/omarchy/plugins/sensors-monitor
omarchy-shell shell rescanPlugins
omarchy plugin enable sensors.monitor
```

Move it with `omarchy bar move sensors.monitor --section right --index 0`.

## Settings

Set through the bar's widget settings, or `omarchy bar set sensors.monitor <key> <value>`:

| Key | Default | Meaning |
|---|---|---|
| `showTemperature` | `true` | show the processor temperature next to the thermometer |
| `warnAt` | `90` | °C at which the processor and graphics count as too hot |
| `notifyHot` | `true` | notify when a sensor reaches its limit |
| `refreshSeconds` | `2` | how often sensors are read |

## Limits

- Only what the kernel exposes through hwmon is shown. The proprietary
  NVIDIA driver does not publish its temperature there.
- Memory temperature needs DDR5 modules (or DDR4 with a sensor), the
  `spd5118` / `jc42` driver bound to them, and firmware that lets the OS
  reach them. On some laptops (e.g. ThinkPad P14s Gen 6 AMD) the modules are
  listed but every SPD register reads back as zero, so there is nothing to show.
- SATA drives appear after `sudo modprobe drivetemp`.

## Development

```bash
sh run.sh --once                   # build if needed, print one snapshot
sh run.sh                          # live JSON lines
sh run.sh --demo                   # made-up sensors that heat up and cool down, for UI work
sh run.sh --self-test              # built-in unit checks
tests/test_helper.sh               # helper against a fake sysfs tree and real sensors (needs jq)
node tests/test_model.mjs          # popup logic
omarchy plugin validate .
```

`sensors.cpp` prints one JSON object per line
(`{"v":1,"cpu":…,"hot":…,"groups":[…],"fans":[…],"alerts":[…]}`);
`Panel.qml` renders it, with all parsing and validation in `Model.js`.
Translations live in `i18n/` (English and Russian so far; pull requests welcome).

## License

MIT

---

## По-русски

Плагин для панели [Omarchy](https://omarchy.org/), который показывает
температуру компьютера. В панели стоит градусник, рядом температура
процессора. Столбик в градуснике поднимается вместе с температурой, а при
перегреве значок окрашивается в цвет тревоги из темы. По клику открывается
окно со всеми датчиками: процессор, видеокарта, память, накопители, Wi-Fi,
материнская плата и скорость вентиляторов.

Порог перегрева для процессора и видеокарты задаётся в настройках (по
умолчанию 90 °C). Для накопителей и памяти берутся пределы, которые сообщает
драйвер. Когда датчик доходит до предела, приходит уведомление.

Температура памяти видна, если ядро может её прочитать. У распаянной памяти
датчика нет, а многие ноутбуки закрывают доступ к датчикам модулей DDR5. В
этом случае окно прямо об этом пишет.

Плагин ничего не отправляет в сеть и не требует прав root. Фоновая часть
написана на C++ и при первом запуске сама собирается системным компилятором
(он входит в `base-devel`, который есть в любой установке Omarchy). Сборка
занимает несколько секунд и нужна только один раз.

Установка:

```bash
omarchy plugin add https://github.com/nydevel/omarchy-sensors-monitor.git --enable
```
