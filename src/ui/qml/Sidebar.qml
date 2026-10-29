import QtQuick
import QtQuick.Layouts

Item {
    id: root

    property string selectedCategory: "all"
    signal categorySelected(string category)
    signal settingsRequested()

    Rectangle {
        anchors.fill: parent
        color: Theme.sidebar
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.bottomMargin: Theme.spaceMd
        spacing: 2

        Text {
            Layout.leftMargin: 20
            Layout.topMargin: Theme.spaceXl - 2
            Layout.bottomMargin: 6
            text: "LIBRARY"
            color: Theme.textTertiary
            font.pixelSize: Theme.fontMicro
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.trackingWider
        }

        SidebarItem {
            Layout.fillWidth: true
            iconName: "all"
            label: "All Downloads"
            count: downloadModel.revision >= 0 ? downloadModel.countByCategory("all") : 0
            selected: root.selectedCategory === "all"
            onClicked: root.categorySelected("all")
        }
        SidebarItem {
            Layout.fillWidth: true
            iconName: "downloading"
            label: "Downloading"
            count: downloadModel.revision >= 0 ? downloadModel.countByCategory("downloading") : 0
            selected: root.selectedCategory === "downloading"
            onClicked: root.categorySelected("downloading")
        }
        SidebarItem {
            Layout.fillWidth: true
            iconName: "completed"
            label: "Completed"
            count: downloadModel.revision >= 0 ? downloadModel.countByCategory("completed") : 0
            selected: root.selectedCategory === "completed"
            onClicked: root.categorySelected("completed")
        }
        SidebarItem {
            Layout.fillWidth: true
            iconName: "torrent"
            label: "Torrents"
            count: downloadModel.revision >= 0 ? downloadModel.countByCategory("torrents") : 0
            selected: root.selectedCategory === "torrents"
            onClicked: root.categorySelected("torrents")
        }
        SidebarItem {
            Layout.fillWidth: true
            iconName: "media"
            label: "Media"
            count: downloadModel.revision >= 0 ? downloadModel.countByCategory("media") : 0
            selected: root.selectedCategory === "media"
            onClicked: root.categorySelected("media")
        }

        Item { Layout.fillHeight: true }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.bottomMargin: 7
            height: 1
            color: Theme.border
        }

        SidebarItem {
            Layout.fillWidth: true
            iconName: "settings"
            label: "Settings"
            count: -1
            selected: false
            onClicked: root.settingsRequested()
        }
    }
}
