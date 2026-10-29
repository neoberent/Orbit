import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    readonly property var summary: downloadModel.summary
    property var speedHistory: null

    readonly property string countsText: {
        const s = summary
        const parts = []
        if (s.active > 0) parts.push(s.active + " downloading")
        if (s.seeding > 0) parts.push(s.seeding + " seeding")
        if (s.queued > 0) parts.push(s.queued + " queued")
        if (s.paused > 0) parts.push(s.paused + " paused")
        if (s.errors > 0) parts.push("<font color=\"" + Theme.danger + "\">" + s.errors + " failed</font>")
        if (parts.length === 0) return downloadModel.count > 0 ? "All transfers idle" : "No transfers"
        return parts.join("  ·  ")
    }

    color: Theme.statusBar
    implicitHeight: Theme.statusBarHeight

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

    component Divider: Rectangle {
        Layout.preferredWidth: 1
        Layout.preferredHeight: 12
        color: Theme.border
    }

    component Meter: RowLayout {
        property string iconName
        property string value
        property bool live: false
        property color liveColor: Theme.accent
        spacing: 5
        Icon {
            name: parent.iconName
            iconSize: 11
            color: parent.live ? parent.liveColor : Theme.textTertiary
        }
        Text {
            text: parent.value
            color: parent.live ? Theme.textPrimary : Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            font.weight: Font.Medium
            font.features: { "tnum": 1 }
        }
    }

    component Note: RowLayout {
        property string iconName: ""
        property string text
        property bool showDot: false
        property color dotColor: Theme.textTertiary
        spacing: 6
        Rectangle {
            visible: parent.showDot
            Layout.preferredWidth: 6
            Layout.preferredHeight: 6
            radius: 3
            color: parent.dotColor
        }
        Icon {
            visible: parent.iconName.length > 0
            name: parent.iconName
            iconSize: 11
            color: Theme.textTertiary
        }
        Text {
            text: parent.text
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.topMargin: 1
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 14

        Meter {
            iconName: "arrowDown"
            value: root.summary.downloadRateText
            live: root.summary.downloading
        }
        SpeedGraph {
            Layout.preferredWidth: 64
            Layout.preferredHeight: 14
            visible: root.speedHistory !== null
            samples: root.speedHistory ? root.speedHistory.total.slice(-60) : []
            capacity: 60
            showGrid: false
            lineWidth: 1.2
            fillAlpha: 0.2
            lineColor: root.summary.downloading ? Theme.accent : Theme.textTertiary
        }
        Meter {
            iconName: "arrowUp"
            value: root.summary.uploadRateText
            live: root.summary.uploading
            liveColor: Theme.seeding
        }

        Divider {}

        Text {
            Layout.fillWidth: true
            text: root.countsText
            textFormat: Text.StyledText
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }

        Note {
            visible: appController.speedRuleActive
            iconName: "clock"
            text: "Scheduled limit " + appController.speedRuleLimitKBps + " KB/s"
        }
        Note {
            visible: !appController.speedRuleActive && appController.httpSpeedLimitKBps > 0
            iconName: "downloading"
            text: "Limit " + appController.httpSpeedLimitKBps + " KB/s"
        }
        Note {
            visible: appController.scheduleEnabled
            iconName: "clock"
            text: appController.scheduleStartTime + "–" + appController.scheduleEndTime
        }
        Note {
            visible: appController.proxyType !== "none"
            iconName: "network"
            text: (appController.proxyType === "socks5" ? "SOCKS5" : "HTTP") + " proxy"
        }
        Note {
            visible: appController.remoteAccessEnabled
            showDot: true
            dotColor: appController.remoteRunning ? Theme.success : Theme.danger
            text: appController.remoteRunning ? "Remote access on :" + appController.remotePort : "Remote access offline"
        }

        Divider {}

        Rectangle {
            id: whenDoneButton
            Layout.preferredHeight: 20
            Layout.preferredWidth: whenDoneRow.implicitWidth + 12
            radius: Theme.radiusSmall
            color: whenDoneMouse.containsMouse || whenDoneMenu.visible ? Theme.surfaceHover : "transparent"
            readonly property bool armed: powerManager.whenDoneAction !== "none"

            Accessible.role: Accessible.Button
            Accessible.name: "When downloads finish: " + powerManager.actionLabel(powerManager.whenDoneAction)
            Accessible.onPressAction: whenDoneMenu.popup(whenDoneButton, 0, -whenDoneMenu.implicitHeight - 4)

            Row {
                id: whenDoneRow
                anchors.centerIn: parent
                spacing: 5
                Icon {
                    name: "power"
                    iconSize: 11
                    color: whenDoneButton.armed ? Theme.warning : (powerManager.sleepBlocked ? Theme.textSecondary : Theme.textTertiary)
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    visible: whenDoneButton.armed || powerManager.sleepBlocked
                    text: whenDoneButton.armed ? "Then " + powerManager.actionLabel(powerManager.whenDoneAction).replace(/^\w/, c => c.toLowerCase()) : "Staying awake"
                    color: whenDoneButton.armed ? Theme.textPrimary : Theme.textSecondary
                    font.pixelSize: Theme.fontCaption
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: whenDoneMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: whenDoneMenu.popup(whenDoneButton, 0, -whenDoneMenu.implicitHeight - 4)
            }
            OrbitToolTip {
                visible: whenDoneMouse.containsMouse && !whenDoneMenu.visible
                text: powerManager.sleepBlocked ? "Keeping the PC awake while downloads run. Click to choose what happens when they finish."
                                                : "Choose what happens when downloads finish"
            }
        }
    }

    OrbitMenu {
        id: whenDoneMenu
        Instantiator {
            model: {
                const list = [{ key: "none", label: "Do Nothing" }, { key: "exit", label: "Exit Orbit" }, { key: "sleep", label: "Sleep" }]
                if (powerManager.hibernateAvailable) list.push({ key: "hibernate", label: "Hibernate" })
                list.push({ key: "shutdown", label: "Shut Down" })
                return list
            }
            delegate: OrbitMenuItem {
                required property var modelData
                text: modelData.label
                iconName: "check"
                iconHidden: powerManager.whenDoneAction !== modelData.key
                onTriggered: powerManager.whenDoneAction = modelData.key
            }
            onObjectAdded: (index, object) => whenDoneMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => whenDoneMenu.removeItem(object)
        }
    }
}
