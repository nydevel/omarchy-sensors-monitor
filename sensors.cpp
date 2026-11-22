// Temperature and fan monitor for the Omarchy bar.
//
// Reads the kernel's hwmon sensors (/sys/class/hwmon): processor, graphics,
// memory modules, drives, Wi-Fi, motherboard and fans. Prints one JSON
// snapshot per line on stdout whenever a reading changes. No root, no
// network, no dependencies beyond the C++ standard library. run.sh compiles
// it on first use.
//
// Usage:
//   sensors [--interval SECONDS] [--warn CELSIUS] [--sysfs DIR] [--cpuinfo FILE]
//           [--once] [--demo] [--self-test]

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxChips = 32;
constexpr size_t kMaxReadings = 16;
constexpr size_t kMaxFans = 8;
constexpr size_t kMaxText = 80;
// Plausible temperature range; anything else is a broken or unused sensor.
constexpr double kMinTemp = -30.0;
constexpr double kMaxTemp = 150.0;
// Plausible threshold range (drivers report 65261.8 °C and similar for "none").
constexpr double kMinLimit = 20.0;
constexpr double kMaxLimit = 150.0;
// Fans faster than this are garbage readings.
constexpr long kMaxRpm = 30000;
// A hot sensor must cool this far below its limit before it counts as normal.
constexpr double kHysteresis = 3.0;

// ------------------------------------------------------------------ helpers

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}

std::string clip(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) continue;
    out += static_cast<char>(c);
  }
  if (out.size() <= kMaxText) return out;
  out.resize(kMaxText);
  // Do not cut a UTF-8 sequence in half.
  while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
  if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80)) out.pop_back();
  return out;
}

std::optional<std::string> read_file(const std::string& path, size_t limit = 4096) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string out;
  char buf[512];
  while (out.size() < limit) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return std::nullopt;  // e.g. EIO / ENODATA from a sensor that is off
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  return out;
}

std::optional<long> read_long(const std::string& path) {
  auto text = read_file(path, 64);
  if (!text) return std::nullopt;
  std::string s = trim(*text);
  if (s.empty()) return std::nullopt;
  errno = 0;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || *end != '\0') return std::nullopt;
  return v;
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string json_str(const std::string& s) { return "\"" + json_escape(s) + "\""; }

std::string json_num(double v) {
  if (!std::isfinite(v)) return "null";
  char buf[32];
  snprintf(buf, sizeof buf, "%.1f", v);
  return buf;
}

bool starts_with(const std::string& s, const char* prefix) {
  return s.compare(0, strlen(prefix), prefix) == 0;
}

// "temp12_input" -> 12 for prefix "temp", suffix "_input".
int attr_index(const std::string& file, const char* prefix, const char* suffix) {
  size_t p = strlen(prefix), q = strlen(suffix);
  if (file.size() <= p + q || !starts_with(file, prefix)) return -1;
  if (file.compare(file.size() - q, q, suffix) != 0) return -1;
  std::string digits = file.substr(p, file.size() - p - q);
  if (digits.empty() || digits.size() > 3) return -1;
  for (char c : digits)
    if (c < '0' || c > '9') return -1;
  return std::atoi(digits.c_str());
}

double now_seconds() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// ------------------------------------------------------------------ model

// Raw view of one hwmon chip, as read from sysfs (or made up by --demo).
struct RawTemp {
  int n = 0;
  std::string label;
  double value = NAN;
  double max = NAN;
  double crit = NAN;
};

struct RawFan {
  int n = 0;
  std::string label;
  long rpm = -1;
};

struct Chip {
  std::string id;     // stable: driver name + device
  std::string name;   // hwmon "name"
  std::string model;  // device/model when the driver exposes one (drives)
  std::vector<RawTemp> temps;
  std::vector<RawFan> fans;
};

struct Reading {
  int n = 0;
  std::string label;
  double t = NAN;
  double high = NAN;
  double crit = NAN;
  double limit = NAN;
  bool hot = false;
  std::string key;
};

struct Group {
  std::string id;
  std::string kind;
  std::string chip;
  std::string title;
  std::vector<Reading> temps;
};

struct Fan {
  int n = 0;
  std::string label;
  std::string chip;
  long rpm = 0;
};

struct Alert {
  std::string kind;
  std::string title;
  std::string label;
  double t = NAN;
};

struct Snapshot {
  double cpu = NAN;
  bool hot = false;
  std::vector<Group> groups;
  std::vector<Fan> fans;
  std::vector<Alert> alerts;
};

const char* const kKindOrder[] = {"cpu", "gpu", "memory", "storage", "system", "wifi", "network", "battery", "other"};

int kind_rank(const std::string& kind) {
  for (size_t i = 0; i < sizeof kKindOrder / sizeof kKindOrder[0]; i++)
    if (kind == kKindOrder[i]) return static_cast<int>(i);
  return 99;
}

std::string kind_for(const std::string& name) {
  static const char* const cpu[] = {"k10temp", "coretemp", "zenpower", "k8temp", "via_cputemp", "cpu_thermal", "cpu-thermal"};
  static const char* const gpu[] = {"amdgpu", "radeon", "nouveau", "i915", "xe", "nvidia"};
  static const char* const memory[] = {"spd5118", "jc42"};
  static const char* const storage[] = {"nvme", "drivetemp"};
  static const char* const wifi[] = {"iwlwifi", "mt7", "mt76", "ath9k", "ath10k", "ath11k", "ath12k", "rtw", "brcmfmac"};
  static const char* const network[] = {"r8169", "igc", "e1000e", "atlantic", "bnxt", "tg3", "ixgbe", "mlx"};
  static const char* const battery[] = {"BAT", "hidpp_battery", "ucsi", "AC", "ADP"};
  for (auto p : cpu)
    if (name == p) return "cpu";
  for (auto p : gpu)
    if (name == p) return "gpu";
  for (auto p : memory)
    if (name == p) return "memory";
  for (auto p : storage)
    if (name == p) return "storage";
  for (auto p : wifi)
    if (starts_with(name, p)) return "wifi";
  for (auto p : network)
    if (starts_with(name, p)) return "network";
  for (auto p : battery)
    if (starts_with(name, p)) return "battery";
  // Thermal zones, laptop embedded controllers and Super I/O chips.
  return "system";
}

// "AMD Ryzen AI 9 HX PRO 370 w/ Radeon 890M" -> "AMD Ryzen AI 9 HX PRO 370"
std::string clean_cpu_name(std::string s) {
  auto erase_all = [&](const std::string& what) {
    for (size_t p; (p = s.find(what)) != std::string::npos;) s.erase(p, what.size());
  };
  erase_all("(R)");
  erase_all("(TM)");
  erase_all("(tm)");
  for (const char* cut : {" w/ ", " with Radeon", " CPU @", " @ "}) {
    size_t p = s.find(cut);
    if (p != std::string::npos) s.erase(p);
  }
  // "... 8-Core Processor" / "... Processor"
  for (const char* tail : {" Processor", " processor"}) {
    size_t p = s.rfind(tail);
    if (p != std::string::npos && p + strlen(tail) == s.size()) {
      s.erase(p);
      size_t sp = s.rfind(' ');
      if (sp != std::string::npos && s.size() > sp + 5 && s.compare(s.size() - 5, 5, "-Core") == 0) s.erase(sp);
    }
  }
  std::string out;
  for (char c : s) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  return trim(out);
}

// The integrated GPU name from the CPU model, e.g. "Radeon 890M".
std::string integrated_gpu_name(const std::string& cpu_model) {
  size_t p = cpu_model.find(" w/ ");
  if (p == std::string::npos) return "";
  return trim(cpu_model.substr(p + 4));
}

// Readable source name for chips without a model: "mt7925_phy0" -> "MediaTek MT7925".
std::string chip_title(const std::string& name) {
  static const std::map<std::string, std::string> known = {
      {"acpitz", "ACPI"}, {"thinkpad", "ThinkPad"}, {"dell_smm", "Dell"}, {"applesmc", "Apple SMC"},
      {"iwlwifi", "Intel"}, {"brcmfmac", "Broadcom"}, {"r8169", "Realtek"}, {"igc", "Intel"}, {"e1000e", "Intel"}};
  std::string base = name;
  size_t phy = base.find("_phy");
  if (phy != std::string::npos) base.erase(phy);
  size_t sep = base.find('_');
  std::string head = sep == std::string::npos ? base : base.substr(0, sep);
  auto it = known.find(base);
  if (it == known.end()) it = known.find(head);
  if (it != known.end()) return it->second;
  if (starts_with(base, "mt7") || starts_with(base, "mt76")) {
    std::string up = base;
    for (char& ch : up) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    return "MediaTek " + up;
  }
  return base;
}

double sane_temp(double v) { return std::isfinite(v) && v > kMinTemp && v < kMaxTemp ? v : NAN; }
double sane_limit(double v) { return std::isfinite(v) && v >= kMinLimit && v <= kMaxLimit ? v : NAN; }

// Which reading stands for "the processor temperature" in the bar.
// Lower score wins.
int cpu_score(const std::string& chip, const std::string& kind, const std::string& label) {
  if (chip == "k10temp" || chip == "zenpower") {
    if (label == "Tdie") return 0;
    if (label == "Tctl") return 1;
    return 5;
  }
  if (chip == "coretemp") return starts_with(label, "Package") ? 0 : 6;
  if (kind == "cpu") return 7;
  if (chip == "thinkpad" && label == "CPU") return 10;
  if (chip == "acpitz") return 20;
  return INT_MAX;
}

class Monitor {
 public:
  Monitor(double warn, std::string cpu_model) : warn_(warn), cpu_model_(std::move(cpu_model)) {}

  Snapshot build(const std::vector<Chip>& chips) {
    Snapshot snap;
    std::map<std::string, bool> seen;
    int best_score = INT_MAX;
    int gpu_chips = 0;
    bool real_fans = false;
    for (const Chip& c : chips) {
      if (kind_for(c.name) == "gpu") gpu_chips++;
      if (c.name != "acpi_fan")
        for (const RawFan& f : c.fans)
          if (f.rpm >= 0 && f.rpm <= kMaxRpm) real_fans = true;
    }

    Group memory;
    memory.id = "memory";
    memory.kind = "memory";

    for (const Chip& c : chips) {
      std::string kind = kind_for(c.name);
      Group g;
      g.id = c.id;
      g.kind = kind;
      g.chip = c.name;
      if (kind == "cpu") g.title = clean_cpu_name(cpu_model_);
      else if (kind == "gpu" && c.name == "amdgpu" && gpu_chips == 1) g.title = integrated_gpu_name(cpu_model_);
      else if (kind == "storage") g.title = c.model;
      else if (kind != "memory") g.title = chip_title(c.name);

      bool any_label = false;
      for (const RawTemp& r : c.temps)
        if (!r.label.empty() && std::isfinite(sane_temp(r.value))) any_label = true;

      for (const RawTemp& r : c.temps) {
        double t = sane_temp(r.value);
        if (!std::isfinite(t)) continue;
        // Board chips list many unnamed duplicates; keep the named ones.
        if (kind == "system" && any_label && r.label.empty()) continue;
        if (r.label.empty() && t == 0.0) continue;
        Reading rd;
        rd.n = r.n;
        rd.label = r.label;
        rd.t = t;
        rd.high = sane_limit(r.max);
        rd.crit = sane_limit(r.crit);
        if (std::isfinite(rd.high) && std::isfinite(rd.crit) && rd.high > rd.crit) rd.high = NAN;
        rd.limit = limit_for(kind, rd);
        rd.key = c.id + "/" + std::to_string(r.n);
        rd.hot = update_hot(rd.key, rd.t, rd.limit);
        seen[rd.key] = true;

        int score = cpu_score(c.name, kind, r.label);
        if (score < best_score) {
          best_score = score;
          snap.cpu = t;
        }
        if (rd.hot) {
          snap.hot = true;
          if (rising_.count(rd.key)) snap.alerts.push_back({kind, g.title, rd.label, t});
        }
        if (kind == "system" && !any_label && !g.temps.empty()) continue;
        g.temps.push_back(rd);
      }
      if (g.temps.size() > kMaxReadings) g.temps.resize(kMaxReadings);

      if (kind == "memory") {
        // One group for all modules, numbered in slot order.
        for (Reading& rd : g.temps) {
          memory.temps.push_back(rd);
        }
      } else if (!g.temps.empty() && snap.groups.size() < kMaxChips) {
        snap.groups.push_back(g);
      }

      if (c.name == "acpi_fan" && real_fans) continue;
      for (const RawFan& f : c.fans) {
        if (f.rpm < 0 || f.rpm > kMaxRpm || snap.fans.size() >= kMaxFans) continue;
        snap.fans.push_back({f.n, f.label, c.name, f.rpm});
      }
    }
    if (!memory.temps.empty()) {
      for (size_t i = 0; i < memory.temps.size(); i++) memory.temps[i].n = static_cast<int>(i) + 1;
      snap.groups.push_back(memory);
    }

    std::stable_sort(snap.groups.begin(), snap.groups.end(), [](const Group& a, const Group& b) {
      return kind_rank(a.kind) < kind_rank(b.kind);
    });
    // Forget sensors that went away so they alert again when they return.
    for (auto it = hot_.begin(); it != hot_.end();) it = seen.count(it->first) ? std::next(it) : hot_.erase(it);
    rising_.clear();
    return snap;
  }

  double limit_for(const std::string& kind, const Reading& rd) const {
    double limit = std::isfinite(rd.high) ? rd.high : (std::isfinite(rd.crit) ? rd.crit - 5.0 : NAN);
    if (kind == "cpu" || kind == "gpu") limit = std::isfinite(limit) ? std::min(limit, warn_) : warn_;
    return limit;
  }

  // Hot when at or above the limit; normal again only kHysteresis below it.
  bool update_hot(const std::string& key, double t, double limit) {
    bool was = hot_.count(key) && hot_[key];
    bool now = std::isfinite(limit) && (was ? t > limit - kHysteresis : t >= limit);
    if (now && !was) rising_[key] = true;
    hot_[key] = now;
    return now;
  }

 private:
  double warn_;
  std::string cpu_model_;
  std::map<std::string, bool> hot_;
  std::map<std::string, bool> rising_;
};

// ------------------------------------------------------------------ sysfs

std::string cpu_model_from(const std::string& path) {
  auto text = read_file(path, 1 << 20);
  if (!text) return "";
  size_t pos = 0;
  while (pos < text->size()) {
    size_t end = text->find('\n', pos);
    if (end == std::string::npos) end = text->size();
    std::string line = text->substr(pos, end - pos);
    if (starts_with(line, "model name")) {
      size_t colon = line.find(':');
      if (colon != std::string::npos) return clip(trim(line.substr(colon + 1)));
    }
    pos = end + 1;
  }
  return "";
}

std::string device_id(const std::string& chip_dir, const std::string& fallback) {
  char buf[PATH_MAX];
  std::string dev = chip_dir + "/device";
  if (realpath(dev.c_str(), buf)) {
    std::string p = buf;
    size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
  }
  return fallback;
}

double milli(const std::string& path) {
  auto v = read_long(path);
  return v ? static_cast<double>(*v) / 1000.0 : NAN;
}

std::vector<Chip> scan(const std::string& root) {
  std::vector<Chip> chips;
  DIR* d = opendir(root.c_str());
  if (!d) return chips;
  std::vector<std::string> entries;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (starts_with(n, "hwmon")) entries.push_back(n);
  }
  closedir(d);
  // hwmon2 before hwmon10.
  std::sort(entries.begin(), entries.end(), [](const std::string& a, const std::string& b) {
    return a.size() != b.size() ? a.size() < b.size() : a < b;
  });

  for (const std::string& entry : entries) {
    if (chips.size() >= kMaxChips) break;
    std::string dir = root + "/" + entry;
    auto name = read_file(dir + "/name", 128);
    if (!name) continue;
    Chip c;
    c.name = clip(trim(*name));
    if (c.name.empty()) continue;
    c.id = c.name + "@" + clip(device_id(dir, entry));
    if (auto model = read_file(dir + "/device/model", 256)) c.model = clip(trim(*model));

    DIR* cd = opendir(dir.c_str());
    if (!cd) continue;
    std::vector<int> temps, fans;
    while (dirent* e = readdir(cd)) {
      std::string f = e->d_name;
      int t = attr_index(f, "temp", "_input");
      if (t >= 0) temps.push_back(t);
      int fn = attr_index(f, "fan", "_input");
      if (fn >= 0) fans.push_back(fn);
    }
    closedir(cd);
    std::sort(temps.begin(), temps.end());
    std::sort(fans.begin(), fans.end());

    for (int i : temps) {
      if (c.temps.size() >= kMaxReadings) break;
      std::string base = dir + "/temp" + std::to_string(i);
      RawTemp r;
      r.n = i;
      r.value = milli(base + "_input");
      if (!std::isfinite(r.value)) continue;
      if (auto label = read_file(base + "_label", 128)) r.label = clip(trim(*label));
      r.max = milli(base + "_max");
      r.crit = milli(base + "_crit");
      if (!std::isfinite(r.crit)) r.crit = milli(base + "_emergency");
      c.temps.push_back(r);
    }
    for (int i : fans) {
      if (c.fans.size() >= kMaxFans) break;
      std::string base = dir + "/fan" + std::to_string(i);
      auto rpm = read_long(base + "_input");
      if (!rpm) continue;
      RawFan f;
      f.n = i;
      f.rpm = *rpm;
      if (auto label = read_file(base + "_label", 128)) f.label = clip(trim(*label));
      c.fans.push_back(f);
    }
    if (!c.temps.empty() || !c.fans.empty()) chips.push_back(c);
  }
  return chips;
}

// ------------------------------------------------------------------ output

std::string to_json(const Snapshot& s) {
  std::string out = "{\"v\":" + std::to_string(kProtocolVersion);
  out += ",\"cpu\":" + json_num(s.cpu);
  out += std::string(",\"hot\":") + (s.hot ? "true" : "false");
  out += ",\"groups\":[";
  for (size_t i = 0; i < s.groups.size(); i++) {
    const Group& g = s.groups[i];
    if (i) out += ",";
    out += "{\"id\":" + json_str(g.id) + ",\"kind\":" + json_str(g.kind) + ",\"chip\":" + json_str(g.chip) +
           ",\"title\":" + json_str(g.title) + ",\"temps\":[";
    for (size_t j = 0; j < g.temps.size(); j++) {
      const Reading& r = g.temps[j];
      if (j) out += ",";
      out += "{\"n\":" + std::to_string(r.n) + ",\"label\":" + json_str(r.label) + ",\"t\":" + json_num(r.t) +
             ",\"high\":" + json_num(r.high) + ",\"crit\":" + json_num(r.crit) + ",\"limit\":" + json_num(r.limit) +
             ",\"hot\":" + (r.hot ? "true" : "false") + "}";
    }
    out += "]}";
  }
  out += "],\"fans\":[";
  for (size_t i = 0; i < s.fans.size(); i++) {
    const Fan& f = s.fans[i];
    if (i) out += ",";
    out += "{\"n\":" + std::to_string(f.n) + ",\"label\":" + json_str(f.label) + ",\"chip\":" + json_str(f.chip) +
           ",\"rpm\":" + std::to_string(f.rpm) + "}";
  }
  out += "],\"alerts\":[";
  for (size_t i = 0; i < s.alerts.size(); i++) {
    const Alert& a = s.alerts[i];
    if (i) out += ",";
    out += "{\"kind\":" + json_str(a.kind) + ",\"title\":" + json_str(a.title) + ",\"label\":" + json_str(a.label) +
           ",\"t\":" + json_num(a.t) + "}";
  }
  out += "]}";
  return out;
}

// ------------------------------------------------------------------ demo

std::vector<Chip> demo_chips(double now) {
  auto wave = [&](double mid, double amp, double period, double phase = 0) {
    return mid + amp * std::sin(now / period * 2 * M_PI + phase);
  };
  std::vector<Chip> chips;
  Chip cpu{"k10temp@0000:00:18.3", "k10temp", "", {}, {}};
  cpu.temps.push_back({1, "Tctl", wave(72, 24, 40), NAN, NAN});
  chips.push_back(cpu);
  Chip gpu{"amdgpu@0000:c4:00.0", "amdgpu", "", {}, {}};
  gpu.temps.push_back({1, "edge", wave(58, 10, 30, 1), NAN, NAN});
  chips.push_back(gpu);
  Chip dimm1{"spd5118@22-0050", "spd5118", "", {}, {}};
  dimm1.temps.push_back({1, "", wave(46, 3, 50), 55, 85});
  chips.push_back(dimm1);
  Chip dimm2{"spd5118@22-0051", "spd5118", "", {}, {}};
  dimm2.temps.push_back({1, "", wave(47, 3, 50, 2), 55, 85});
  chips.push_back(dimm2);
  Chip nvme{"nvme@nvme0", "nvme", "Demo NVMe SSD 1TB", {}, {}};
  nvme.temps.push_back({1, "Composite", wave(52, 8, 60), 85.8, 87.8});
  nvme.temps.push_back({2, "Sensor 1", wave(50, 8, 60), 65261.8, NAN});
  chips.push_back(nvme);
  Chip ec{"thinkpad@thinkpad_hwmon", "thinkpad", "", {}, {}};
  ec.temps.push_back({1, "CPU", wave(70, 20, 40), NAN, NAN});
  ec.fans.push_back({1, "", static_cast<long>(wave(3200, 2200, 40))});
  ec.fans.push_back({2, "", static_cast<long>(wave(3000, 2000, 40, 0.5))});
  chips.push_back(ec);
  Chip wifi{"mt7925_phy0@phy0", "mt7925_phy0", "", {}, {}};
  wifi.temps.push_back({1, "", wave(50, 2, 20), NAN, NAN});
  chips.push_back(wifi);
  return chips;
}

// ------------------------------------------------------------------ notify lock

// Every monitor runs its own widget (and helper); only one may raise alerts.
class NotifyLock {
 public:
  bool held() {
    if (fd_ >= 0) return true;
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    std::string dir = runtime && *runtime ? runtime : "/tmp";
    std::string path = dir + "/omarchy-sensors-monitor-" + std::to_string(getuid()) + ".lock";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      close(fd);
      return false;
    }
    fd_ = fd;
    return true;
  }

 private:
  int fd_ = -1;
};

// ------------------------------------------------------------------ self test

bool write_file(const std::string& path, const std::string& text) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return false;
  bool ok = fputs(text.c_str(), f) >= 0;
  return fclose(f) == 0 && ok;
}

int self_test() {
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      failures++;
    }
  };

  check(json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\u000a", "json escaping");
  check(json_num(61.06) == "61.1" && json_num(NAN) == "null", "number format");
  check(attr_index("temp12_input", "temp", "_input") == 12, "attr index");
  check(attr_index("temp_input", "temp", "_input") == -1, "attr index without digits");
  check(attr_index("temp1_label", "temp", "_input") == -1, "attr index other suffix");
  check(clean_cpu_name("AMD Ryzen AI 9 HX PRO 370 w/ Radeon 890M") == "AMD Ryzen AI 9 HX PRO 370", "cpu name w/");
  check(clean_cpu_name("AMD Ryzen 7 5800X 8-Core Processor") == "AMD Ryzen 7 5800X", "cpu name core suffix");
  check(clean_cpu_name("Intel(R) Core(TM) i7-8650U CPU @ 1.90GHz") == "Intel Core i7-8650U", "cpu name intel");
  check(integrated_gpu_name("AMD Ryzen AI 9 HX PRO 370 w/ Radeon 890M") == "Radeon 890M", "igpu name");
  check(kind_for("k10temp") == "cpu" && kind_for("nvme") == "storage" && kind_for("spd5118") == "memory" &&
            kind_for("mt7925_phy0") == "wifi" && kind_for("acpitz") == "system" && kind_for("amdgpu") == "gpu",
        "kinds");
  check(std::isnan(sane_limit(65261.8)) && sane_limit(85.8) == 85.8, "limit sanity");
  check(chip_title("mt7925_phy0") == "MediaTek MT7925" && chip_title("acpitz") == "ACPI" &&
            chip_title("iwlwifi_1") == "Intel" && chip_title("nct6798") == "nct6798",
        "chip titles");
  check(clip(std::string(100, 'x')).size() == kMaxText, "clip length");
  check(clip("a\x01" "b") == "ab", "clip control chars");

  // Hysteresis and rising-edge alerts.
  {
    Monitor m(90, "");
    std::vector<Chip> chips{{"k10temp@x", "k10temp", "", {{1, "Tctl", 85, NAN, NAN}}, {}}};
    Snapshot s = m.build(chips);
    check(!s.hot && s.alerts.empty() && s.cpu == 85, "cool cpu");
    chips[0].temps[0].value = 91;
    s = m.build(chips);
    check(s.hot && s.alerts.size() == 1, "hot cpu alerts once");
    chips[0].temps[0].value = 88.5;
    s = m.build(chips);
    check(s.hot && s.alerts.empty(), "hysteresis keeps hot without new alert");
    chips[0].temps[0].value = 86;
    s = m.build(chips);
    check(!s.hot, "cools below hysteresis");
  }

  // Drive limit from the driver; garbage thresholds ignored; board duplicates dropped.
  {
    Monitor m(90, "AMD Ryzen AI 9 HX PRO 370 w/ Radeon 890M");
    std::vector<Chip> chips{
        {"thinkpad@ec", "thinkpad", "", {{1, "CPU", 60, NAN, NAN}, {3, "", 60, NAN, NAN}, {4, "", 0, NAN, NAN}},
         {{1, "", 4500}, {2, "", 4400}}},
        {"nvme@nvme0", "nvme", "Micron", {{1, "Composite", 86, 85.8, 87.8}, {2, "Sensor 1", 50, 65261.8, NAN}}, {}},
        {"k10temp@cpu", "k10temp", "", {{1, "Tctl", 61.1, NAN, NAN}}, {}},
        {"amdgpu@gpu", "amdgpu", "", {{1, "edge", 54, NAN, NAN}}, {}},
        {"acpi_fan@f", "acpi_fan", "", {}, {{1, "", 0}}},
        {"spd5118@22-0051", "spd5118", "", {{1, "", 44, 55, 85}}, {}},
        {"spd5118@22-0050", "spd5118", "", {{1, "", 43, 55, 85}}, {}},
    };
    Snapshot s = m.build(chips);
    check(s.cpu == 61.1, "cpu prefers k10temp over thinkpad");
    check(s.groups.size() == 5 && s.groups[0].kind == "cpu" && s.groups[1].kind == "gpu" &&
              s.groups[2].kind == "memory" && s.groups[3].kind == "storage" && s.groups[4].kind == "system",
          "group order");
    check(s.groups[0].title == "AMD Ryzen AI 9 HX PRO 370", "cpu title");
    check(s.groups[1].title == "Radeon 890M", "gpu title");
    check(s.groups[2].temps.size() == 2 && s.groups[2].temps[0].n == 1 && s.groups[2].temps[1].n == 2, "memory merged");
    check(s.groups[3].temps[0].hot && std::isnan(s.groups[3].temps[1].high), "nvme limits");
    check(s.groups[4].temps.size() == 1 && s.groups[4].temps[0].label == "CPU", "board keeps labeled only");
    check(s.fans.size() == 2, "acpi_fan dropped when real fans exist");
    check(s.hot && s.alerts.size() == 1 && s.alerts[0].kind == "storage", "drive alert");
  }

  // Scanning a fake sysfs tree.
  {
    char tmpl[] = "/tmp/sensors-selftest-XXXXXX";
    char* root = mkdtemp(tmpl);
    check(root != nullptr, "mkdtemp");
    if (root) {
      std::string r = root;
      mkdir((r + "/hwmon3").c_str(), 0700);
      mkdir((r + "/hwmon10").c_str(), 0700);
      mkdir((r + "/other").c_str(), 0700);
      write_file(r + "/hwmon3/name", "k10temp\n");
      write_file(r + "/hwmon3/temp1_input", "61125\n");
      write_file(r + "/hwmon3/temp1_label", "Tctl\n");
      write_file(r + "/hwmon10/name", "thinkpad\n");
      write_file(r + "/hwmon10/fan1_input", "4567\n");
      write_file(r + "/hwmon10/temp2_input", "bogus\n");
      auto chips = scan(r);
      check(chips.size() == 2 && chips[0].name == "k10temp" && chips[1].name == "thinkpad", "scan order");
      check(chips.size() == 2 && chips[0].temps.size() == 1 && std::fabs(chips[0].temps[0].value - 61.125) < 1e-9,
            "scan temp");
      check(chips.size() == 2 && chips[1].temps.empty() && chips[1].fans.size() == 1 && chips[1].fans[0].rpm == 4567,
            "scan fan, skip bogus");
      std::string cmd = "rm -rf '" + r + "'";
      check(system(cmd.c_str()) == 0, "cleanup");
    }
  }

  if (failures == 0) printf("ok\n");
  return failures == 0 ? 0 : 1;
}

}  // namespace

// ------------------------------------------------------------------ main

int main(int argc, char** argv) {
  double interval = 2.0;
  double warn = 90.0;
  bool once = false, demo = false;
  std::string sysfs = "/sys/class/hwmon";
  std::string cpuinfo = "/proc/cpuinfo";
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--interval" && i + 1 < argc) {
      interval = std::atof(argv[++i]);
    } else if (arg == "--warn" && i + 1 < argc) {
      warn = std::atof(argv[++i]);
    } else if (arg == "--sysfs" && i + 1 < argc) {
      sysfs = argv[++i];
    } else if (arg == "--cpuinfo" && i + 1 < argc) {
      cpuinfo = argv[++i];
    } else if (arg == "--once") {
      once = true;
    } else if (arg == "--demo") {
      demo = true;
    } else if (arg == "--self-test") {
      return self_test();
    } else if (arg == "--help" || arg == "-h") {
      printf("usage: sensors [--interval SECONDS] [--warn CELSIUS] [--sysfs DIR] [--cpuinfo FILE] [--once] [--demo] [--self-test]\n");
      return 0;
    }
  }
  if (!std::isfinite(interval)) interval = 2.0;
  if (!std::isfinite(warn)) warn = 90.0;
  interval = std::min(30.0, std::max(0.5, interval));
  warn = std::min(120.0, std::max(40.0, warn));
  signal(SIGPIPE, SIG_IGN);

  Monitor monitor(warn, demo ? "AMD Ryzen 9 Demo w/ Radeon Demo" : cpu_model_from(cpuinfo));
  NotifyLock lock;
  pid_t parent = getppid();
  std::string last;
  timespec pause{static_cast<time_t>(interval), static_cast<long>((interval - static_cast<long>(interval)) * 1e9)};

  while (true) {
    Snapshot snap = monitor.build(demo ? demo_chips(now_seconds()) : scan(sysfs));
    if (!snap.alerts.empty() && !lock.held()) snap.alerts.clear();
    std::string line = to_json(snap);
    if (line != last) {
      std::string out = line + "\n";
      if (fputs(out.c_str(), stdout) < 0 || fflush(stdout) != 0) return 0;
      last = line;
    }
    if (once) return 0;
    if (getppid() != parent) return 0;  // the shell went away
    nanosleep(&pause, nullptr);
  }
}
