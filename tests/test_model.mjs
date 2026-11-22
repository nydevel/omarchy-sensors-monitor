// Tests for Model.js: node tests/test_model.mjs
import { readFileSync } from "node:fs"
import assert from "node:assert/strict"
import vm from "node:vm"

const source = readFileSync(new URL("../Model.js", import.meta.url), "utf8").replace(/^\.pragma library\s*/, "")
// QML strings have .arg(); emulate it inside the sandbox realm, where Model.js runs.
const ARG = `String.prototype.arg = function (v) {
  const m = this.match(/%(\\d)/g)
  if (!m) return String(this)
  const lowest = m.map(x => +x[1]).sort()[0]
  return this.replace(new RegExp("%" + lowest, "g"), String(v))
}
`
const M = {}
vm.runInNewContext(ARG + source + "\nObject.assign(out, {parseSnapshot, hasKind, maxRpm, level, fraction, formatTemp, readingLabel, fanLabel, kindTemp, notificationSafe})", { out: M })

const en = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url), "utf8"))
const ru = JSON.parse(readFileSync(new URL("../i18n/ru.json", import.meta.url), "utf8"))
const tr = { t: k => en[k] || k }
const fmt = (n, d) => Number(n).toFixed(d || 0)

let tests = 0
function test(name, fn) {
  try { fn() } catch (e) { console.error("FAIL: " + name); throw e }
  tests++
}

const sample = {
  v: 1, cpu: 61.1, hot: false,
  groups: [
    { id: "k10temp@x", kind: "cpu", chip: "k10temp", title: "AMD Ryzen", temps: [{ n: 1, label: "Tctl", t: 61.1, high: null, crit: null, limit: 90, hot: false }] },
    { id: "memory", kind: "memory", chip: "", title: "", temps: [{ n: 1, label: "", t: 44, high: 55, crit: 85, limit: 55, hot: false }, { n: 2, label: "", t: 46, high: 55, crit: 85, limit: 55, hot: false }] },
    { id: "nvme@n", kind: "storage", chip: "nvme", title: "SSD", temps: [{ n: 1, label: "Composite", t: 86, high: 85.8, crit: 87.8, limit: 85.8, hot: true }, { n: 2, label: "Sensor 1", t: 50, high: null, crit: null, limit: null, hot: false }] }
  ],
  fans: [{ n: 1, label: "", chip: "thinkpad", rpm: 4567 }, { n: 2, label: "", chip: "thinkpad", rpm: 4400 }],
  alerts: [{ kind: "storage", title: "SSD", label: "Composite", t: 86 }]
}

test("rejects junk", () => {
  assert.equal(M.parseSnapshot(""), null)
  assert.equal(M.parseSnapshot("{"), null)
  assert.equal(M.parseSnapshot('{"v":2,"groups":[]}'), null)
  assert.equal(M.parseSnapshot('{"v":1}'), null)
  assert.equal(M.parseSnapshot("x".repeat(200000)), null)
})

test("parses a snapshot", () => {
  const s = M.parseSnapshot(JSON.stringify(sample))
  assert.equal(s.cpu, 61.1)
  assert.equal(s.groups.length, 3)
  assert.equal(s.fans.length, 2)
  assert.equal(s.alerts.length, 1)
  assert.equal(s.groups[2].temps[0].hot, true)
})

test("sanitises values", () => {
  const s = M.parseSnapshot(JSON.stringify({
    v: 1, cpu: "hot", hot: "yes",
    groups: [
      { id: 1, kind: "evil", title: "t\u0007" + "y".repeat(200), temps: [{ n: -4, label: '<img src="https://example.invalid/x">', t: 50, high: 1e9, hot: 1 }, { t: "NaN" }, null] },
      { kind: "cpu", temps: [] },
      { kind: "gpu" }
    ],
    fans: [{ rpm: 99999 }, { rpm: -1 }, { n: 1, rpm: 1200.4 }, "x"],
    alerts: [{ kind: "cpu", t: null }, { kind: "cpu", t: 95, title: 7 }]
  }))
  assert.equal(s.cpu, null)
  assert.equal(s.hot, false)
  assert.equal(s.groups.length, 1)
  const g = s.groups[0]
  assert.equal(g.kind, "other")
  assert.equal(g.title.length, 80)
  assert.ok(!/\u0007/.test(g.title))
  assert.equal(g.temps.length, 1)
  assert.equal(g.temps[0].n, 0)
  assert.equal(g.temps[0].high, null)
  assert.equal(g.temps[0].hot, false)
  assert.equal(JSON.stringify(s.fans.map(f => f.rpm)), "[1200]")
  assert.equal(s.alerts.length, 1)
  assert.equal(s.alerts[0].title, "7")
})

test("fastest fan", () => {
  assert.equal(M.maxRpm([]), -1)
  assert.equal(M.maxRpm(sample.fans), 4567)
})

test("levels and gauge", () => {
  assert.equal(M.level({ t: 50, limit: 90, hot: false }), "ok")
  assert.equal(M.level({ t: 82, limit: 90, hot: false }), "warm")
  assert.equal(M.level({ t: 91, limit: 90, hot: true }), "hot")
  assert.equal(M.level({ t: 99, limit: null, hot: false }), "ok")
  assert.equal(M.fraction({ t: 43.9, crit: 87.8, limit: 85.8 }), 0.5)
  assert.equal(M.fraction({ t: 50, crit: null, limit: 90 }), 0.5)
  assert.equal(M.fraction({ t: 150, crit: null, limit: null }), 1)
  assert.equal(M.fraction({ t: -5, crit: null, limit: null }), 0)
})

test("labels", () => {
  const s = M.parseSnapshot(JSON.stringify(sample))
  assert.equal(M.readingLabel(s.groups[0], s.groups[0].temps[0], tr), "Die")
  assert.equal(M.readingLabel(s.groups[1], s.groups[1].temps[1], tr), "Module 2")
  assert.equal(M.readingLabel(s.groups[2], s.groups[2].temps[0], tr), "Overall")
  assert.equal(M.readingLabel(s.groups[2], s.groups[2].temps[1], tr), "Sensor 1")
  const lone = { kind: "wifi", temps: [{ n: 1, label: "" }] }
  assert.equal(M.readingLabel(lone, lone.temps[0], tr), "Temperature")
  const many = { kind: "system", temps: [{ n: 3, label: "" }, { n: 5, label: "" }] }
  assert.equal(M.readingLabel(many, many.temps[1], tr), "Sensor 5")
  const intel = { kind: "cpu", temps: [{ n: 1, label: "Package id 0" }, { n: 2, label: "Core 4" }, { n: 3, label: "weird" }] }
  assert.equal(M.readingLabel(intel, intel.temps[0], tr), "Package")
  assert.equal(M.readingLabel(intel, intel.temps[1], tr), "Core 4")
  assert.equal(M.readingLabel(intel, intel.temps[2], tr), "weird")
  assert.equal(M.fanLabel({ n: 1, label: "" }, 1, tr), "Fan")
  assert.equal(M.fanLabel({ n: 2, label: "" }, 2, tr), "Fan 2")
  assert.equal(M.fanLabel({ n: 2, label: "CPU fan" }, 2, tr), "CPU fan")
})

test("summary helpers", () => {
  const s = M.parseSnapshot(JSON.stringify(sample))
  assert.equal(M.kindTemp(s.groups, "storage"), 86)
  assert.equal(M.kindTemp(s.groups, "gpu"), null)
  assert.equal(M.hasKind(s.groups, "memory"), true)
  assert.equal(M.hasKind(s.groups, "gpu"), false)
  assert.equal(M.formatTemp(61.4, fmt), "61 °C")
  assert.equal(M.formatTemp(null, fmt), "—")
  assert.equal(M.notificationSafe('<b>"CPU"</b> & co'), "bCPU/b  co")
})

test("translations cover the same keys", () => {
  assert.deepEqual(Object.keys(ru).sort(), Object.keys(en).sort())
})

console.log(`model: ${tests} tests passed`)
