#include "SystemAppearance.hpp"

#include "utils/Logger.hpp"

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

#if defined(__linux__)
#include "PortalSettings.hpp"
#include <QVariantList>
#endif

SystemAppearance::SystemAppearance(QObject* parent) : QObject(parent) {
#if defined(__linux__)
    auto* portal = new PortalSettings(this);
    connect(portal, &PortalSettings::settingChanged, this, &SystemAppearance::applySetting);
    portal->read(QStringLiteral("color-scheme"));
    portal->read(QStringLiteral("accent-color"));
#else
    readQt();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { readQt(); });
#endif
}

void SystemAppearance::readQt() {
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    colorScheme_ = scheme == Qt::ColorScheme::Dark ? QStringLiteral("dark")
                   : scheme == Qt::ColorScheme::Light ? QStringLiteral("light")
                                                      : QString();
    accentColor_ = QGuiApplication::palette().color(QPalette::Accent);
    emit changed();
}

void SystemAppearance::applySetting(const QString& key, const QVariant& value) {
    if (key == QLatin1String("color-scheme")) {
        // 0 = no preference, 1 = prefer dark, 2 = prefer light.
        const uint scheme = value.toUInt();
        const QString next = scheme == 1 ? QStringLiteral("dark") : scheme == 2 ? QStringLiteral("light") : QString();
        if (next == colorScheme_) return;
        colorScheme_ = next;
    } else if (key == QLatin1String("accent-color")) {
        // Red, green and blue from 0 to 1; anything out of range means "not set".
        const QVariantList rgb = value.toList();
        QColor next;
        if (rgb.size() == 3) {
            const double r = rgb[0].toDouble(), g = rgb[1].toDouble(), b = rgb[2].toDouble();
            if (r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1) {
                next = QColor::fromRgbF(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
            }
        }
        if (next == accentColor_) return;
        accentColor_ = next;
    } else {
        return;
    }
    ORBIT_LOG_INFO("appearance", "Desktop appearance: " +
                                     (colorScheme_.isEmpty() ? std::string("no preference") : colorScheme_.toStdString()) +
                                     (accentColor_.isValid() ? ", accent " + accentColor_.name().toStdString() : std::string()));
    emit changed();
}
