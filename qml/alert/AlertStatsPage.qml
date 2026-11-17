import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../common"

Page {
    id: root
    objectName: "alertStatsPage"
    anchors.fill: parent
    background: Rectangle { color: "transparent" }

    property var rackRows: []
    property var wheelRows: []
    property int selectedRack: 0
    property int selectedWheel: 0

    property int rackRequestId: -1
    property int wheelRequestId: -1
    property int imageRequestId: -1
    property int dispositionRequestId: -1
    property int imagePage: 1
    property int imageTotal: 0
    property var currentImage: ({})
    property string queryStart: ""
    property string queryEnd: ""
    property string operationMessage: ""
    property var pendingDisposition: ({})
    property bool preserveWheel: false
    // 系统设置中预设的处置人员；上一次所选人员在本次运行内再次处置时预选
    readonly property var handlerNames: appController ? appController.alertHandlers : []
    property string lastOperatorName: ""
    property string operatorError: ""

    function resetImage() {
        imageRequestId = -1
        imagePage = 1
        imageTotal = 0
        currentImage = ({})
        viewer.visible = false
    }

    function disposeWheel(wheel, kind, pendingCount) {
        if (dispositionRequestId !== -1 || dispositionDialog.visible || pendingCount <= 0) return
        pendingDisposition = { rack: selectedRack, wheel: wheel, kind: kind,
                               start: queryStart, end: queryEnd, count: pendingCount }
        dispositionDialog.open()
    }

    function confirmDisposition() {
        if (dispositionRequestId !== -1 || !pendingDisposition.kind) return
        // 可从名单选择，也可直接填写名单外的姓名
        var operatorName = String(operatorCombo.editText).trim()
        if (operatorName.length === 0) {
            operatorError = "请填写处置人员"
            operatorCombo.forceActiveFocus()
            return
        }
        operatorError = ""
        lastOperatorName = operatorName
        var operation = pendingDisposition
        operationMessage = "正在保存处置状态…"
        dispositionRequestId = appController.requestAlertOperation(operation.kind, operation.start, operation.end,
                                                                  operation.rack, operation.wheel, 1, operatorName)
        dispositionDialog.close()
    }

    function refreshRackCounts() {
        rackRequestId = appController.requestAlertOperation("racks", queryStart, queryEnd, 0, 0, 1)
    }

    function loadRackStatistics() {
        if (!appController || !appController.requestAlertOperation)
            return
        queryStart = dateBar.startDate
        queryEnd = dateBar.endDate
        wheelRequestId = -1
        operationMessage = ""
        rackRows = []
        selectedRack = 0
        selectedWheel = 0
        wheelRows = []
        resetImage()
        refreshRackCounts()
    }

    function applyRackStatistics(stats) {
        var rows = []
        for (var i = 0; i < stats.length; ++i) {
            var row = stats[i]
            var count = Number(row.count)
            if (count > 0) {
                rows.push({ rack: Number(row.rack), count: count,
                            pending: Number(row.pending), handled: Number(row.handled) })
            }
        }
        rackRows = rows
        if (selectedRack > 0) return
        selectedWheel = 0
        wheelRows = []
        resetImage()
        if (rows.length > 0)
            loadWheelStatistics(rows[0].rack)
    }

    function loadWheelStatistics(rack, preserve) {
        preserveWheel = preserve === true && selectedRack === rack
        selectedRack = rack
        if (!preserveWheel) {
            selectedWheel = 0
            wheelRows = []
            resetImage()
        }
        wheelRequestId = appController.requestAlertOperation("groups", queryStart, queryEnd,
                                                             rack, 0, 1)
    }

    function applyWheelStatistics(stats) {
        wheelRows = stats
        if (preserveWheel && selectedWheel > 0) return
        if (stats.length > 0) loadImages(Number(stats[0].wheel))
        else resetImage()
    }

    Connections {
        target: appController
        function onAlertOperationFinished(requestId, result) {
            if (requestId === root.rackRequestId) {
                root.rackRequestId = -1
                if (result.ok) root.applyRackStatistics(result.rows || [])
                else root.operationMessage = result.error
            } else if (requestId === root.dispositionRequestId) {
                root.dispositionRequestId = -1
                root.operationMessage = result.ok
                    ? "已保存，本次处置 " + result.affected + " 条异常记录"
                    : result.error
                if (result.ok) root.refreshRackCounts()
                if (root.selectedRack > 0) root.loadWheelStatistics(root.selectedRack, true)
            } else if (requestId === root.wheelRequestId) {
                root.wheelRequestId = -1
                if (result.ok) root.applyWheelStatistics(result.rows || [])
                else root.operationMessage = result.error
            } else if (requestId === root.imageRequestId) {
                root.imageRequestId = -1
                if (result.ok) {
                    root.imagePage = Number(result.page)
                    root.imageTotal = Number(result.total)
                    root.currentImage = result.image || ({})
                } else root.operationMessage = result.error
            }
        }
    }

    function loadImages(wheel) {
        selectedWheel = wheel
        resetImage()
        loadImagePage(1)
    }

    function loadImagePage(page) {
        if (selectedRack <= 0 || selectedWheel <= 0) return
        imageRequestId = appController.requestAlertOperation("image", queryStart, queryEnd,
                                                             selectedRack, selectedWheel, page)
    }

    function generateCsv() {
        var csv = "wheel_type,position_range,轮系类别,架号,NG数量,未处理频次,已处理频次\n"
        var prefix = "all,1～16,全部轮位,"
        for (var i = 0; i < rackRows.length; ++i)
            csv += prefix + rackRows[i].rack + "," + rackRows[i].count + "," + rackRows[i].pending + "," + rackRows[i].handled + "\n"
        return csv
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // 顶栏：日期查询栏（全部轮位）
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            DateQueryBar {
                id: dateBar
                defaultRangeDays: 7
                Layout.fillWidth: true
                wheelType: -1
                exportCsvContent: root.generateCsv()
                onSearchClicked: function(startDate, endDate) {
                    root.loadRackStatistics()
                }
                onResetClicked: root.loadRackStatistics()
            }

        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            // 1. 架号 NG 汇总栏
            Rectangle {
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                radius: Theme.radiusMedium
                color: Theme.surface
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle { width: 3; height: 14; radius: Theme.radiusPill; color: Theme.borderStrong }
                        Label {
                            text: "架号异常"
                            color: Theme.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontBody
                            font.weight: Theme.fontMedium
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 32
                        color: Theme.surfaceAlt
                        radius: Theme.radiusMicro

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            Text { text: "架号"; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; font.weight: Theme.fontMedium; color: Theme.textSoft; font.family: Theme.fontSans; font.pixelSize: Theme.fontCaption }
                            Text { text: "未处理频次"; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignHCenter; font.weight: Theme.fontMedium; color: Theme.textSoft; font.family: Theme.fontSans; font.pixelSize: Theme.fontCaption }
                            Text { text: "已处理频次"; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignHCenter; font.weight: Theme.fontMedium; color: Theme.textSoft; font.family: Theme.fontSans; font.pixelSize: Theme.fontCaption }
                        }
                    }

                    ListView {
                        id: rackList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: root.rackRows
                        clip: true
                        spacing: 4

                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            readonly property bool selected: root.selectedRack === modelData.rack
                            width: rackList.width
                            height: 38
                            radius: Theme.radiusSmall
                            color: selected ? Theme.pressed : (itemMouse.containsMouse ? Theme.surfaceAlt : Theme.fieldFill)
                            border.width: 1
                            border.color: selected ? Theme.accent : "transparent"
                            clip: true

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 6

                                // 排行徽章
                                Rectangle {
                                    Layout.preferredWidth: 18
                                    Layout.preferredHeight: 18
                                    radius: Theme.radiusPill
                                    color: Theme.surfaceAlt
                                    border.color: Theme.borderMuted
                                    border.width: 1
                                    Text {
                                        anchors.centerIn: parent
                                        text: String(index + 1)
                                        color: Theme.muted
                                        font.family: Theme.fontSans; font.features: Theme.fontFeaturesTabular
                                        font.pixelSize: Theme.fontMicro
                                        font.weight: Theme.fontMedium
                                    }
                                }

                                Text {
                                    text: "架 #" + parent.parent.modelData.rack
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignHCenter
                                    color: parent.parent.selected ? Theme.accent : Theme.text
                                    font.weight: Theme.fontMedium
                                    font.family: Theme.fontSans; font.features: Theme.fontFeaturesTabular
                                    font.pixelSize: Theme.fontCaption
                                }

                                Text {
                                    text: parent.parent.modelData.pending
                                    Layout.preferredWidth: 80
                                    horizontalAlignment: Text.AlignHCenter
                                    color: Number(parent.parent.modelData.pending) > 0 ? Theme.danger : Theme.text
                                    font.weight: Theme.fontMedium
                                    font.family: Theme.fontSans
                                    font.features: Theme.fontFeaturesTabular
                                    font.pixelSize: Theme.fontCaption
                                }
                                Text {
                                    text: parent.parent.modelData.handled
                                    Layout.preferredWidth: 80
                                    horizontalAlignment: Text.AlignHCenter
                                    color: Theme.text
                                    font.weight: Theme.fontMedium
                                    font.family: Theme.fontSans
                                    font.features: Theme.fontFeaturesTabular
                                    font.pixelSize: Theme.fontCaption
                                }
                            }

                            MouseArea {
                                id: itemMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.loadWheelStatistics(parent.modelData.rack)
                            }
                        }

                        // 优雅的零报警空状态
                        ColumnLayout {
                            anchors.centerIn: parent
                            visible: rackList.count === 0
                            spacing: 8

                            AppIcon {
                                Layout.alignment: Qt.AlignHCenter
                                name: "icon_check_circle"
                                size: 36
                                color: Theme.ok
                            }

                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: "全区间运行正常"
                                color: Theme.ok
                                font.family: Theme.fontSans
                                font.weight: Theme.fontMedium
                                font.pixelSize: Theme.fontCaption
                            }

                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: "当前时间范围无异常记录"
                                color: Theme.muted
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                }
            }

            // 2. 轮位 NG 汇总栏
            Rectangle {
                Layout.preferredWidth: 330
                Layout.fillHeight: true
                radius: Theme.radiusMedium
                color: Theme.surface
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle { width: 3; height: 14; radius: Theme.radiusPill; color: Theme.borderStrong }
                        Label {
                            text: selectedRack > 0 ? "架 #" + selectedRack + " · 异常统计" : "异常统计"
                            color: Theme.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontBody
                            font.weight: Theme.fontMedium
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 32
                        color: Theme.surfaceAlt
                        radius: Theme.radiusMicro

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            Text { text: "轮号"; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; font.weight: Theme.fontMedium; color: Theme.textSoft; font.family: Theme.fontSans; font.pixelSize: Theme.fontCaption }
                            Text { text: "NG 频次"; Layout.preferredWidth: 70; horizontalAlignment: Text.AlignHCenter; font.weight: Theme.fontMedium; color: Theme.danger; font.family: Theme.fontSans; font.pixelSize: Theme.fontCaption }
                        }
                    }

                    ListView {
                        id: wheelList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: root.wheelRows
                        clip: true
                        spacing: 4

                        delegate: Rectangle {
                            id: wheelDelegate
                            required property var modelData
                            required property int index
                            readonly property bool selected: root.selectedWheel === modelData.wheel
                            width: wheelList.width
                            height: 96
                            radius: Theme.radiusSmall
                            color: selected ? Theme.pressed : (wheelMouse.containsMouse ? Theme.surfaceAlt : Theme.fieldFill)
                            border.width: 1
                            border.color: selected ? Theme.accent : "transparent"
                            clip: true

                            MouseArea {
                                id: wheelMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.loadImages(wheelDelegate.modelData.wheel)
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: Theme.space2
                                spacing: Theme.space1
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: appController.wheelLabel(wheelDelegate.modelData.wheel)
                                        Layout.fillWidth: true
                                        color: Theme.text
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontCaption
                                        font.weight: Theme.fontMedium
                                    }
                                    Label {
                                        text: "NG " + wheelDelegate.modelData.count
                                        color: Theme.textSoft
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontCaption
                                        font.features: Theme.fontFeaturesTabular
                                    }
                                }
                                Label {
                                    text: "待处置 " + wheelDelegate.modelData.pending
                                          + " · 已处理 " + wheelDelegate.modelData.handled
                                          + " · 误报 " + wheelDelegate.modelData.falseAlarm
                                    color: Theme.textSoft
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontCaption
                                    font.features: Theme.fontFeaturesTabular
                                }
                                RowLayout {
                                    spacing: Theme.space2
                                    Repeater {
                                        model: [{label: "已处理", kind: "handled"}, {label: "误报", kind: "falseAlarm"}]
                                        delegate: ActionButton {
                                            required property var modelData
                                            text: modelData.label
                                            variant: "secondary"
                                            implicitHeight: 28
                                            enabled: Number(wheelDelegate.modelData.pending) > 0
                                                     && root.dispositionRequestId === -1
                                            onClicked: root.disposeWheel(Number(wheelDelegate.modelData.wheel), modelData.kind,
                                                                        Number(wheelDelegate.modelData.pending))
                                        }
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            anchors.centerIn: parent
                            visible: selectedRack > 0 && wheelList.count === 0
                            spacing: 6
                            AppIcon {
                                Layout.alignment: Qt.AlignHCenter
                                name: "icon_check_circle"
                                size: 28
                                color: Theme.ok
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: "该架号全轮检测正常"
                                color: Theme.ok
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                }
            }

            // 3. 异常原图画廊
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.radiusMedium
                color: Theme.surface
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle { width: 3; height: 14; radius: Theme.radiusPill; color: Theme.borderStrong }
                        Label {
                            text: selectedWheel > 0 ? "架 #" + selectedRack + " · " + appController.wheelLabel(selectedWheel) + " 异常证据链" : "异常点检原图"
                            color: Theme.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontBody
                            font.weight: Theme.fontMedium
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.operationMessage || "操作仅处置当前日期范围内的未处置记录；已处理表示已换轮"
                        wrapMode: Text.WordWrap
                        color: Theme.textSoft
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontCaption
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: Theme.surfaceAlt
                        radius: Theme.radiusSmall
                        Image {
                            id: evidenceImage
                            anchors.fill: parent
                            anchors.margins: Theme.space2
                            source: root.currentImage.url || ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            // 按显示尺寸的 2 倍解码且不进共享缓存：逐张翻看时不再按相机原图分辨率解码（原图在点击后的查看器中打开）
                            cache: false
                            smooth: true
                            sourceSize.width: Math.min(4096, Math.max(1, width * 2))
                            sourceSize.height: Math.min(4096, Math.max(1, height * 2))
                            visible: root.imageRequestId === -1
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                enabled: evidenceImage.status === Image.Ready
                                onClicked: viewer.openViewer(root.currentImage.url, root.currentImage.name,
                                                              root.currentImage.time)
                            }
                        }
                        Label {
                            anchors.centerIn: parent
                            text: root.imageRequestId !== -1 ? "正在加载…"
                                  : (root.imageTotal === 0 ? "当前轮位没有异常图像记录"
                                     : "图片尚未到达或文件不可用")
                            visible: root.imageRequestId !== -1 || evidenceImage.status !== Image.Ready
                            color: Theme.muted
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.currentImage.name || ""
                        elide: Text.ElideMiddle
                        color: Theme.textSoft
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontCaption
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: root.currentImage.time || ""
                            color: Theme.textSoft
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontCaption
                            font.features: Theme.fontFeaturesTabular
                        }
                        Item { Layout.fillWidth: true }
                        ActionButton {
                            text: "上一幅"
                            variant: "secondary"
                            enabled: root.imagePage > 1 && root.imageRequestId === -1
                            onClicked: root.loadImagePage(root.imagePage - 1)
                        }
                        Label {
                            text: (root.imageTotal > 0 ? root.imagePage : 0) + " / " + root.imageTotal
                            color: Theme.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontCaption
                            font.features: Theme.fontFeaturesTabular
                        }
                        ActionButton {
                            text: "下一幅"
                            variant: "secondary"
                            enabled: root.imagePage < root.imageTotal && root.imageRequestId === -1
                            onClicked: root.loadImagePage(root.imagePage + 1)
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: dispositionDialog
        objectName: "dispositionDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(460, root.width - Theme.space6)
        padding: Theme.space5
        modal: true
        closePolicy: Popup.CloseOnEscape

        readonly property string actionText: root.pendingDisposition.kind === "handled" ? "已处理" : "误报"

        onAboutToShow: {
            root.operatorError = ""
            var index = root.handlerNames.indexOf(root.lastOperatorName)
            if (index < 0 && root.lastOperatorName.length === 0 && root.handlerNames.length === 1)
                index = 0
            operatorCombo.currentIndex = index
            // 上次填写的名单外姓名同样保留
            operatorCombo.editText = index >= 0 ? root.handlerNames[index] : root.lastOperatorName
        }
        onOpened: {
            operatorCombo.forceActiveFocus()
            if (operatorCombo.contentItem && operatorCombo.contentItem.selectAll)
                operatorCombo.contentItem.selectAll()
        }
        onClosed: root.pendingDisposition = ({})

        background: Rectangle {
            color: Theme.surfaceFloating
            radius: Theme.radiusLarge
            border.width: 1
            border.color: Theme.border
        }

        header: Label {
            text: "确认标记为" + dispositionDialog.actionText
            padding: Theme.space5
            bottomPadding: Theme.space2
            color: Theme.text
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontTitle
            font.weight: Theme.fontMedium
            wrapMode: Text.Wrap
        }

        contentItem: ColumnLayout {
            spacing: Theme.space3
            Label {
                Layout.fillWidth: true
                text: "架号 #" + (root.pendingDisposition.rack || "") + " · "
                      + (root.pendingDisposition.wheel ? appController.wheelLabel(root.pendingDisposition.wheel) : "")
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontBody
                font.weight: Theme.fontMedium
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: (root.pendingDisposition.start || "") + " 至 " + (root.pendingDisposition.end || "")
                      + "\n将此范围内该轮位的未处置异常记录标记为" + dispositionDialog.actionText
                      + "（当前查询 " + (root.pendingDisposition.count || 0) + " 条）。"
                color: Theme.textSoft
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontCaption
                font.features: Theme.fontFeaturesTabular
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: "处置后不可再次修改，请核对架号、轮位和日期范围。"
                color: Theme.textSoft
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Label {
                Layout.topMargin: Theme.space2
                text: "处置人员"
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontCaption
                font.weight: Theme.fontMedium
            }
            // 可编辑：下拉选择系统设置中的预设名单，名单外的人员直接输入姓名
            ComboBox {
                id: operatorCombo
                objectName: "operatorCombo"
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                editable: true
                model: root.handlerNames
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontBody
                Accessible.name: "处置人员"
                Keys.onReturnPressed: if (!popup.visible) root.confirmDisposition()
                Keys.onEnterPressed: if (!popup.visible) root.confirmDisposition()
                onEditTextChanged: if (root.operatorError.length > 0 && editText.trim().length > 0) root.operatorError = ""

                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: operatorCombo.hovered ? Theme.hover : Theme.fieldFill
                    border.width: 1
                    border.color: root.operatorError.length > 0 ? Theme.dangerBorder
                                  : ((operatorCombo.activeFocus || operatorCombo.popup.visible) ? Theme.selectedBorder
                                     : (operatorCombo.hovered ? Theme.borderStrong : Theme.border))
                }

                contentItem: TextField {
                    leftPadding: Theme.space3
                    rightPadding: root.handlerNames.length > 0 ? 36 : Theme.space3
                    text: operatorCombo.editText
                    placeholderText: "请输入或选择处置人员"
                    placeholderTextColor: Theme.muted
                    color: Theme.text
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.textOnAccent
                    font: operatorCombo.font
                    maximumLength: 32
                    selectByMouse: true
                    verticalAlignment: Text.AlignVCenter
                    inputMethodHints: Qt.ImhNoPredictiveText
                    background: Item {}
                    // 可编辑下拉框的输入区会吃掉鼠标，原来只有右侧箭头一小条能展开名单：点输入区也展开
                    onPressed: if (root.handlerNames.length > 0 && !operatorCombo.popup.visible) operatorCombo.popup.open()
                }

                // 箭头区域占满右侧 36px 高度，整块都可点击展开 / 收起名单
                indicator: Item {
                    visible: root.handlerNames.length > 0
                    x: operatorCombo.width - width
                    width: 36
                    height: operatorCombo.height

                    AppIcon {
                        anchors.centerIn: parent
                        name: "icon_arrow_down"
                        size: 12
                        color: Theme.textSoft
                    }
                }

                popup: Popup {
                    y: operatorCombo.height + 4
                    width: operatorCombo.width
                    implicitHeight: Math.min(240, contentItem.implicitHeight + 8)
                    padding: 4
                    background: Rectangle {
                        color: Theme.surfaceFloating
                        radius: Theme.radiusSmall
                        border.color: Theme.border
                        border.width: 1
                    }
                    contentItem: ListView {
                        clip: true
                        implicitHeight: contentHeight
                        model: operatorCombo.popup.visible ? operatorCombo.delegateModel : null
                        currentIndex: operatorCombo.highlightedIndex
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    }
                }

                delegate: ItemDelegate {
                    required property string modelData
                    required property int index
                    width: operatorCombo.width - 8
                    height: 34
                    highlighted: operatorCombo.highlightedIndex === index
                    contentItem: Text {
                        text: modelData
                        color: Theme.text
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontBody
                        font.weight: index === operatorCombo.currentIndex ? Theme.fontMedium : Theme.fontRegular
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: Theme.space2
                        elide: Text.ElideRight
                    }
                    background: Rectangle {
                        radius: Theme.radiusMicro
                        color: highlighted ? Theme.pressed : "transparent"
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.operatorError.length > 0
                text: root.operatorError
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: root.operatorError.length === 0
                text: root.handlerNames.length > 0 ? "可从名单中选择，名单外的人员可直接输入姓名。"
                                                   : "可直接输入姓名；常用人员可在“系统设置 → 报警处置”中预设。"
                color: Theme.muted
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }

        footer: Item {
            implicitHeight: Theme.space6 * 2

            RowLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space5
                anchors.rightMargin: Theme.space5
                spacing: Theme.space2
                Item { Layout.fillWidth: true }
                ActionButton {
                    id: cancelDispositionButton
                    text: "取消"
                    variant: "secondary"
                    onClicked: dispositionDialog.reject()
                }
                ActionButton {
                    text: "确认" + dispositionDialog.actionText
                    variant: "danger"
                    enabled: root.dispositionRequestId === -1
                    onClicked: root.confirmDisposition()
                }
            }
        }
    }

    ZoomOverlay {
        id: viewer
        anchors.fill: parent
    }

    Component.onCompleted: Qt.callLater(loadRackStatistics)
}
