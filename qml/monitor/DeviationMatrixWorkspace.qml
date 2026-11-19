import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 偏差矩阵：50 架 × 8 个驱动轮最后一次检测的偏差值（视觉端上报的 deviation，服务端不计算）。
// 着色阈值只影响本页显示，不参与 OK/NG 判定；保存在系统配置中，重启后保留。
Item {
    id: root
    anchors.fill: parent

    readonly property int rackCount: 50
    readonly property int driveWheelCount: 8
    readonly property int firstDriveWheel: 9   // 驱动轮 1 对应轮位 9

    property var deviationByCell: ({})
    property string lastUpdated: ""
    property int hoveredRack: -1
    property int hoveredWheel: -1
    property int requestId: -1

    // 偏差 >= warnThreshold 绿色；dangerThreshold <= 偏差 < warnThreshold 黄色；偏差 < dangerThreshold 红色
    property real warnThreshold: -10
    property real dangerThreshold: -20

    readonly property var wheelRows: {
        var rows = []
        for (var i = 0; i < driveWheelCount; ++i) {
            var raw = firstDriveWheel + i
            rows.push({ wheel: raw, label: appController.wheelTagName(raw) })
        }
        return rows
    }

    readonly property int recordedCount: Object.keys(deviationByCell).length
    readonly property int warnCount: countLevel("warn")
    readonly property int dangerCount: countLevel("danger")

    function cellKey(rack, wheel) {
        return rack + ":" + wheel
    }

    function hasDeviation(cell) {
        return cell !== undefined && cell !== null && cell.deviation !== undefined && cell.deviation !== null
    }

    // ok / warn / danger / none
    function levelOf(cell) {
        if (!hasDeviation(cell))
            return "none"
        var v = Number(cell.deviation)
        if (v >= warnThreshold)
            return "ok"
        if (v >= dangerThreshold)
            return "warn"
        return "danger"
    }

    function countLevel(level) {
        var count = 0
        for (var key in deviationByCell) {
            if (levelOf(deviationByCell[key]) === level)
                ++count
        }
        return count
    }

    function formatNumber(value, digits) {
        var n = Number(value)
        return isFinite(n) ? n.toFixed(digits) : "—"
    }

    function refresh() {
        if (!appController || !appController.requestStats)
            return
        requestId = appController.requestStats("rackDriveWheelDeviation", [])
    }

    function applyDeviation(rows) {
        var cells = ({})
        for (var i = 0; i < rows.length; ++i)
            cells[cellKey(rows[i].rack, rows[i].wheel)] = rows[i]
        deviationByCell = cells
        lastUpdated = Qt.formatDateTime(new Date(), "yyyy-MM-dd HH:mm:ss")
    }

    function loadThresholds() {
        if (!appController || !appController.deviationThresholds)
            return
        var t = appController.deviationThresholds()
        warnThreshold = Number(t.warn)
        dangerThreshold = Number(t.danger)
        warnField.text = formatThreshold(warnThreshold)
        dangerField.text = formatThreshold(dangerThreshold)
    }

    function formatThreshold(value) {
        return String(Math.round(value * 100) / 100)
    }

    function saveThresholds() {
        var warn = Number(warnField.text.trim())
        var danger = Number(dangerField.text.trim())
        if (warnField.text.trim() === "" || dangerField.text.trim() === "" || !isFinite(warn) || !isFinite(danger)) {
            toast.show(false, "请输入有效的阈值")
            return
        }
        if (danger >= warn) {
            toast.show(false, "红色阈值必须小于黄色阈值")
            return
        }
        if (!appController.setDeviationThresholds(warn, danger)) {
            toast.show(false, "阈值保存失败，请查看日志")
            return
        }
        loadThresholds()
        toast.show(true, "阈值已保存")
    }

    // 最新图像在后台查询，返回前再次点击以最后一次为准
    property int latestImageRequestId: -1
    property var latestImageRequest: ({})

    function openLatestImage(rackNumber, wheelNumber, wheelLabel) {
        if (!appController || !appController.requestLatestWheelImage)
            return
        latestImageRequest = { rack: rackNumber, label: wheelLabel }
        latestImageRequestId = appController.requestLatestWheelImage(rackNumber, wheelNumber)
    }

    function showLatestImage(image) {
        if (!image || !image.filePath || String(image.filePath).length === 0)
            return
        var normalizedPath = String(image.filePath).replace(/\\/g, "/")
        var sourceUrl = /^[a-zA-Z]:\//.test(normalizedPath) ? "file:///" + normalizedPath : normalizedPath
        imagePreview.openViewer(sourceUrl,
                "架号 #" + latestImageRequest.rack + " · " + latestImageRequest.label,
                "最新检测判定: " + (image.result === 1 ? "OK 正常" : "NG 异常") + " · 采集时间: " + image.time)
    }

    Component.onCompleted: {
        loadThresholds()
        refresh()
    }

    Connections {
        target: appController
        function onRackWheelMonitorUpdated() {
            root.refresh()
        }
        function onCurrentBatchChanged() {
            // 一个载具分两次触发，合并短时间内的多次变化后再查询
            refreshTimer.restart()
        }
        function onStatsReady(requestId, kind, result) {
            if (requestId === root.requestId)
                root.applyDeviation(result || [])
        }
        function onLatestWheelImageReady(requestId, image) {
            if (requestId === root.latestImageRequestId)
                root.showLatestImage(image)
        }
    }

    Timer {
        id: refreshTimer
        interval: 1500
        repeat: false
        onTriggered: root.refresh()
    }

    component LegendItem: RowLayout {
        property color swatch: Theme.ok
        property color swatchBorder: Theme.okBorder
        property string text: ""
        spacing: Theme.space1
        Rectangle {
            width: 12
            height: 12
            radius: Theme.radiusMicro
            color: parent.swatch
            border.width: 1
            border.color: parent.swatchBorder
        }
        Label {
            text: parent.text
            color: Theme.textSoft
            font.family: Theme.fontSans
            font.features: Theme.fontFeaturesTabular
            font.pixelSize: Theme.fontCaption
        }
    }

    component ThresholdField: TextField {
        id: field
        Layout.preferredWidth: 72
        Layout.preferredHeight: 30
        leftPadding: Theme.space2
        rightPadding: Theme.space2
        horizontalAlignment: TextInput.AlignHCenter
        selectByMouse: true
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        validator: RegularExpressionValidator { regularExpression: /^-?\d{0,4}(\.\d{0,2})?$/ }
        color: Theme.text
        placeholderTextColor: Theme.muted
        font.family: Theme.fontSans
        font.features: Theme.fontFeaturesTabular
        font.pixelSize: Theme.fontBody
        background: Rectangle {
            color: Theme.fieldFill
            radius: Theme.radiusSmall
            border.width: field.activeFocus ? 2 : 1
            border.color: field.activeFocus ? Theme.selectedBorder : (field.hovered ? Theme.borderStrong : Theme.border)
        }
        Keys.onReturnPressed: root.saveThresholds()
        Keys.onEnterPressed: root.saveThresholds()
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMedium
        color: Theme.surface
        border.width: 1
        border.color: Theme.border

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            // 标题、图例与同步时间
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    width: 4
                    height: 18
                    radius: Theme.radiusPill
                    color: Theme.borderStrong
                }

                Label {
                    text: "50 架驱动轮最后一次偏差值"
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontTitle
                    font.weight: Theme.fontMedium
                }

                RowLayout {
                    Layout.leftMargin: 12
                    spacing: 14

                    LegendItem {
                        swatch: Theme.okSoft
                        swatchBorder: Theme.okBorder
                        text: root.formatThreshold(root.warnThreshold) + " 及以上"
                    }
                    LegendItem {
                        swatch: Theme.warnSoft
                        swatchBorder: Theme.warnBorder
                        text: root.formatThreshold(root.dangerThreshold) + " ~ " + root.formatThreshold(root.warnThreshold)
                    }
                    LegendItem {
                        swatch: Theme.dangerSoft
                        swatchBorder: Theme.dangerBorder
                        text: "低于 " + root.formatThreshold(root.dangerThreshold)
                    }
                    LegendItem {
                        swatch: "transparent"
                        swatchBorder: Theme.borderStrong
                        text: "无偏差值"
                    }
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: "黄 " + root.warnCount + " · 红 " + root.dangerCount + " · 已检 " + root.recordedCount + " / " + (root.rackCount * root.driveWheelCount)
                    color: Theme.textSoft
                    font.family: Theme.fontSans
                    font.features: Theme.fontFeaturesTabular
                    font.pixelSize: Theme.fontCaption
                }

                Rectangle {
                    Layout.preferredHeight: 24
                    implicitWidth: updateLabel.implicitWidth + 16
                    radius: Theme.radiusPill
                    color: Theme.surfaceAlt
                    border.color: Theme.borderMuted
                    border.width: 1

                    Label {
                        id: updateLabel
                        anchors.centerIn: parent
                        text: root.lastUpdated === "" ? "数据加载中..." : "同步时间: " + root.lastUpdated
                        color: Theme.textSoft
                        font.family: Theme.fontSans
                        font.features: Theme.fontFeaturesTabular
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }

            // 阈值设置
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space2

                Label {
                    text: "着色阈值"
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontCaption
                    font.weight: Theme.fontMedium
                }
                Label {
                    text: "偏差低于"
                    color: Theme.textSoft
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontCaption
                }
                ThresholdField {
                    id: warnField
                    Accessible.name: "黄色阈值"
                }
                Label {
                    text: "为黄色，低于"
                    color: Theme.textSoft
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontCaption
                }
                ThresholdField {
                    id: dangerField
                    Accessible.name: "红色阈值"
                }
                Label {
                    text: "为红色"
                    color: Theme.textSoft
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontCaption
                }
                ActionButton {
                    text: "保存阈值"
                    variant: "secondary"
                    Layout.preferredHeight: 30
                    Layout.leftMargin: Theme.space2
                    onClicked: root.saveThresholds()
                }
                ActionButton {
                    text: "恢复默认"
                    variant: "secondary"
                    Layout.preferredHeight: 30
                    onClicked: {
                        warnField.text = "-10"
                        dangerField.text = "-20"
                        root.saveThresholds()
                    }
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: "单位 mm，单击单元格查看最新检测图像"
                    color: Theme.muted
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontCaption
                }
            }

            // 8 个驱动轮 × 50 架矩阵（纵向驱动轮，横向架号，自适应铺满无滚动条）
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.fieldFill
                radius: Theme.radiusSmall
                border.width: 1
                border.color: Theme.borderMuted
                clip: true

                Item {
                    id: tableContainer
                    anchors.fill: parent
                    anchors.margins: 1

                    readonly property real wheelHeaderWidth: Math.max(88, Math.min(112, Math.floor(width * 0.06)))
                    readonly property real headerHeight: Math.max(26, Math.min(32, Math.floor(height * 0.06)))
                    readonly property real rowHeight: (height - headerHeight) / root.driveWheelCount
                    readonly property real colWidth: (width - wheelHeaderWidth) / root.rackCount
                    readonly property int valueFontSize: colWidth >= 40 ? Theme.fontCaption : Theme.fontMicro

                    // 表头：驱动轮 / 架号 + 50 架号列
                    Row {
                        id: header
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: tableContainer.headerHeight
                        z: 10

                        Rectangle {
                            width: tableContainer.wheelHeaderWidth
                            height: parent.height
                            color: Theme.pressed
                            border.color: Theme.borderMuted
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "驱动轮 / 架号"
                                color: Theme.text
                                font.family: Theme.fontSans
                                font.weight: Theme.fontMedium
                                font.pixelSize: Theme.fontCaption
                            }
                        }

                        Repeater {
                            model: root.rackCount
                            delegate: Rectangle {
                                id: rackHeader
                                required property int index
                                readonly property int rackNumber: index + 1
                                readonly property bool isHovered: rackNumber === root.hoveredRack

                                width: tableContainer.colWidth
                                height: parent.height
                                color: isHovered ? Theme.pressed : (index % 2 === 0 ? Theme.surfaceAlt : Theme.surface)
                                border.color: isHovered ? Theme.borderStrong : Theme.borderMuted
                                border.width: isHovered ? 2 : 1
                                z: isHovered ? 2 : 0

                                Text {
                                    anchors.centerIn: parent
                                    text: rackHeader.rackNumber
                                    color: rackHeader.isHovered ? Theme.text : Theme.textSoft
                                    font.family: Theme.fontSans
                                    font.features: Theme.fontFeaturesTabular
                                    font.weight: rackHeader.isHovered ? Theme.fontMedium : Theme.fontRegular
                                    font.pixelSize: tableContainer.colWidth >= 32 ? Theme.fontCaption : Theme.fontMicro
                                }
                            }
                        }
                    }

                    Column {
                        anchors.top: header.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom

                        Repeater {
                            model: root.wheelRows
                            delegate: Rectangle {
                                id: wheelRow
                                required property var modelData
                                required property int index
                                readonly property int wheelNumber: modelData.wheel
                                readonly property string wheelLabel: modelData.label
                                readonly property bool isHovered: wheelNumber === root.hoveredWheel

                                width: parent.width
                                height: tableContainer.rowHeight
                                color: index % 2 === 0 ? "transparent" : Theme.subtleFill

                                Row {
                                    anchors.fill: parent

                                    Rectangle {
                                        width: tableContainer.wheelHeaderWidth
                                        height: parent.height
                                        color: wheelRow.isHovered ? Theme.pressed : Theme.driveWheelSoft
                                        border.color: wheelRow.isHovered ? Theme.borderStrong : Theme.borderMuted
                                        border.width: wheelRow.isHovered ? 2 : 1
                                        z: wheelRow.isHovered ? 2 : 0

                                        Rectangle {
                                            anchors.left: parent.left
                                            anchors.top: parent.top
                                            anchors.bottom: parent.bottom
                                            width: 3
                                            color: Theme.driveWheel
                                        }

                                        Text {
                                            anchors.centerIn: parent
                                            anchors.horizontalCenterOffset: 2
                                            text: wheelRow.wheelLabel
                                            color: wheelRow.isHovered ? Theme.text : Theme.driveWheel
                                            font.family: Theme.fontSans
                                            font.weight: Theme.fontMedium
                                            font.pixelSize: Theme.fontCaption
                                        }
                                    }

                                    Repeater {
                                        model: root.rackCount
                                        delegate: Rectangle {
                                            id: cell
                                            required property int index
                                            readonly property int rackNumber: index + 1
                                            readonly property var cellState: root.deviationByCell[root.cellKey(rackNumber, wheelRow.wheelNumber)]
                                            readonly property string level: root.levelOf(cellState)
                                            readonly property bool isHovered: rackNumber === root.hoveredRack && wheelRow.isHovered
                                            readonly property bool isCrosshair: (rackNumber === root.hoveredRack || wheelRow.isHovered) && !isHovered

                                            width: tableContainer.colWidth
                                            height: parent.height
                                            color: level === "ok" ? Theme.okSoft
                                                 : level === "warn" ? Theme.warnSoft
                                                 : level === "danger" ? Theme.dangerSoft
                                                 : (isCrosshair ? Theme.surfaceAlt : "transparent")
                                            border.color: isHovered ? Theme.text : (isCrosshair ? Theme.borderStrong : Theme.borderMuted)
                                            border.width: isHovered ? 2 : 1
                                            z: isHovered ? 3 : (isCrosshair ? 1 : 0)

                                            Text {
                                                anchors.centerIn: parent
                                                width: parent.width - 2
                                                horizontalAlignment: Text.AlignHCenter
                                                elide: Text.ElideNone
                                                text: root.hasDeviation(cell.cellState)
                                                      ? root.formatNumber(cell.cellState.deviation, 1)
                                                      : (cell.cellState ? "—" : "")
                                                color: cell.level === "ok" ? Theme.ok
                                                     : cell.level === "warn" ? Theme.warn
                                                     : cell.level === "danger" ? Theme.danger
                                                     : Theme.muted
                                                font.family: Theme.fontSans
                                                font.features: Theme.fontFeaturesTabular
                                                font.weight: cell.level === "ok" || cell.level === "none" ? Theme.fontRegular : Theme.fontMedium
                                                font.pixelSize: tableContainer.valueFontSize
                                            }

                                            ToolTip.visible: cellMouse.containsMouse && Boolean(cell.cellState)
                                            ToolTip.text: cell.cellState
                                                          ? ("架号 #" + cell.rackNumber + " · " + wheelRow.wheelLabel
                                                             + "\n偏差值: " + (root.hasDeviation(cell.cellState)
                                                                              ? root.formatNumber(cell.cellState.deviation, 3) + " mm"
                                                                              : "无（最后一次记录未上报偏差值）")
                                                             + "\n判定: " + (cell.cellState.result === 1 ? "OK 正常" : "NG 异常")
                                                             + "\n检测时间: " + cell.cellState.time
                                                             + "\n单击查看检测原图")
                                                          : ""

                                            MouseArea {
                                                id: cellMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: cell.cellState ? Qt.PointingHandCursor : Qt.ArrowCursor
                                                onEntered: {
                                                    root.hoveredRack = cell.rackNumber
                                                    root.hoveredWheel = wheelRow.wheelNumber
                                                }
                                                onExited: {
                                                    if (root.hoveredRack === cell.rackNumber) root.hoveredRack = -1
                                                    if (root.hoveredWheel === wheelRow.wheelNumber) root.hoveredWheel = -1
                                                }
                                                onClicked: {
                                                    if (cell.cellState)
                                                        root.openLatestImage(cell.rackNumber, wheelRow.wheelNumber, wheelRow.wheelLabel)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ResultToast {
        id: toast
        z: 10
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space6
    }

    ZoomOverlay {
        id: imagePreview
        anchors.fill: parent
    }
}
