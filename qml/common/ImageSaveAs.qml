import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Dialogs
import ".."

// 图像“另存为”：弹出系统保存对话框，由 AppController 在后台线程复制或转码原图，
// 完成后在所在窗口底部显示结果提示。放在图像窗口内并铺满其图像区。
Item {
    id: root

    property string sourceUrl: ""
    property string suggestedName: ""
    property bool busy: false
    property int pendingRequest: -1

    readonly property bool available: typeof appController !== "undefined" && appController
                                      && typeof appController.saveImageAs === "function"
    readonly property bool canSave: available && !busy && sourceUrl.length > 0

    function baseName(url) {
        var s = String(url || "")
        var q = s.indexOf("?")
        if (q >= 0) s = s.substring(0, q)
        s = s.replace(/\\/g, "/")
        var name = s.substring(s.lastIndexOf("/") + 1)
        try { name = decodeURIComponent(name) } catch (e) {}
        return name
    }

    function suffixOf(name) {
        var dot = name.lastIndexOf(".")
        return dot > 0 ? name.substring(dot + 1).toLowerCase() : ""
    }

    function filterFor(suffix) {
        switch (suffix) {
        case "png": return "PNG 图像 (*.png)"
        case "jpg": return "JPEG 图像 (*.jpg *.jpeg)"
        case "bmp": return "BMP 图像 (*.bmp)"
        case "tiff": return "TIFF 图像 (*.tif *.tiff)"
        default: return suffix.toUpperCase() + " 图像 (*." + suffix + ")"
        }
    }

    function open() {
        if (!canSave) return
        var name = suggestedName.length > 0 ? suggestedName : baseName(sourceUrl)
        if (name.length === 0) name = "image.png"
        var suffixes = appController.imageSaveSuffixes(sourceUrl)
        var filters = []
        for (var i = 0; i < suffixes.length; ++i) {
            var f = filterFor(suffixes[i])
            filters.push(i === 0 ? f.replace(" 图像", " 图像（原格式，无损）") : f)
        }
        saveDialog.nameFilters = filters.length > 0 ? filters : ["所有文件 (*)"]
        saveDialog.defaultSuffix = suffixes.length > 0 ? suffixes[0] : suffixOf(name)
        var folder = appController.imageSaveFolderUrl()
        if (folder.length > 0) {
            saveDialog.currentFolder = folder
            saveDialog.selectedFile = folder + "/" + encodeURIComponent(name)
        }
        saveDialog.open()
    }

    function showToast(ok, text, path) {
        toast.show(ok, text, path)
    }

    FileDialog {
        id: saveDialog
        title: "图像另存为"
        fileMode: FileDialog.SaveFile
        onAccepted: {
            var target = String(selectedFile)
            if (target.length === 0) return
            root.busy = true
            root.pendingRequest = appController.saveImageAs(root.sourceUrl, target)
        }
    }

    Connections {
        target: root.available ? appController : null
        ignoreUnknownSignals: true
        function onImageSaveFinished(requestId, ok, message, targetPath) {
            if (requestId !== root.pendingRequest) return
            root.busy = false
            root.pendingRequest = -1
            root.showToast(ok, message, targetPath)
        }
    }

    // 结果提示条（底部居中），位于图像信息条上方，窄窗口下也不与其重叠
    ResultToast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space6 + Theme.space5
    }
}
