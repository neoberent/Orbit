import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: ""
    modal: true
    closePolicy: Popup.CloseOnEscape
    standardButtons: Dialog.NoButton
    width: 520
    anchors.centerIn: parent
    padding: 0

    signal refreshed(string message)

    property var taskId: 0
    property string taskName: ""
    property string currentLink: ""
    property real knownSize: 0
    property string knownSizeText: ""
    property real downloaded: 0
    property real progress: 0
    property bool captured: false
    property bool capturedWithSession: false
    property string errorText: ""

    property int probeToken: -1
    property string probedUrl: ""
    property var probe: null

    readonly property string link: linkField.text.trim()
    readonly property bool validLink: /^https?:\/\/\S+$/i.test(link)
    readonly property bool checking: probeToken >= 0
    readonly property bool probeCurrent: probe !== null && probedUrl === link
    readonly property bool hasProgress: downloaded > 0
    readonly property bool compatible: probeCurrent && probe.ok && knownSize > 0 && probe.size === knownSize && probe.supportsRanges
    readonly property bool startsOver: hasProgress && probeCurrent && probe.ok && !compatible

    function openFor(item) {
        taskId = item.taskId
        taskName = item.name
        currentLink = item.source
        knownSize = item.totalBytes
        knownSizeText = item.totalText
        // A cancelled download's partial file is gone, so there's nothing to continue from.
        downloaded = item.stateLabel === "Cancelled" ? 0 : item.downloadedBytes
        progress = item.progress
        captured = false
        capturedWithSession = false
        errorText = ""
        probe = null
        probedUrl = ""
        probeToken = -1
        linkField.text = ""
        appController.beginLinkCapture(taskId)
        open()
    }

    function runProbe() {
        probeTimer.stop()
        if (!validLink) { probeToken = -1; return }
        if (probeCurrent || (checking && probedUrl === link)) return
        probedUrl = link
        probe = null
        probeToken = appController.probeLink(taskId, link)
    }

    function verdict() {
        if (!validLink) return { tone: Theme.textTertiary, icon: "info", text: "" }
        if (checking || !probeCurrent) return { tone: Theme.textTertiary, icon: "clock", text: "Checking the new link…" }
        if (!probe.ok) return { tone: Theme.danger, icon: "alert", text: "The server didn't answer as expected (" + (probe.error || "unknown error")
                                                                          + "). You can still try this link." }
        if (!hasProgress) return { tone: Theme.success, icon: "check", text: "The link works (" + probe.sizeText + "). The download will use it from the start." }
        if (compatible) return { tone: Theme.success, icon: "check", text: "Same file (" + probe.sizeText + "). The download continues from "
                                                                            + Theme.percentText(progress) + "." }
        if (knownSize <= 0) return { tone: Theme.warning, icon: "alert", text: "The original size was never known, so the download starts over with this link." }
        if (probe.size !== knownSize) return { tone: Theme.warning, icon: "alert", text: "This link is a different file (" + probe.sizeText + " instead of "
                                                                                          + root.knownSizeText + "). The download starts over." }
        return { tone: Theme.warning, icon: "alert", text: "This server can't continue partial downloads, so the download starts over." }
    }

    function apply() {
        if (!validLink) return
        const error = appController.replaceLink(taskId, link, startsOver)
        if (error.length > 0) { errorText = error; return }
        const name = taskName
        close()
        refreshed(startsOver ? "“" + name + "” starts over with the new link" : "“" + name + "” continues with the new link")
    }

    onClosed: appController.endLinkCapture()

    Timer { id: probeTimer; interval: 500; onTriggered: root.runProbe() }

    Connections {
        target: appController
        function onLinkProbed(token, result) {
            if (token !== root.probeToken) return
            root.probeToken = -1
            root.probe = result
        }
        function onLinkCaptured(id, url, withSession) {
            if (!root.visible || id !== root.taskId) return
            root.captured = true
            root.capturedWithSession = withSession
            linkField.text = url
            root.runProbe()
        }
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast } }

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }
    background: DialogSurface {}

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Layout.bottomMargin: 14
            spacing: 12
            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.radiusMedium
                color: Theme.withAlpha(Theme.accent, 0.12)
                border.width: 1
                border.color: Theme.withAlpha(Theme.accent, 0.22)
                Icon { anchors.centerIn: parent; name: "link"; iconSize: 15; color: Theme.accent }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { text: "Refresh Link"; color: Theme.textPrimary; font.pixelSize: Theme.fontHeading + 1; font.weight: Font.DemiBold }
                Text {
                    Layout.fillWidth: true
                    text: root.taskName
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideMiddle
                }
            }
            IconButton { iconName: "close"; tooltip: "Close"; onClicked: root.close() }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            spacing: 6

            Text { text: "Current link"; color: Theme.textSecondary; font.pixelSize: Theme.fontCaption; font.weight: Font.Medium }
            Text {
                Layout.fillWidth: true
                text: root.currentLink
                color: Theme.textTertiary
                font.pixelSize: Theme.fontCaption
                font.family: Theme.monoFamily
                elide: Text.ElideMiddle
            }

            Text {
                Layout.topMargin: 10
                text: "New link"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                font.weight: Font.Medium
            }
            FormField {
                id: linkField
                Layout.fillWidth: true
                iconName: "link"
                placeholderText: "Paste the new link here"
                accessibleLabel: "New link"
                field.onTextChanged: {
                    root.errorText = ""
                    if (root.link !== root.probedUrl) root.captured = false
                    probeTimer.restart()
                }
                field.Keys.onReturnPressed: root.apply()
                field.Keys.onEnterPressed: root.apply()
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 6
                visible: !root.validLink
                implicitHeight: hint.implicitHeight + 20
                radius: Theme.radiusMedium
                color: Theme.withAlpha(Theme.accent, 0.06)
                border.width: 1
                border.color: Theme.withAlpha(Theme.accent, 0.16)
                RowLayout {
                    id: hint
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10
                    Icon { Layout.alignment: Qt.AlignTop; name: "extension"; iconSize: 14; color: Theme.accent }
                    Text {
                        Layout.fillWidth: true
                        text: "Or start the download again in your browser. While this window is open, Orbit catches the link from the Orbit extension, together with your sign-in."
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                        lineHeight: 1.15
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                visible: root.validLink
                spacing: 8
                Icon {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 1
                    name: root.verdict().icon
                    iconSize: 13
                    color: root.verdict().tone
                }
                Text {
                    Layout.fillWidth: true
                    text: root.verdict().text
                          + (root.captured ? (root.capturedWithSession ? " Caught from your browser, signed in." : " Caught from your browser.") : "")
                    color: root.verdict().tone === Theme.textTertiary ? Theme.textTertiary : Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            Text {
                visible: root.errorText.length > 0
                Layout.fillWidth: true
                text: root.errorText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }

        Item { Layout.preferredHeight: 18 }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.leftMargin: 20
            spacing: 8
            Item { Layout.fillWidth: true }
            ModernButton { text: "Cancel"; variant: "ghost"; onClicked: root.close() }
            ModernButton {
                text: !root.hasProgress ? "Use Link"
                      : root.startsOver ? "Start Over"
                      : "Use Link and Resume"
                variant: root.startsOver ? "dangerSolid" : "primary"
                enabled: root.validLink && root.probeCurrent
                onClicked: root.apply()
            }
        }
    }

    onOpened: linkField.field.forceActiveFocus()
}
