.pragma library

// Pure helpers for Panel.qml. Kept free of QML types so tests can load them
// in a plain JavaScript engine.

var MAX_LINE = 100000
var MAX_GROUPS = 32
var MAX_TEMPS = 16
var MAX_FANS = 8
var MAX_ALERTS = 16
var MAX_TEXT = 80
var KINDS = ["cpu", "gpu", "memory", "storage", "system", "wifi", "network", "battery", "other"]

function text(value) {
  var s = String(value === undefined || value === null ? "" : value)
  // Drop control characters; display sinks are PlainText anyway.
  s = s.replace(/[\u0000-\u001f\u007f]/g, "")
  return s.length > MAX_TEXT ? s.slice(0, MAX_TEXT - 1) + "…" : s
}

function temp(value) {
  if (value === null || value === undefined || value === "") return null
  var n = Number(value)
  return isFinite(n) && n > -50 && n < 200 ? n : null
}

function index(value) {
  var n = Math.floor(Number(value))
  return isFinite(n) && n >= 0 && n < 1000 ? n : 0
}

function kind(value) {
  return KINDS.indexOf(value) >= 0 ? value : "other"
}

function parseSnapshot(line) {
  if (typeof line !== "string" || line.length === 0 || line.length > MAX_LINE) return null
  var raw
  try { raw = JSON.parse(line) } catch (e) { return null }
  if (!raw || typeof raw !== "object" || raw.v !== 1 || !Array.isArray(raw.groups)) return null

  var groups = []
  for (var i = 0; i < raw.groups.length && groups.length < MAX_GROUPS; i++) {
    var g = raw.groups[i]
    if (!g || typeof g !== "object" || !Array.isArray(g.temps)) continue
    var temps = []
    for (var j = 0; j < g.temps.length && temps.length < MAX_TEMPS; j++) {
      var r = g.temps[j]
      if (!r || typeof r !== "object") continue
      var t = temp(r.t)
      if (t === null) continue
      temps.push({
        n: index(r.n),
        label: text(r.label),
        t: t,
        high: temp(r.high),
        crit: temp(r.crit),
        limit: temp(r.limit),
        hot: r.hot === true
      })
    }
    if (temps.length === 0) continue
    groups.push({ id: text(g.id), kind: kind(g.kind), chip: text(g.chip), title: text(g.title), temps: temps })
  }

  var fans = []
  if (Array.isArray(raw.fans)) {
    for (var k = 0; k < raw.fans.length && fans.length < MAX_FANS; k++) {
      var f = raw.fans[k]
      if (!f || typeof f !== "object") continue
      var rpm = Number(f.rpm)
      if (!isFinite(rpm) || rpm < 0 || rpm > 30000) continue
      fans.push({ n: index(f.n), label: text(f.label), chip: text(f.chip), rpm: Math.round(rpm) })
    }
  }

  var alerts = []
  if (Array.isArray(raw.alerts)) {
    for (var a = 0; a < raw.alerts.length && alerts.length < MAX_ALERTS; a++) {
      var al = raw.alerts[a]
      if (!al || typeof al !== "object") continue
      var at = temp(al.t)
      if (at === null) continue
      alerts.push({ kind: kind(al.kind), title: text(al.title), label: text(al.label), t: at })
    }
  }

  return { cpu: temp(raw.cpu), hot: raw.hot === true, groups: groups, fans: fans, alerts: alerts }
}

function hasKind(groups, k) {
  for (var i = 0; i < groups.length; i++) if (groups[i].kind === k) return true
  return false
}

// Fastest fan, or -1 when the machine reports no fans.
function maxRpm(fans) {
  if (!fans || fans.length === 0) return -1
  var max = 0
  for (var i = 0; i < fans.length; i++) max = Math.max(max, fans[i].rpm)
  return max
}

// "ok" | "warm" | "hot". Warm is within 10 °C of the limit.
function level(reading) {
  if (!reading) return "ok"
  if (reading.hot) return "hot"
  if (reading.limit !== null && reading.t >= reading.limit - 10) return "warm"
  return "ok"
}

// How full the little bar next to a reading is: 0 °C empty, the critical
// point (or limit + 10, or 100 °C) full.
function fraction(reading) {
  if (!reading) return 0
  var top = reading.crit !== null ? reading.crit : (reading.limit !== null ? reading.limit + 10 : 100)
  return Math.max(0, Math.min(1, reading.t / Math.max(1, top)))
}

function formatTemp(t, formatNumber) {
  if (t === null || t === undefined) return "—"
  return formatNumber(Math.round(t), 0) + " °C"
}

// tr: { t(key) } — translations for labels.
function readingLabel(group, reading, tr) {
  if (group.kind === "memory") return tr.t("label.module").arg(reading.n)
  var known = { "Composite": "label.composite", "Tctl": "label.tctl", "Tdie": "label.tdie", "edge": "label.edge",
    "junction": "label.junction", "mem": "label.vram", "CPU": "label.cpu", "GPU": "label.gpu" }
  if (reading.label) {
    if (known[reading.label]) return tr.t(known[reading.label])
    if (/^Package id \d+$/.test(reading.label)) return tr.t("label.package")
    if (/^Core \d+$/.test(reading.label)) return tr.t("label.core").arg(reading.label.slice(5))
    if (/^Sensor \d+$/.test(reading.label)) return tr.t("label.sensor").arg(reading.label.slice(7))
    return reading.label
  }
  return group.temps.length === 1 ? tr.t("label.temperature") : tr.t("label.sensor").arg(reading.n)
}

function fanLabel(fan, count, tr) {
  if (fan.label) return fan.label
  return count === 1 ? tr.t("fan.single") : tr.t("fan.numbered").arg(fan.n)
}

// Hottest reading of the first group of a kind.
function kindTemp(groups, k) {
  for (var i = 0; i < groups.length; i++) {
    if (groups[i].kind !== k) continue
    var max = null
    for (var j = 0; j < groups[i].temps.length; j++) {
      var t = groups[i].temps[j].t
      if (max === null || t > max) max = t
    }
    return max
  }
  return null
}

// Notification bodies may be read as markup by the daemon; keep only text.
function notificationSafe(value) {
  return text(value).replace(/[<>&"']/g, "")
}
