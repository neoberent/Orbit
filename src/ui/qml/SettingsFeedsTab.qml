import QtQuick
import QtQuick.Layouts

// Feeds: RSS/Atom feeds whose new matching torrents download by themselves.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    property string message: ""
    property bool messageOk: true
    property string folder: ""

    function load() {
        urlField.text = ""
        filterField.text = ""
        tab.folder = ""
        tab.message = ""
    }

    function add() {
        const error = feedWatcher.addFeed(urlField.text, filterField.text, tab.folder)
        tab.messageOk = error.length === 0
        tab.message = error.length > 0 ? error : "Added. Its current items count as seen; new ones that match will download."
        if (error.length === 0) { urlField.text = ""; filterField.text = ""; tab.folder = "" }
    }

    SettingsGroup {
        title: "Add a feed"
        description: "Checked every 30 minutes, through the same proxy and network interface as downloads."

        SettingsRow {
            divider: false
            label: "Feed address"
            extra: FormField {
                id: urlField
                Layout.fillWidth: true
                monospace: true
                iconName: "link"
                accessibleLabel: "Feed address"
                placeholderText: "https://example.org/rss"
            }
        }
        SettingsRow {
            label: "Only titles with"
            description: "Words that must all appear (-word excludes), or /a regular expression/. Empty takes every new item."
            extra: FormField {
                id: filterField
                Layout.fillWidth: true
                accessibleLabel: "Filter"
                placeholderText: "e.g. 1080p -cam"
            }
        }
        SettingsRow {
            label: "Save into"
            description: tab.folder.length > 0 ? tab.folder : "Your download folder"
            ModernButton {
                text: "Choose…"
                variant: "secondary"
                onClicked: {
                    const dir = appController.pickFolder("Save this feed's downloads in", tab.folder)
                    if (dir.length > 0) tab.folder = dir
                }
            }
        }
        SettingsRow {
            label: " "
            Text {
                visible: tab.message.length > 0
                Layout.maximumWidth: 380
                text: tab.message
                wrapMode: Text.Wrap
                color: tab.messageOk ? Theme.success : Theme.danger
                font.pixelSize: Theme.fontCaption
            }
            ModernButton { text: "Add Feed"; variant: "primary"; enabled: urlField.text.trim().length > 0; onClicked: tab.add() }
        }
    }

    SettingsGroup {
        title: "Your feeds"
        description: feedWatcher.feeds.length === 0 ? "None yet." : ""

        Repeater {
            model: feedWatcher.feeds
            delegate: SettingsRow {
                required property var modelData
                required property int index
                divider: index > 0
                label: modelData.name
                description: (modelData.filter.length > 0 ? "“" + modelData.filter + "”  ·  " : "Every new item  ·  ") +
                             (modelData.lastError.length > 0 ? modelData.lastError
                              : modelData.lastChecked > 0 ? modelData.matched + " downloaded" : "Not checked yet")
                ToggleSwitch {
                    checked: modelData.enabled
                    onToggled: feedWatcher.setFeedEnabled(modelData.url, checked)
                }
                IconButton {
                    iconName: "trash"
                    danger: true
                    tooltip: "Remove feed"
                    onClicked: feedWatcher.removeFeed(modelData.url)
                }
            }
        }
        SettingsRow {
            visible: feedWatcher.feeds.length > 0
            label: " "
            ModernButton { text: "Check Now"; variant: "secondary"; iconName: "retry"; onClicked: feedWatcher.checkNow() }
        }
    }
}
