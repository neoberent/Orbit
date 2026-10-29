#include "SplashScreen.hpp"

#include <QApplication>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>
#elif defined(__linux__)
#include <thread>
#include <xcb/xcb.h>
#endif

namespace {

constexpr qreal kCardWidth = 420;
constexpr qreal kCardHeight = 264;
constexpr qreal kMargin = 24;
constexpr qreal kRadius = 8;
constexpr double kMinimumVisibleSec = 0.95;
constexpr double kIntroSec = 0.18;
constexpr double kOutroSec = 0.24;
constexpr double kRevealFallbackSec = 2.5;

const QColor kCard(0x15, 0x15, 0x1a);
const QColor kBorder(0x2a, 0x2a, 0x32);
const QColor kAccent(0x5b, 0x6e, 0xf2);
const QColor kRing(0xc4, 0xcb, 0xfa);
const QColor kTrack(0x24, 0x24, 0x2c);
const QColor kTextPrimary(0xec, 0xec, 0xf0);
const QColor kTextSecondary(0x8a, 0x8a, 0x95);
const QColor kTextTertiary(0x5f, 0x5f, 0x6a);

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double easeOutCubic(double t) { t = clamp01(t); return 1.0 - std::pow(1.0 - t, 3.0); }
double easeInOutCubic(double t) {
    t = clamp01(t);
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

void drawMark(QPainter& p, const QPointF& center) {
    constexpr qreal discRadius = 20;
    constexpr qreal holeRadius = 7;
    constexpr qreal ringStroke = 2.4;
    const QRectF ring(-33, -9.5, 66, 19);

    QPainterPath disc;
    disc.setFillRule(Qt::OddEvenFill);
    disc.addEllipse(QPointF(0, 0), discRadius, discRadius);
    disc.addEllipse(QPointF(0, 0), holeRadius, holeRadius);

    QPainterPath discClip;
    discClip.addEllipse(QPointF(0, 0), discRadius + 0.5, discRadius + 0.5);

    QPen ringPen(kRing, ringStroke);
    ringPen.setCapStyle(Qt::FlatCap);

    p.save();
    p.translate(center);
    p.rotate(-22);
    p.setBrush(Qt::NoBrush);
    p.setPen(ringPen);
    p.drawArc(ring, 0, 180 * 16);

    p.fillPath(disc, kAccent);

    p.save();
    p.setClipPath(discClip);
    p.setPen(QPen(kCard, ringStroke + 3.0, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(ring, 180 * 16, 180 * 16);
    p.restore();

    p.setPen(ringPen);
    p.drawArc(ring, 180 * 16, 180 * 16);
    p.restore();
}

}

struct SplashScreen::Shared {
    std::mutex mutex;
    QString status;
    double target{0.0};
    std::chrono::steady_clock::time_point statusAt{std::chrono::steady_clock::now()};
    bool finishRequested{false};
    bool fadeRequested{false};
    std::atomic<bool> closeNow{false};
};

SplashScreen::SplashScreen(const QString& version, QObject* parent)
    : QObject(parent), shared_(std::make_shared<Shared>()), version_(version) {
    baseFont_ = QApplication::font();
    computePlacement();
}

SplashScreen::~SplashScreen() { dismiss(); }

void SplashScreen::computePlacement() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("ui"));
    const int w = settings.value(QStringLiteral("windowWidth"), 1240).toInt();
    const int h = settings.value(QStringLiteral("windowHeight"), 760).toInt();
    const int x = settings.value(QStringLiteral("windowX"), -1).toInt();
    const int y = settings.value(QStringLiteral("windowY"), -1).toInt();

    const QPoint savedCenter(x + w / 2, y + h / 2);
    QScreen* screen = (x >= 0 && y >= 0) ? QGuiApplication::screenAt(savedCenter) : nullptr;
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;

    const QRect available = screen->availableGeometry();
    const QPoint logicalCenter = (x >= 0 && y >= 0 && available.contains(savedCenter))
                                     ? savedCenter
                                     : available.center();
    scale_ = screen->devicePixelRatio();
    const QPoint origin = screen->geometry().topLeft();
    nativeCenter_ = origin + (logicalCenter - origin) * scale_;
}

void SplashScreen::setStatus(const QString& text, qreal progress) {
    std::scoped_lock lock(shared_->mutex);
    shared_->status = text;
    shared_->target = std::max(shared_->target, clamp01(progress));
    shared_->statusAt = std::chrono::steady_clock::now();
}

void SplashScreen::finish() {
    {
        std::scoped_lock lock(shared_->mutex);
        shared_->finishRequested = true;
        shared_->status = QStringLiteral("Ready");
        shared_->target = 1.0;
    }
    if (!thread_.joinable() && !revealPosted_.exchange(true)) {
        QMetaObject::invokeMethod(this, [this] { emit revealRequested(); }, Qt::QueuedConnection);
    }
}

void SplashScreen::fadeOut() {
    std::scoped_lock lock(shared_->mutex);
    shared_->fadeRequested = true;
}

void SplashScreen::dismiss() {
    shared_->closeNow = true;
    if (thread_.joinable()) thread_.join();
}

QImage SplashScreen::renderChrome(int width, int height) const {
    const QRectF card(kMargin, kMargin, kCardWidth, kCardHeight);

    QImage shadow(width, height, QImage::Format_ARGB32_Premultiplied);
    shadow.fill(Qt::transparent);
    {
        QPainter p(&shadow);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale_, scale_);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 95));
        p.drawRoundedRect(card.adjusted(4, 8, -4, 2), kRadius, kRadius);
    }
    const QSize blurSize(std::max(1, width / 10), std::max(1, height / 10));
    shadow = shadow.scaled(blurSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                 .scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QImage chrome(width, height, QImage::Format_ARGB32_Premultiplied);
    chrome.fill(Qt::transparent);
    QPainter p(&chrome);
    p.setRenderHint(QPainter::Antialiasing);
    p.drawImage(0, 0, shadow);
    p.scale(scale_, scale_);

    QPainterPath shape;
    shape.addRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    p.fillPath(shape, kCard);
    p.setPen(QPen(kBorder, 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
    return chrome;
}

#if defined(_WIN32)

void SplashScreen::show() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this] { run(); });
}

void SplashScreen::run() {
    using Clock = std::chrono::steady_clock;

    const int width = static_cast<int>(std::lround((kCardWidth + 2 * kMargin) * scale_));
    const int height = static_cast<int>(std::lround((kCardHeight + 2 * kMargin) * scale_));
    const POINT position{nativeCenter_.x() - width / 2, nativeCenter_.y() - height / 2};

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
    wc.lpszClassName = L"OrbitSplashWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"Orbit", WS_POPUP, position.x, position.y, width, height,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        if (!revealPosted_.exchange(true)) {
            QMetaObject::invokeMethod(this, [this] { emit revealRequested(); }, Qt::QueuedConnection);
        }
        return;
    }

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previousBitmap = SelectObject(memoryDc, dib);

    QImage canvas(static_cast<uchar*>(bits), width, height, width * 4, QImage::Format_ARGB32_Premultiplied);
    const QImage chrome = renderChrome(width, height);

    QFont titleFont(baseFont_);
    titleFont.setPixelSize(22);
    titleFont.setWeight(QFont::DemiBold);
    QFont subtitleFont(baseFont_);
    subtitleFont.setPixelSize(12);
    QFont captionFont(baseFont_);
    captionFont.setPixelSize(11);
    const QFontMetricsF captionMetrics(captionFont);

    const QRectF card(kMargin, kMargin, kCardWidth, kCardHeight);
    const QPointF logoCenter(card.center().x(), card.top() + 78);
    const qreal barLeft = card.left() + 32;
    const qreal barWidth = kCardWidth - 64;
    const qreal barTop = card.bottom() - 46;

    const auto start = Clock::now();
    double lastT = 0.0;
    double displayed = 0.0;
    double revealAt = -1.0;
    double fadeStart = -1.0;
    bool shown = false;

    while (!shared_->closeNow) {
        const auto frameStart = Clock::now();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const double t = std::chrono::duration<double>(frameStart - start).count();
        const double dt = std::max(0.0, t - lastT);
        lastT = t;

        QString status;
        double target = 0.0;
        double sinceStatus = 0.0;
        bool finishing = false;
        bool fading = false;
        {
            std::scoped_lock lock(shared_->mutex);
            status = shared_->status;
            target = shared_->target;
            sinceStatus = std::chrono::duration<double>(frameStart - shared_->statusAt).count();
            finishing = shared_->finishRequested;
            fading = shared_->fadeRequested;
        }

        const double creep = finishing ? 0.0 : 0.14 * (1.0 - std::exp(-sinceStatus / 1.3));
        const double goal = finishing ? 1.0 : std::max(target, std::min(0.94, target + creep));
        displayed += (goal - displayed) * (1.0 - std::exp(-dt * (finishing ? 10.0 : 4.0)));
        if (finishing && 1.0 - displayed < 0.004) displayed = 1.0;

        if (finishing && displayed >= 1.0 && t >= kMinimumVisibleSec && revealAt < 0.0) {
            revealAt = t;
            if (!revealPosted_.exchange(true)) {
                QMetaObject::invokeMethod(this, [this] { emit revealRequested(); }, Qt::QueuedConnection);
            }
        }
        if (revealAt >= 0.0 && t - revealAt > kRevealFallbackSec) fading = true;
        if (fading && fadeStart < 0.0) fadeStart = t;

        const double outro = fadeStart < 0.0 ? 0.0 : clamp01((t - fadeStart) / kOutroSec);
        if (outro >= 1.0) break;
        const double windowAlpha = easeOutCubic(t / kIntroSec) * (1.0 - easeInOutCubic(outro));

        const double contentT = easeOutCubic((t - 0.05) / 0.35);

        canvas.fill(Qt::transparent);
        {
            QPainter p(&canvas);
            p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
            p.drawImage(0, 0, chrome);
            p.scale(scale_, scale_);
            p.setOpacity(contentT);

            drawMark(p, logoCenter);

            p.setFont(titleFont);
            p.setPen(kTextPrimary);
            p.drawText(QRectF(card.left(), card.top() + 122, kCardWidth, 30), Qt::AlignHCenter | Qt::AlignTop,
                       QStringLiteral("Orbit"));
            p.setFont(subtitleFont);
            p.setPen(kTextSecondary);
            p.drawText(QRectF(card.left(), card.top() + 152, kCardWidth, 18), Qt::AlignHCenter | Qt::AlignTop,
                       QStringLiteral("Download Manager"));

            p.setPen(Qt::NoPen);
            p.setBrush(kTrack);
            p.drawRect(QRectF(barLeft, barTop, barWidth, 2));
            p.setBrush(kAccent);
            p.drawRect(QRectF(barLeft, barTop, barWidth * displayed, 2));

            const QRectF captionRow(barLeft, barTop + 10, barWidth, 16);
            p.setFont(captionFont);
            p.setPen(kTextTertiary);
            p.drawText(captionRow, Qt::AlignLeft | Qt::AlignVCenter,
                       captionMetrics.elidedText(status, Qt::ElideRight, barWidth - 100));
            p.drawText(captionRow, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Version ") + version_);
        }

        GdiFlush();
        BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255.0 * windowAlpha)), AC_SRC_ALPHA};
        POINT source{0, 0};
        SIZE size{width, height};
        POINT destination = position;
        UpdateLayeredWindow(hwnd, screenDc, &destination, &size, memoryDc, &source, 0, &blend, ULW_ALPHA);
        if (!shown) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            shown = true;
        }

        DwmFlush();
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - frameStart).count();
        if (spent < 10) Sleep(static_cast<DWORD>(14 - spent));
    }

    DestroyWindow(hwnd);
    SelectObject(memoryDc, previousBitmap);
    DeleteObject(dib);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    UnregisterClassW(wc.lpszClassName, instance);

    if (!revealPosted_.exchange(true)) {
        QMetaObject::invokeMethod(this, [this] { emit revealRequested(); }, Qt::QueuedConnection);
    }
}

#elif defined(__linux__)

namespace {

xcb_visualtype_t* findArgbVisual(xcb_screen_t* screen) {
    for (auto depths = xcb_screen_allowed_depths_iterator(screen); depths.rem; xcb_depth_next(&depths)) {
        if (depths.data->depth != 32) continue;
        for (auto visuals = xcb_depth_visuals_iterator(depths.data); visuals.rem; xcb_visualtype_next(&visuals)) {
            if (visuals.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) return visuals.data;
        }
    }
    return nullptr;
}

xcb_atom_t internAtom(xcb_connection_t* connection, const char* name) {
    xcb_intern_atom_reply_t* reply =
        xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr);
    const xcb_atom_t atom = reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    std::free(reply);
    return atom;
}

}

void SplashScreen::show() {
    if (thread_.joinable()) return;
    // The splash is drawn on its own X connection; without X (a pure Wayland session with
    // QT_QPA_PLATFORM=wayland) Orbit just opens straight into its window.
    if (QGuiApplication::platformName() != QLatin1String("xcb")) return;
    thread_ = std::thread([this] { run(); });
}

// Mirrors the Win32 loop above: an override-redirect ARGB window (no taskbar entry, never
// takes focus, stays on top) repainted from this thread while the GUI thread loads.
void SplashScreen::run() {
    using Clock = std::chrono::steady_clock;

    const auto postReveal = [this] {
        if (!revealPosted_.exchange(true)) {
            QMetaObject::invokeMethod(this, [this] { emit revealRequested(); }, Qt::QueuedConnection);
        }
    };

    const int width = static_cast<int>(std::lround((kCardWidth + 2 * kMargin) * scale_));
    const int height = static_cast<int>(std::lround((kCardHeight + 2 * kMargin) * scale_));
    const QPoint position(nativeCenter_.x() - width / 2, nativeCenter_.y() - height / 2);

    xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        postReveal();
        return;
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    xcb_visualtype_t* visual = screen ? findArgbVisual(screen) : nullptr;
    if (!visual) {
        xcb_disconnect(connection);
        postReveal();
        return;
    }

    const xcb_colormap_t colormap = xcb_generate_id(connection);
    xcb_create_colormap(connection, XCB_COLORMAP_ALLOC_NONE, colormap, screen->root, visual->visual_id);
    const xcb_window_t window = xcb_generate_id(connection);
    const uint32_t values[] = {0, 0, 1, XCB_EVENT_MASK_EXPOSURE, colormap};
    xcb_create_window(connection, 32, window, screen->root, static_cast<int16_t>(position.x()),
                      static_cast<int16_t>(position.y()), static_cast<uint16_t>(width), static_cast<uint16_t>(height), 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visual->visual_id,
                      XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP,
                      values);

    const char title[] = "Orbit";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        sizeof(title) - 1, title);
    const char wmClass[] = "orbit\0orbit";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof(wmClass), wmClass);
    const xcb_atom_t windowType = internAtom(connection, "_NET_WM_WINDOW_TYPE");
    const xcb_atom_t splashType = internAtom(connection, "_NET_WM_WINDOW_TYPE_SPLASH");
    if (windowType != XCB_ATOM_NONE && splashType != XCB_ATOM_NONE) {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, windowType, XCB_ATOM_ATOM, 32, 1, &splashType);
    }

    const xcb_gcontext_t gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, window, 0, nullptr);

    // put_image requests are capped in size; larger frames go up in bands of rows.
    const size_t stride = static_cast<size_t>(width) * 4;
    const size_t maxRequestBytes = static_cast<size_t>(xcb_get_maximum_request_length(connection)) * 4;
    const int rowsPerRequest = std::max(1, static_cast<int>((maxRequestBytes - 64) / stride));

    QImage canvas(width, height, QImage::Format_ARGB32_Premultiplied);
    QImage output(width, height, QImage::Format_ARGB32_Premultiplied);
    const QImage chrome = renderChrome(width, height);

    QFont titleFont(baseFont_);
    titleFont.setPixelSize(22);
    titleFont.setWeight(QFont::DemiBold);
    QFont subtitleFont(baseFont_);
    subtitleFont.setPixelSize(12);
    QFont captionFont(baseFont_);
    captionFont.setPixelSize(11);
    const QFontMetricsF captionMetrics(captionFont);

    const QRectF card(kMargin, kMargin, kCardWidth, kCardHeight);
    const QPointF logoCenter(card.center().x(), card.top() + 78);
    const qreal barLeft = card.left() + 32;
    const qreal barWidth = kCardWidth - 64;
    const qreal barTop = card.bottom() - 46;

    const auto start = Clock::now();
    double lastT = 0.0;
    double displayed = 0.0;
    double revealAt = -1.0;
    double fadeStart = -1.0;
    bool shown = false;

    while (!shared_->closeNow) {
        const auto frameStart = Clock::now();
        while (xcb_generic_event_t* event = xcb_poll_for_event(connection)) std::free(event);
        if (xcb_connection_has_error(connection)) break;

        const double t = std::chrono::duration<double>(frameStart - start).count();
        const double dt = std::max(0.0, t - lastT);
        lastT = t;

        QString status;
        double target = 0.0;
        double sinceStatus = 0.0;
        bool finishing = false;
        bool fading = false;
        {
            std::scoped_lock lock(shared_->mutex);
            status = shared_->status;
            target = shared_->target;
            sinceStatus = std::chrono::duration<double>(frameStart - shared_->statusAt).count();
            finishing = shared_->finishRequested;
            fading = shared_->fadeRequested;
        }

        const double creep = finishing ? 0.0 : 0.14 * (1.0 - std::exp(-sinceStatus / 1.3));
        const double goal = finishing ? 1.0 : std::max(target, std::min(0.94, target + creep));
        displayed += (goal - displayed) * (1.0 - std::exp(-dt * (finishing ? 10.0 : 4.0)));
        if (finishing && 1.0 - displayed < 0.004) displayed = 1.0;

        if (finishing && displayed >= 1.0 && t >= kMinimumVisibleSec && revealAt < 0.0) {
            revealAt = t;
            postReveal();
        }
        if (revealAt >= 0.0 && t - revealAt > kRevealFallbackSec) fading = true;
        if (fading && fadeStart < 0.0) fadeStart = t;

        const double outro = fadeStart < 0.0 ? 0.0 : clamp01((t - fadeStart) / kOutroSec);
        if (outro >= 1.0) break;
        const double windowAlpha = easeOutCubic(t / kIntroSec) * (1.0 - easeInOutCubic(outro));

        const double contentT = easeOutCubic((t - 0.05) / 0.35);

        canvas.fill(Qt::transparent);
        {
            QPainter p(&canvas);
            p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
            p.drawImage(0, 0, chrome);
            p.scale(scale_, scale_);
            p.setOpacity(contentT);

            drawMark(p, logoCenter);

            p.setFont(titleFont);
            p.setPen(kTextPrimary);
            p.drawText(QRectF(card.left(), card.top() + 122, kCardWidth, 30), Qt::AlignHCenter | Qt::AlignTop,
                       QStringLiteral("Orbit"));
            p.setFont(subtitleFont);
            p.setPen(kTextSecondary);
            p.drawText(QRectF(card.left(), card.top() + 152, kCardWidth, 18), Qt::AlignHCenter | Qt::AlignTop,
                       QStringLiteral("Download Manager"));

            p.setPen(Qt::NoPen);
            p.setBrush(kTrack);
            p.drawRect(QRectF(barLeft, barTop, barWidth, 2));
            p.setBrush(kAccent);
            p.drawRect(QRectF(barLeft, barTop, barWidth * displayed, 2));

            const QRectF captionRow(barLeft, barTop + 10, barWidth, 16);
            p.setFont(captionFont);
            p.setPen(kTextTertiary);
            p.drawText(captionRow, Qt::AlignLeft | Qt::AlignVCenter,
                       captionMetrics.elidedText(status, Qt::ElideRight, barWidth - 100));
            p.drawText(captionRow, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Version ") + version_);
        }

        // The window's overall opacity (UpdateLayeredWindow's constant alpha on Windows).
        const QImage* frame = &canvas;
        if (windowAlpha < 0.999) {
            output.fill(Qt::transparent);
            QPainter p(&output);
            p.setOpacity(std::max(0.0, windowAlpha));
            p.drawImage(0, 0, canvas);
            frame = &output;
        }
        for (int row = 0; row < height; row += rowsPerRequest) {
            const int rows = std::min(rowsPerRequest, height - row);
            xcb_put_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, gc, static_cast<uint16_t>(width),
                          static_cast<uint16_t>(rows), 0, static_cast<int16_t>(row), 0, 32,
                          static_cast<uint32_t>(stride * static_cast<size_t>(rows)), frame->constScanLine(row));
        }
        if (!shown) {
            xcb_map_window(connection, window);
            shown = true;
        }
        xcb_flush(connection);

        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - frameStart).count();
        if (spent < 16) std::this_thread::sleep_for(std::chrono::milliseconds(16 - spent));
    }

    xcb_destroy_window(connection, window);
    xcb_free_gc(connection, gc);
    xcb_free_colormap(connection, colormap);
    xcb_flush(connection);
    xcb_disconnect(connection);

    postReveal();
}

#else

void SplashScreen::show() {}

void SplashScreen::run() {}

#endif
