#pragma once

#include <QColor>
#include <QObject>

// What the desktop asks apps to look like: light or dark, and its accent colour. On Linux
// it comes from the settings portal (org.freedesktop.appearance), which GNOME, Plasma and
// most shells provide; elsewhere from Qt's style hints and palette. Changes arrive live.
class SystemAppearance final : public QObject {
    Q_OBJECT
    // "dark", "light", or "" when the desktop has no preference.
    Q_PROPERTY(QString colorScheme READ colorScheme NOTIFY changed)
    Q_PROPERTY(QColor accentColor READ accentColor NOTIFY changed)
    Q_PROPERTY(bool hasAccentColor READ hasAccentColor NOTIFY changed)

public:
    explicit SystemAppearance(QObject* parent = nullptr);

    [[nodiscard]] QString colorScheme() const { return colorScheme_; }
    [[nodiscard]] QColor accentColor() const { return accentColor_; }
    [[nodiscard]] bool hasAccentColor() const { return accentColor_.isValid(); }

signals:
    void changed();

private:
    void readQt();
    void applySetting(const QString& key, const QVariant& value);

    QString colorScheme_;
    QColor accentColor_;
};
