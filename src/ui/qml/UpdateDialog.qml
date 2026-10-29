import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: 480
    anchors.centerIn: parent
    padding: 0

    property bool installWhenReady: false

    readonly property string phase: updateManager.state
    readonly property bool downloading: phase === "downloading"
    readonly property bool ready: phase === "ready"
    readonly property bool failed: phase === "error"
    readonly property real fraction: updateManager.downloadSize > 0 ? updateManager.bytesReceived / updateManager.downloadSize : 0

    readonly property bool portable: appController.portable

    function startUpdate() {
        // The installer updates the installed copy in %LocalAppData%, not a portable
        // folder, so portable users get the download page instead.
        if (portable) {
            updateManager.openReleasePage()
            close()
            return
        }
        installWhenReady = true
        if (ready) updateManager.installUpdate()
        else updateManager.downloadUpdate()
    }

    Connections {
        target: updateManager
        function onStateChanged() {
            if (updateManager.state !== "ready" || !root.installWhenReady) return
            if (root.visible) updateManager.installUpdate()
            else root.open()
        }
    }

    onClosed: if (!downloading) installWhenReady = false

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast }
    }

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }

    background: DialogSurface {}

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Layout.bottomMargin: 16
            spacing: 14

            Image {
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                source: "qrc:/images/logo.png"
                sourceSize: Qt.size(80, 80)
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    Layout.fillWidth: true
                    text: root.ready ? "Update ready to install" : "A new version of Orbit is available"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontHeading + 1
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                RowLayout {
                    spacing: 8
                    Text {
                        text: updateManager.currentVersion
                        color: Theme.textTertiary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    Icon {
                        name: "arrowDown"
                        rotation: -90
                        iconSize: 11
                        color: Theme.textTertiary
                    }
                    Text {
                        text: updateManager.latestVersion
                        color: Theme.accent
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontCaption
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: updateManager.releaseDate.length > 0
                        text: "·  Released " + updateManager.releaseDate
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.bottomMargin: 18
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "WHAT'S NEW"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontMicro
                    font.weight: Font.DemiBold
                    font.letterSpacing: Theme.trackingWider
                }
                Text {
                    visible: updateManager.releasePage.length > 0
                    text: "Full release notes"
                    color: notesLink.containsMouse ? Theme.accentHover : Theme.accent
                    font.pixelSize: Theme.fontCaption
                    MouseArea {
                        id: notesLink
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: updateManager.openReleasePage()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(214, notesColumn.implicitHeight + 26)
                radius: Theme.radiusMedium
                color: Theme.surfaceInset
                border.width: 1
                border.color: Theme.border

                Flickable {
                    id: notesFlick
                    anchors.fill: parent
                    anchors.margins: 12
                    contentHeight: notesColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: notesFlick.contentHeight > notesFlick.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

                    ColumnLayout {
                        id: notesColumn
                        width: notesFlick.width - 8
                        spacing: 7

                        Repeater {
                            model: updateManager.releaseNotes
                            delegate: RowLayout {
                                required property string modelData
                                Layout.fillWidth: true
                                spacing: 10
                                Rectangle {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.topMargin: 6
                                    Layout.preferredWidth: 4
                                    Layout.preferredHeight: 4
                                    radius: 2
                                    color: Theme.accent
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontBody
                                    wrapMode: Text.Wrap
                                    lineHeight: 1.1
                                }
                            }
                        }

                        Text {
                            visible: updateManager.releaseNotes.length === 0
                            Layout.fillWidth: true
                            text: "This release includes improvements and fixes."
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            ColumnLayout {
                visible: root.downloading || root.ready
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: 7

                ProgressIndicator {
                    Layout.fillWidth: true
                    height: 3
                    value: root.ready ? 1 : root.fraction
                    indeterminate: root.downloading && updateManager.downloadSize <= 0
                    fillColor: root.ready ? Theme.success : Theme.accent
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Icon {
                        visible: root.ready
                        name: "check"
                        iconSize: 12
                        color: Theme.success
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.ready
                              ? "Downloaded and verified"
                              : Theme.formatBytes(updateManager.bytesReceived)
                                + (updateManager.downloadSize > 0 ? " of " + Theme.formatBytes(updateManager.downloadSize) : "")
                        color: root.ready ? Theme.success : Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                        font.features: { "tnum": 1 }
                    }
                    Text {
                        visible: root.downloading && updateManager.downloadRate > 0
                        text: Theme.formatRate(updateManager.downloadRate)
                        color: Theme.textTertiary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontMicro
                    }
                }
            }

            Text {
                visible: !root.failed
                Layout.fillWidth: true
                Layout.topMargin: 4
                text: root.downloading ? "You can keep using Orbit while the update downloads."
                                       : "Orbit closes while the update installs and reopens when it's done."
                color: Theme.textTertiary
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            RowLayout {
                visible: root.failed
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 8
                Icon {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 1
                    name: "alert"
                    iconSize: 13
                    color: Theme.danger
                }
                Text {
                    Layout.fillWidth: true
                    text: updateManager.errorMessage
                    color: Theme.danger
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.leftMargin: 12
            spacing: 8

            ModernButton {
                visible: !root.downloading && !root.ready
                text: "Skip Version"
                variant: "ghost"
                onClicked: {
                    updateManager.skipThisVersion()
                    root.close()
                }
            }
            Item { Layout.fillWidth: true }

            ModernButton {
                visible: root.downloading
                text: "Cancel"
                variant: "ghost"
                onClicked: {
                    root.installWhenReady = false
                    updateManager.cancelDownload()
                }
            }
            ModernButton {
                text: root.downloading ? "Hide" : "Later"
                variant: "secondary"
                onClicked: root.close()
            }
            ModernButton {
                id: primaryButton
                enabled: !root.downloading
                text: root.downloading ? "Downloading " + Math.floor(root.fraction * 100) + "%"
                      : root.portable ? "Open Download Page"
                      : root.ready ? "Restart & Install"
                      : root.failed ? "Try Again"
                      : "Download & Install"
                iconName: root.downloading ? "" : root.ready ? "retry" : "arrowDown"
                variant: "primary"
                onClicked: root.startUpdate()
            }
        }
    }

    onOpened: if (primaryButton.enabled) primaryButton.forceActiveFocus()
}
