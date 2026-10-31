import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: Math.min(800, parent ? parent.width - 48 : 800)
    height: Math.min(640, parent ? parent.height - 48 : 640)
    anchors.centerIn: parent
    padding: 0

    signal updateDialogRequested()

    property int currentTab: 0
    onCurrentTabChanged: settingsScroll.contentItem.contentY = 0

    readonly property var pages: [
        { label: "General", icon: "settings", title: "General", description: "Appearance, notifications, power and the clipboard" },
        { label: "Downloads", icon: "downloading", title: "Downloads", description: "Where files go, connections, and bandwidth" },
        { label: "Network", icon: "network", title: "Network", description: "Send Orbit's traffic through a proxy" },
        { label: "BitTorrent", icon: "torrent", title: "BitTorrent", description: "Torrent speed limits and when to stop seeding" },
        { label: "Media", icon: "media", title: "Media", description: "Sign-in for video sites, and keeping yt-dlp up to date" },
        { label: "Browser", icon: "extension", title: "Browser Integration", description: "Hand downloads from your browser to Orbit" },
        { label: "Schedule", icon: "clock", title: "Schedule", description: "When Orbit may download, and how fast" },
        { label: "Remote Access", icon: "globe", title: "Remote Access", description: "Check on downloads from your phone or another computer" },
        { label: "About", icon: "info", title: "About Orbit", description: "Version, updates, backups, and troubleshooting" }
    ]

    property int clockTick: 0
    Timer { interval: 30000; repeat: true; running: root.visible; onTriggered: root.clockTick++ }

    function relativeTime(when) {
        void root.clockTick
        if (!when || isNaN(when.getTime())) return ""
        const minutes = Math.floor((Date.now() - when.getTime()) / 60000)
        if (minutes < 1) return "just now"
        if (minutes < 60) return minutes + (minutes === 1 ? " minute ago" : " minutes ago")
        const hours = Math.floor(minutes / 60)
        if (hours < 24) return hours + (hours === 1 ? " hour ago" : " hours ago")
        return Qt.formatDateTime(when, "MMM d, yyyy 'at' h:mm AP")
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast }
        NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: Theme.animFast }
    }

    Overlay.modal: Rectangle {
        color: Theme.withAlpha("#000000", 0.6)
    }

    background: DialogSurface {}

    onOpened: {
        generalTab.load()
        downloadsTab.load()
        networkTab.load()
        torrentTab.load()
        scheduleTab.load()
        remoteTab.load()
        aboutTab.load()
    }

    contentItem: RowLayout {
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 196
            Layout.fillHeight: true
            Layout.leftMargin: 1
            Layout.topMargin: 1
            Layout.bottomMargin: 1
            color: Theme.surfaceInset
            radius: Theme.radiusXl - 1

            Rectangle {
                anchors.right: parent.right
                width: Theme.radiusXl
                height: parent.height
                color: parent.color
            }
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 18
                anchors.bottomMargin: 14
                spacing: 2

                Text {
                    Layout.leftMargin: 20
                    Layout.bottomMargin: 16
                    text: "Settings"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontHeading
                    font.weight: Font.DemiBold
                    font.letterSpacing: Theme.trackingTight
                }

                Repeater {
                    model: root.pages
                    SidebarItem {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        iconName: modelData.icon
                        label: modelData.label
                        selected: root.currentTab === index
                        count: -1
                        onClicked: root.currentTab = index
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 14
                Layout.topMargin: 16
                Layout.bottomMargin: 14
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: root.pages[root.currentTab].title
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                        font.letterSpacing: Theme.trackingTight
                    }
                    Text {
                        text: root.pages[root.currentTab].description
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontCaption
                    }
                }
                IconButton { iconName: "close"; tooltip: "Close"; onClicked: root.close() }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

            ScrollView {
                id: settingsScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                contentWidth: availableWidth

                ColumnLayout {
                    width: settingsScroll.availableWidth
                    spacing: 0

                    SettingsGeneralTab { id: generalTab; dialog: root; visible: root.currentTab === 0 }
                    SettingsDownloadsTab { id: downloadsTab; dialog: root; visible: root.currentTab === 1 }
                    SettingsNetworkTab { id: networkTab; dialog: root; visible: root.currentTab === 2 }
                    SettingsTorrentTab { id: torrentTab; dialog: root; visible: root.currentTab === 3 }
                    SettingsMediaTab { id: mediaTab; dialog: root; visible: root.currentTab === 4 }
                    SettingsBrowserTab { id: browserTab; dialog: root; visible: root.currentTab === 5 }
                    SettingsScheduleTab { id: scheduleTab; dialog: root; visible: root.currentTab === 6 }
                    SettingsRemoteTab { id: remoteTab; dialog: root; visible: root.currentTab === 7 }
                    SettingsAboutTab { id: aboutTab; dialog: root; visible: root.currentTab === 8 }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 16
                Layout.topMargin: 12
                Layout.bottomMargin: 12

                Text {
                    Layout.fillWidth: true
                    text: "Changes are saved automatically"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                }
                ModernButton { text: "Done"; variant: "primary"; onClicked: root.close() }
            }
        }
    }
}
