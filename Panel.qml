import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

// Bar widget: a thermometer with the processor temperature next to it, and a
// popup listing every temperature sensor and fan.
Panel {
  id: root
  moduleName: "sensors.monitor"
  ipcTarget: "sensors.monitor"

  readonly property string pluginDir: decodeURIComponent(String(Qt.resolvedUrl(".")).replace(/^file:\/\//, "")).replace(/\/$/, "")
  // Builds sensors.cpp on first use, then runs it.
  readonly property string helperPath: pluginDir + "/run.sh"
  property int quickExits: 0
  property real startedAt: 0

  readonly property bool showTemperature: setting("showTemperature", true) !== false
  readonly property bool notifyHot: setting("notifyHot", true) !== false
  readonly property int warnAt: Math.max(60, Math.min(110, Math.round(Number(setting("warnAt", 90)) || 90)))
  readonly property int refreshSeconds: Math.max(1, Math.min(30, Math.round(Number(setting("refreshSeconds", 2)) || 2)))

  property bool loaded: false
  property var groups: []
  property var fans: []
  property var cpuTemp: null
  property bool hot: false
  readonly property int rpm: Model.maxRpm(fans)
  readonly property color ink: hot ? Commons.Color.urgent : (root.bar ? root.bar.foreground : Commons.Color.foreground)

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  readonly property var tr: ({ t: function(key) { return strings.t(key) } })

  function tempText(t) {
    return Model.formatTemp(t, Commons.I18n.formatNumber)
  }

  function rpmText(rpm) {
    return rpm > 0 ? strings.t("fan.rpm").arg(Commons.I18n.formatNumber(rpm, 0)) : strings.t("fan.stopped")
  }

  function tooltip() {
    if (!root.loaded) return strings.t("panel.starting")
    var parts = []
    var kinds = ["cpu", "gpu", "memory", "storage"]
    for (var i = 0; i < kinds.length; i++) {
      var t = kinds[i] === "cpu" && root.cpuTemp !== null ? root.cpuTemp : Model.kindTemp(root.groups, kinds[i])
      if (t !== null) parts.push(strings.t("kind." + kinds[i]) + " " + root.tempText(t))
    }
    if (root.rpm >= 0) parts.push(strings.t("panel.fans") + " " + root.rpmText(root.rpm))
    return parts.length > 0 ? parts.join(" · ") : strings.t("bar.none")
  }

  function applySnapshot(line) {
    var snap = Model.parseSnapshot(line)
    if (!snap) return
    root.groups = snap.groups
    root.fans = snap.fans
    root.cpuTemp = snap.cpu
    root.hot = snap.hot
    root.loaded = true
    if (root.notifyHot && root.bar) {
      for (var i = 0; i < snap.alerts.length; i++) {
        var a = snap.alerts[i]
        var body = strings.t("kind." + a.kind) + (a.title ? " (" + a.title + ")" : "") + ": " + root.tempText(a.t)
        root.bar.run("omarchy-notification-send -g 󰈐 "
          + Util.shellQuote(strings.t("notify.title"))
          + " " + Util.shellQuote(Model.notificationSafe(body)))
      }
    }
  }

  function restartMonitor() {
    monitor.running = false
    Qt.callLater(function() { monitor.running = true })
  }

  onWarnAtChanged: restartMonitor()
  onRefreshSecondsChanged: restartMonitor()

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  Process {
    id: monitor
    command: ["sh", root.helperPath, "--interval", String(root.refreshSeconds), "--warn", String(root.warnAt)]
    running: true
    // Process already has a C++ slot named onStarted, so track starts here.
    onRunningChanged: if (running) root.startedAt = Date.now()
    stdout: SplitParser {
      onRead: function(line) {
        root.quickExits = 0
        root.applySnapshot(line)
      }
    }
    onExited: {
      // A helper that dies right away (e.g. no compiler) backs off to a
      // minute instead of rebuilding every few seconds.
      if (Date.now() - root.startedAt < 3000) root.quickExits++
      respawn.interval = root.quickExits >= 3 ? 60000 : 5000
      respawn.restart()
    }
  }

  Timer {
    id: respawn
    interval: 5000
    onTriggered: if (!monitor.running) monitor.running = true
  }

  TextMetrics {
    id: tempMetrics
    font.family: root.bar ? root.bar.fontFamily : Style.fontFamily
    font.pixelSize: Style.bar.iconFont
    text: "100°"
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    iconComponent: thermometerIcon
    labelText: root.showTemperature && root.cpuTemp !== null && !vertical
      ? Commons.I18n.formatNumber(Math.round(root.cpuTemp), 0) + "°" : ""
    slotSize: Style.bar.iconSlot + (labelText !== "" ? tempMetrics.advanceWidth + Style.space(4) : 0)
    tooltipText: root.opened ? "" : root.tooltip()
    onPressed: function(b) { root.toggle() }
  }

  // Thermometer drawn from shapes, so it follows the theme colour and needs
  // no particular icon font. The column rises with the processor temperature.
  Component {
    id: thermometerIcon

    // A fixed box as the root: the bar sizes icons by their painted bounds,
    // which would otherwise change with the column.
    Rectangle {
      id: icon
      readonly property real s: Style.bar.iconSize
      readonly property real stroke: Math.max(1, Math.round(s * 0.09))
      readonly property real level: root.cpuTemp === null ? 0.35 : Math.max(0.15, Math.min(1, root.cpuTemp / 100))
      width: s
      height: s
      color: "transparent"

      // Tube
      Rectangle {
        id: tube
        x: (icon.s - width) / 2
        y: icon.s * 0.02
        width: Math.round(icon.s * 0.34)
        height: icon.s * 0.66
        radius: width / 2
        color: "transparent"
        border.width: icon.stroke
        border.color: root.ink
      }

      // Column, from the bulb up to the current level.
      Rectangle {
        readonly property real colTop: tube.y + icon.stroke + 0.5
        readonly property real colBottom: bulb.y + bulb.height / 2
        x: (icon.s - width) / 2
        width: Math.max(1, tube.width - icon.stroke * 2)
        radius: width / 2
        color: root.ink
        height: (colBottom - colTop) * icon.level
        y: colBottom - height
        Behavior on height { NumberAnimation { duration: Style.duration(320); easing.type: Easing.OutCubic } }
      }

      // Bulb
      Rectangle {
        id: bulb
        width: Math.round(icon.s * 0.58)
        height: width
        radius: width / 2
        x: (icon.s - width) / 2
        y: icon.s - height
        color: root.ink
      }
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(360))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight)

    content: PanelKeyCatcher {
      anchors.fill: parent
      implicitHeight: column.implicitHeight
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Style.space(12)

        // ---------- Hero ----------
        Item {
          width: parent.width
          implicitHeight: Math.max(heroLabels.implicitHeight, heroTemp.implicitHeight)

          Column {
            id: heroLabels
            anchors.left: parent.left
            anchors.right: heroTemp.left
            anchors.rightMargin: Style.space(10)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            PlainText {
              width: parent.width
              text: strings.t("panel.title")
              font.pixelSize: Style.font.title
              font.bold: true
            }

            PlainText {
              width: parent.width
              text: (!root.loaded ? strings.t("panel.starting") : (root.hot ? strings.t("panel.hot") : strings.t("panel.ok"))).toUpperCase()
              color: root.hot ? Commons.Color.urgent : (root.bar ? root.bar.foreground : Commons.Color.foreground)
              opacity: root.hot ? 1 : 0.6
              font.pixelSize: Style.font.caption
              font.bold: true
              font.letterSpacing: 1.2
            }
          }

          PlainText {
            id: heroTemp
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: root.cpuTemp !== null ? root.tempText(root.cpuTemp) : ""
            color: root.ink
            font.pixelSize: Style.font.heading
            font.bold: true
          }
        }

        PlainText {
          visible: root.loaded && root.groups.length === 0
          width: parent.width
          text: strings.t("panel.empty")
          opacity: 0.6
          wrapMode: Text.WordWrap
          font.pixelSize: Style.font.bodySmall
        }

        // ---------- Temperature groups ----------
        Repeater {
          model: root.groups

          Column {
            id: groupItem
            required property var modelData
            readonly property var group: modelData
            width: column.width
            spacing: Style.space(6)

            PanelSeparator { foreground: root.bar.foreground }

            GroupHeader {
              width: parent.width
              heading: strings.t("kind." + groupItem.group.kind)
              detail: groupItem.group.title
            }

            Repeater {
              model: groupItem.group.temps

              ReadingRow {
                required property var modelData
                width: groupItem.width
                label: Model.readingLabel(groupItem.group, modelData, root.tr)
                value: root.tempText(modelData.t)
                fraction: Model.fraction(modelData)
                level: Model.level(modelData)
              }
            }
          }
        }

        // ---------- Memory without a sensor ----------
        Column {
          visible: root.loaded && root.groups.length > 0 && !Model.hasKind(root.groups, "memory")
          width: parent.width
          spacing: Style.space(6)

          PanelSeparator { foreground: root.bar.foreground }

          GroupHeader {
            width: parent.width
            heading: strings.t("kind.memory")
            detail: ""
          }

          PlainText {
            width: parent.width
            text: strings.t("panel.noMemory")
            opacity: 0.6
            wrapMode: Text.WordWrap
            elide: Text.ElideNone
            font.pixelSize: Style.font.caption
          }
        }

        // ---------- Fans ----------
        Column {
          visible: root.fans.length > 0
          width: parent.width
          spacing: Style.space(6)

          PanelSeparator { foreground: root.bar.foreground }

          GroupHeader {
            width: parent.width
            heading: strings.t("panel.fans")
            detail: ""
          }

          Repeater {
            model: root.fans

            ReadingRow {
              required property var modelData
              width: column.width
              label: Model.fanLabel(modelData, root.fans.length, root.tr)
              value: root.rpmText(modelData.rpm)
              fraction: Math.min(1, modelData.rpm / 6000)
              level: "ok"
            }
          }
        }
      }
    }
  }

  component GroupHeader: Item {
    property string heading: ""
    property string detail: ""
    implicitHeight: Math.max(headingText.implicitHeight, detailText.implicitHeight)

    PlainText {
      id: headingText
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      text: parent.heading
      font.pixelSize: Style.font.body
      font.bold: true
      elide: Text.ElideNone
    }

    PlainText {
      id: detailText
      anchors.left: headingText.right
      anchors.leftMargin: Style.space(8)
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      horizontalAlignment: Text.AlignRight
      text: parent.detail
      opacity: 0.55
      font.pixelSize: Style.font.caption
    }
  }

  // Label, a small gauge, and the value. Warm readings are bolder; hot ones
  // use the theme's urgent colour.
  component ReadingRow: Item {
    id: row
    property string label: ""
    property string value: ""
    property real fraction: 0
    property string level: "ok"
    readonly property color tone: level === "hot" ? Commons.Color.urgent : (root.bar ? root.bar.foreground : Commons.Color.foreground)
    implicitHeight: Math.max(rowLabel.implicitHeight, rowValue.implicitHeight)

    PlainText {
      id: rowLabel
      anchors.left: parent.left
      anchors.right: gauge.left
      anchors.rightMargin: Style.space(10)
      anchors.verticalCenter: parent.verticalCenter
      text: row.label
      opacity: 0.8
      font.pixelSize: Style.font.bodySmall
    }

    Item {
      id: gauge
      anchors.right: rowValue.left
      anchors.rightMargin: Style.space(10)
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(90)
      height: Style.space(5)

      Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Qt.rgba(row.tone.r, row.tone.g, row.tone.b, 0.12)
      }

      Rectangle {
        height: parent.height
        radius: height / 2
        color: row.tone
        opacity: row.level === "ok" ? 0.7 : 1
        width: Math.max(height, parent.width * Math.max(0, Math.min(1, row.fraction)))
        Behavior on width { NumberAnimation { duration: Style.duration(320); easing.type: Easing.OutCubic } }
      }
    }

    PlainText {
      id: rowValue
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      width: Style.space(92)
      horizontalAlignment: Text.AlignRight
      text: row.value
      color: row.tone
      font.pixelSize: Style.font.bodySmall
      font.bold: row.level !== "ok"
    }
  }

  component PlainText: Text {
    textFormat: Text.PlainText
    color: root.bar ? root.bar.foreground : Commons.Color.foreground
    font.family: root.bar ? root.bar.fontFamily : Style.fontFamily
    elide: Text.ElideRight
  }
}
