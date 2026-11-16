import QtQuick 2.15
import QtQuick.Layouts 1.15
import ".."

// 操作结果提示条：成功时可"打开文件夹"定位生成的文件，数秒后自动淡出，悬停时暂停计时。
// 由使用方设置锚点（通常位于所在区域底部居中），调用 show() 显示。
Rectangle {
    id: toast

    property bool ok: true
    property string message: ""
    property string path: ""

    function show(success, text, filePath) {
        ok = success
        message = text
        path = filePath || ""
        opacity = 1.0
        hideTimer.interval = success ? 4000 : 6000
        hideTimer.restart()
    }

    function hide() {
        opacity = 0.0
    }

    width: Math.min(parent ? parent.width - Theme.space6 * 2 : implicitWidth, toastRow.implicitWidth + Theme.space4 * 2)
    height: 40
    radius: Theme.radiusSmall
    color: Theme.surfaceFloating
    border.width: 1
    border.color: toast.ok ? Theme.okBorder : Theme.warnBorder
    opacity: 0.0
    visible: opacity > 0.0
    Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }

    Timer {
        id: hideTimer
        repeat: false
        onTriggered: toast.hide()
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onEntered: hideTimer.stop()
        onExited: hideTimer.restart()
    }

    RowLayout {
        id: toastRow
        anchors.fill: parent
        anchors.leftMargin: Theme.space4
        anchors.rightMargin: Theme.space2
        spacing: Theme.space2

        AppIcon {
            name: toast.ok ? "icon_check_circle" : "icon_warning"
            size: 16
            color: toast.ok ? Theme.ok : Theme.warn
        }

        Text {
            Layout.fillWidth: true
            text: toast.message
            color: Theme.text
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideMiddle
        }

        IconButton {
            visible: toast.ok && toast.path.length > 0
            Layout.preferredHeight: 28
            bordered: false
            iconName: "icon_folder"
            iconSize: 14
            text: "打开文件夹"
            onClicked: {
                appController.revealFileInFolder(toast.path)
                toast.hide()
            }
        }

        IconButton {
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            bordered: false
            iconName: "icon_close"
            iconSize: 12
            toolTipText: "关闭提示"
            onClicked: toast.hide()
        }
    }
}
