import QtQuick
import QtQuick.Layouts

// General settings: appearance, notifications, power and the clipboard.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    // Fills the fields from the current settings; called each time the dialog opens.
    function load() {
        clipboardExtensionsField.text = clipboardMonitor.extensions
    }

    SettingsGroup {
        title: "Appearance"

        SettingsRow {
            divider: false
            label: "Theme"
            description: Prefs.themeMode === "system"
                         ? (systemAppearance.colorScheme === "" ? "Your desktop doesn't say, so Orbit stays " + (Theme.dark ? "dark" : "light")
                                                               : "Follows your desktop (" + systemAppearance.colorScheme + " right now)")
                         : "The same look whatever the desktop uses"
            SegmentedControl {
                options: [{ label: "Dark", value: "dark" }, { label: "Light", value: "light" }, { label: "System", value: "system" }]
                value: Prefs.themeMode
                onActivated: (v) => Prefs.themeMode = v
            }
        }
        SettingsRow {
            label: "Accent colour"
            description: Prefs.accent === "system" ? "Your desktop's accent colour"
                         : (Theme.accentPalette.find(a => a.key === Prefs.accent) || Theme.accentPalette[0]).label
            Row {
                spacing: 8
                Repeater {
                    model: Theme.accentPalette.concat(systemAppearance.hasAccentColor
                                                      ? [{ key: "system", label: "Desktop accent", color: Theme.readableAccent(systemAppearance.accentColor) }]
                                                      : [])
                    delegate: Rectangle {
                        id: swatch
                        required property var modelData
                        readonly property bool chosen: Prefs.accent === modelData.key
                        width: 22
                        height: 22
                        radius: 11
                        color: modelData.color
                        border.width: chosen ? 2 : 1
                        border.color: chosen ? Theme.textPrimary : Theme.withAlpha(Theme.textPrimary, 0.12)
                        scale: swatchMouse.containsMouse && !chosen ? 1.08 : 1
                        Behavior on scale { NumberAnimation { duration: Theme.animFast; easing.type: Theme.easeStandard } }
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.label
                        Accessible.checked: chosen

                        // The desktop's accent is marked so it isn't mistaken for a fixed colour.
                        Icon {
                            anchors.centerIn: parent
                            visible: swatch.modelData.key === "system"
                            name: "settings"
                            iconSize: 11
                            color: "#ffffff"
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            visible: swatch.chosen && swatch.modelData.key !== "system"
                            width: 6
                            height: 6
                            radius: 3
                            color: "#ffffff"
                        }
                        MouseArea {
                            id: swatchMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Prefs.accent = swatch.modelData.key
                        }
                        OrbitToolTip {
                            visible: swatchMouse.containsMouse
                            text: swatch.modelData.label
                        }
                    }
                }
            }
        }
        SettingsRow {
            label: "Download list"
            description: Prefs.density === "compact" ? "Shorter rows, more downloads on screen" : "Roomy rows with a larger file preview"
            SegmentedControl {
                options: [{ label: "Comfortable", value: "comfortable" }, { label: "Compact", value: "compact" }]
                value: Prefs.density
                onActivated: (v) => Prefs.density = v
            }
        }
        SettingsRow {
            visible: !Theme.windows
            label: "Translucent window"
            description: "The desktop shows through the sidebar and background. KDE Plasma and Hyprland blur it; other desktops show it unblurred"
            ToggleSwitch {
                checked: Prefs.translucent
                onToggled: Prefs.translucent = checked
            }
        }
    }

    SettingsGroup {
        title: "Notifications"

        SettingsRow {
            divider: false
            label: "Notify when downloads finish"
            description: "Shows a " + (Theme.windows ? "Windows " : "desktop ") + "notification when a download completes or fails while Orbit is in the background"
            ToggleSwitch {
                checked: Prefs.notifyOnFinish
                onToggled: Prefs.notifyOnFinish = checked
            }
        }
    }

    SettingsGroup {
        title: "Power"

        SettingsRow {
            divider: false
            label: "Keep the PC awake while downloading"
            description: (Theme.windows ? "Windows" : "Your PC") + " won't go to sleep while a download is running"
            ToggleSwitch {
                checked: powerManager.preventSleep
                onToggled: powerManager.preventSleep = checked
            }
        }
        SettingsRow {
            label: "When downloads finish"
            description: "Runs once after everything that's downloading or queued is done, with 30 seconds to cancel"
            SegmentedControl {
                options: {
                    const list = [{ label: "Nothing", value: "none" }, { label: "Exit", value: "exit" }, { label: "Sleep", value: "sleep" }]
                    if (powerManager.hibernateAvailable) list.push({ label: "Hibernate", value: "hibernate" })
                    list.push({ label: "Shut down", value: "shutdown" })
                    return list
                }
                value: powerManager.whenDoneAction
                onActivated: (v) => powerManager.whenDoneAction = v
            }
        }
    }

    SettingsGroup {
        title: "Clipboard"

        SettingsRow {
            divider: false
            label: "Offer to download copied links"
            description: "When you copy a magnet link or a link to a file in another app, Orbit offers to download it"
            ToggleSwitch {
                checked: clipboardMonitor.enabled
                onToggled: clipboardMonitor.enabled = checked
            }
        }
        SettingsRow {
            label: "File types"
            description: "Extensions that count as a download link"
            dimmed: !clipboardMonitor.enabled
            enabled: clipboardMonitor.enabled
            extra: FormField {
                id: clipboardExtensionsField
                Layout.fillWidth: true
                monospace: true
                onEditingFinished: clipboardMonitor.extensions = text
            }
        }
        SettingsRow {
            label: "Include video page links"
            description: "YouTube, TikTok, Twitch and similar"
            dimmed: !clipboardMonitor.enabled
            enabled: clipboardMonitor.enabled
            ToggleSwitch {
                checked: clipboardMonitor.catchMedia
                onToggled: clipboardMonitor.catchMedia = checked
            }
        }
    }
}
