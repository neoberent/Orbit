#include "ChecksumCalculator.hpp"
#include "ClipboardMonitor.hpp"
#include "DownloadFilterProxyModel.hpp"
#include "DownloadListModel.hpp"
#include "LocalApiServer.hpp"
#include "MainWindow.hpp"
#include "PowerManager.hpp"
#include "SplashScreen.hpp"
#include "TrayController.hpp"
#include "UpdateManager.hpp"
#include "YtDlpUpdater.hpp"
#include "core/DownloadManager.hpp"
#include "core/ProtocolHandler.hpp"
#include "core/SingleInstanceGuard.hpp"
#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml/qqml.h>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

#include <memory>
#include <utility>
#include <QStringList>
#include <QSettings>

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>
#else
#include <QFontDatabase>
#include <QSocketNotifier>
#include <QUrl>

#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/auxv.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#endif

namespace {

constexpr auto kSingleInstanceKey = "OrbitDownloadManager.SingleInstance";

// Looks for a pending source handed to us on the command line — either an
// "orbit://add?url=..." argument (from the protocol handler) or a bare .torrent file
// path (from Windows launching us as the file's registered app) — and returns it, or
// an empty string if there isn't one.
QString extractPendingProtocolUrl(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (auto decoded = orbit::core::protocol::extractDownloadUrl(arg)) {
            return QString::fromStdString(*decoded);
        }
        if (orbit::core::protocol::isOrbitUri(arg)) {
            // Looked like an orbit:// launch but didn't parse — log the raw string so a
            // browser/OS quirk in how it's shaped shows up instead of failing silently.
            ORBIT_LOG_WARN("protocol", "Received an orbit:// argument that failed to parse: " + arg);
            continue;
        }
        QString argQ = QString::fromStdString(arg);
        // Linux desktops may hand an opened file over as a file:// URL.
        if (argQ.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) argQ = QUrl(argQ).toLocalFile();
        if (argQ.endsWith(QStringLiteral(".torrent"), Qt::CaseInsensitive)) {
            return argQ;
        }
    }
    return {};
}

// Portable mode keeps every setting, log and download record in a Data folder next to
// orbit.exe (switched on by a portable.flag file there). ORBIT_DATA_DIR does the same for
// any folder, which keeps test profiles away from the real one.
QString resolveDataDirectory(bool& portable) {
    portable = QFileInfo::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/portable.flag"));
    const QString overrideDir = qEnvironmentVariable("ORBIT_DATA_DIR");
    if (!overrideDir.isEmpty()) return QDir(overrideDir).absolutePath();
    if (portable) return QCoreApplication::applicationDirPath() + QStringLiteral("/Data");
    return {};
}

// A restart (after importing a backup) starts the new process before the old one has
// exited; let the old one finish before taking over as the single instance.
void waitForPreviousInstance(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string prefix = "--wait-for-exit=";
        if (arg.rfind(prefix, 0) != 0) continue;
#if defined(_WIN32)
        const DWORD pid = static_cast<DWORD>(std::strtoul(arg.c_str() + prefix.size(), nullptr, 10));
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(process, 20000);
            CloseHandle(process);
        }
#else
        const auto pid = static_cast<pid_t>(std::strtol(arg.c_str() + prefix.size(), nullptr, 10));
        if (pid <= 1) continue;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (::kill(pid, 0) == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#endif
    }
}

#if !defined(_WIN32)
// Orbit places its own windows (the splash over the saved window position, the clipboard
// prompt by the tray, the main window where it was left), which Wayland doesn't allow
// clients to do. Under a Wayland session it therefore runs through XWayland, where it
// behaves exactly as on Windows; QT_QPA_PLATFORM still picks something else.
// Started through a symlink (~/.local/bin/orbit), Qt reports the link's folder as Orbit's
// own, where orbit-native-host, portable.flag and in-app updates don't belong. Qt takes
// that path from the kernel, so Orbit restarts itself from its real location instead.
void runFromRealLocation(char* argv[]) {
    const auto* launchedAs = reinterpret_cast<const char*>(::getauxval(AT_EXECFN));
    char real[PATH_MAX];
    const ssize_t length = ::readlink("/proc/self/exe", real, sizeof(real) - 1);
    if (!launchedAs || length <= 0) return;
    real[length] = '\0';
    char resolved[PATH_MAX];
    if (!::realpath(launchedAs, resolved) || std::strcmp(launchedAs, resolved) == 0) return;
    if (std::strcmp(resolved, real) != 0) return;  // Not this executable after all.
    ::execv(real, argv);
}

// Returns whether it set QT_QPA_PLATFORM, which is cleared again once Qt has started so
// programs Orbit opens (a video player, the file manager) keep their own default.
bool choosePlatform() {
    if (!qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) return false;
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") || qEnvironmentVariableIsEmpty("DISPLAY")) return false;
    qputenv("QT_QPA_PLATFORM", "xcb;wayland");
    return true;
}

// Segoe UI is Windows-only; Inter, the next font in Orbit's list, ships inside Orbit so
// the interface looks the same on every Linux system.
void loadBundledFonts() {
    for (const char* weight : {"Regular", "Medium", "SemiBold", "Bold"}) {
        if (QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter-%1.ttf").arg(QLatin1String(weight))) < 0) {
            ORBIT_LOG_WARN("main", std::string("Couldn't load the bundled Inter ") + weight);
        }
    }
}

// SIGTERM (logout, shutdown, `kill`) and SIGINT/SIGHUP quit through the event loop like
// closing from the tray does, so torrent resume data and the download list get saved.
int g_signalFds[2] = {-1, -1};

void handleQuitSignal(int) {
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(g_signalFds[1], &byte, 1);
}

void installQuitSignalHandlers(QCoreApplication& app) {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, g_signalFds) != 0) return;
    auto* notifier = new QSocketNotifier(g_signalFds[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [notifier] {
        notifier->setEnabled(false);
        char byte = 0;
        [[maybe_unused]] const auto got = ::read(g_signalFds[0], &byte, 1);
        ORBIT_LOG_INFO("main", "Asked to quit by the system");
        QCoreApplication::quit();
    });
    struct sigaction action{};
    action.sa_handler = handleQuitSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (int signal : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(signal, &action, nullptr);
}
#endif

}

int main(int argc, char* argv[]) {
    Q_INIT_RESOURCE(webassets);

    waitForPreviousInstance(argc, argv);
#if !defined(_WIN32)
    runFromRealLocation(argv);
    const bool platformChosen = choosePlatform();
#endif

    QApplication app(argc, argv);
#if !defined(_WIN32)
    if (platformChosen) qunsetenv("QT_QPA_PLATFORM");
#endif
    QApplication::setOrganizationName("Orbit");
    QApplication::setApplicationName("Orbit Download Manager");
    QApplication::setApplicationVersion(QStringLiteral(ORBIT_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/images/logo.png")));
    QApplication::setQuitOnLastWindowClosed(false);
#if !defined(_WIN32)
    // Ties windows and the tray icon to orbit.desktop for the taskbar icon and name.
    QGuiApplication::setDesktopFileName(QStringLiteral("orbit"));
#endif

    bool portable = false;
    const QString dataDirectory = resolveDataDirectory(portable);
    if (!dataDirectory.isEmpty()) {
        QDir().mkpath(dataDirectory);
        orbit::utils::setAppDataDirOverride(dataDirectory.toStdWString());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dataDirectory);
    }

    // Set up logging before anything else so both the primary instance and a
    // forwarding/secondary launch (which may exit within milliseconds) get logged.
    orbit::utils::Logger::instance().setLogFile(
        (orbit::utils::appDataDir() / "orbit.log").string());
    ORBIT_LOG_INFO("main", "Orbit starting up");
    if (!dataDirectory.isEmpty()) {
        ORBIT_LOG_INFO("main", std::string(portable ? "Portable mode, data in " : "Using data folder ") + dataDirectory.toStdString());
    }

    const QString pendingProtocolUrl = extractPendingProtocolUrl(argc, argv);

    orbit::core::SingleInstanceGuard instanceGuard(QString::fromLatin1(kSingleInstanceKey));
    if (!instanceGuard.start(pendingProtocolUrl)) {
        // Another instance already owns the app; our payload (possibly empty, meaning
        // "just come to the foreground") has already been forwarded to it. Exit immediately.
        ORBIT_LOG_INFO("protocol", pendingProtocolUrl.isEmpty()
                                        ? "Secondary instance: forwarded a wake-up (no URL)"
                                        : "Secondary instance: forwarded URL to primary: " +
                                              pendingProtocolUrl.toStdString());
        return 0;
    }

#if defined(_WIN32)
    CreateMutexW(nullptr, FALSE, L"OrbitDownloadManager.Running");
#else
    installQuitSignalHandlers(app);
    loadBundledFonts();
#endif

    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
                          QStringLiteral("Inter"), QStringLiteral("sans-serif")});
    appFont.setHintingPreference(QFont::PreferFullHinting);
    QApplication::setFont(appFont);

    SplashScreen splash(QApplication::applicationVersion());
    splash.show();
    splash.setStatus(QStringLiteral("Starting up"), 0.08);

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle("Basic");
    }

    splash.setStatus(QStringLiteral("Restoring downloads"), 0.3);
    orbit::core::DownloadManager manager(orbit::utils::appDataDir() / "state");

    DownloadListModel downloadModel(manager);
    MainWindow controller(manager, portable);
    TrayController trayController;
    UpdateManager updateManager;
    YtDlpUpdater ytDlpUpdater;
    PowerManager powerManager(downloadModel);
    ClipboardMonitor clipboardMonitor;
    LocalApiServer apiServer([&controller](const QJsonObject& request) { return controller.handleApiRequest(request); });
    apiServer.listen();

    qmlRegisterType<ChecksumCalculator>("Orbit.Native", 1, 0, "ChecksumCalculator");

    DownloadFilterProxyModel downloadProxyModel;
    downloadProxyModel.setSourceModel(&downloadModel);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("downloadModel", &downloadModel);
    engine.rootContext()->setContextProperty("downloadProxyModel", &downloadProxyModel);
    engine.rootContext()->setContextProperty("appController", &controller);
    engine.rootContext()->setContextProperty("trayController", &trayController);
    engine.rootContext()->setContextProperty("updateManager", &updateManager);
    engine.rootContext()->setContextProperty("ytDlpUpdater", &ytDlpUpdater);
    engine.rootContext()->setContextProperty("powerManager", &powerManager);
    engine.rootContext()->setContextProperty("clipboardMonitor", &clipboardMonitor);

    QObject::connect(&trayController, &TrayController::quitRequested, &app, &QCoreApplication::quit);
    QObject::connect(&powerManager, &PowerManager::quitRequested, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    QObject::connect(&controller, &MainWindow::quitRequested, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    QObject::connect(&trayController, &TrayController::whenDoneRequested, &powerManager, &PowerManager::setWhenDoneAction);
    QObject::connect(&powerManager, &PowerManager::whenDoneActionChanged, &trayController, [&] {
        trayController.setWhenDoneAction(powerManager.whenDoneAction(), powerManager.hibernateAvailable());
    });
    trayController.setWhenDoneAction(powerManager.whenDoneAction(), powerManager.hibernateAvailable());

    // A plain link/magnet starts immediately and gets a tray toast; a .torrent instead
    // triggers externalTorrentNeedsReview, which main.qml handles by raising the window
    // and opening the Add Download dialog for file-selection review.
    QObject::connect(&controller, &MainWindow::externalDownloadAdded, &trayController,
                      &TrayController::notifyDownloadAdded);

    // Downloads handed to us by a second launch (protocol click, or plain relaunch) arrive
    // here. An empty payload just means "bring the window to the front".
    auto handleIncomingPayload = [&](const QString& payload) {
        if (payload.isEmpty()) {
            ORBIT_LOG_INFO("protocol", "Received wake-up with no URL; raising window");
            trayController.showAndRaise();
            return;
        }
        ORBIT_LOG_INFO("protocol", "Received URL from another launch: " + payload.toStdString());
        controller.addFromExternalSource(payload);
    };
    bool uiRevealed = false;
    QStringList deferredPayloads;
    if (!pendingProtocolUrl.isEmpty()) deferredPayloads << pendingProtocolUrl;
    QObject::connect(&instanceGuard, &orbit::core::SingleInstanceGuard::payloadReceived, &app,
                      [&](const QString& payload) {
                          if (!uiRevealed) {
                              deferredPayloads << payload;
                              return;
                          }
                          handleIncomingPayload(payload);
                      });

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app, [](const QList<QQmlError>& warnings) {
        for (const QQmlError& w : warnings) {
            ORBIT_LOG_ERROR("qml", w.toString().toStdString());
        }
    });

    splash.setStatus(QStringLiteral("Loading interface"), 0.6);
    engine.load(QUrl(QStringLiteral("qrc:/qml/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        ORBIT_LOG_ERROR("main", "Failed to load QML UI");
        splash.dismiss();
        return -1;
    }

    auto* rootWindow = qobject_cast<QQuickWindow*>(engine.rootObjects().first());

    QObject::connect(&updateManager, &UpdateManager::installerLaunched, &app, [rootWindow] {
        if (rootWindow) rootWindow->hide();
        QCoreApplication::quit();
    }, Qt::QueuedConnection);
    updateManager.startAutomaticChecks();
    ytDlpUpdater.start();
    QObject::connect(&downloadModel, &DownloadListModel::transferFinished, &ytDlpUpdater,
                      [&](qlonglong id, const QString&, bool success, const QString&) {
                          if (!success && downloadModel.get(id).value(QStringLiteral("isMedia")).toBool()) {
                              ytDlpUpdater.noteMediaFailure();
                          }
                      });

    {
        QSettings settings;
        const QString lastRunVersion = settings.value("updates/lastRunVersion").toString();
        const QString runningVersion = QApplication::applicationVersion();
        if (!lastRunVersion.isEmpty() && lastRunVersion != runningVersion) {
            ORBIT_LOG_INFO("update", "Updated from " + lastRunVersion.toStdString() + " to " + runningVersion.toStdString());
            QTimer::singleShot(1500, &trayController, [&trayController, runningVersion] {
                trayController.notify(QStringLiteral("Orbit was updated"),
                                      QStringLiteral("You're now running version ") + runningVersion, false);
            });
        }
        settings.setValue("updates/lastRunVersion", runningVersion);
    }

    if (rootWindow) {
        trayController.attachWindow(rootWindow);

#if defined(_WIN32)
        // Qt doesn't sync the native title bar/frame to the app's own dark theme on
        // Windows, so without this the window chrome stays light (following the OS
        // default) even though every QML surface underneath is dark.
        auto applyTitleBarTheme = [rootWindow, &controller] {
            const auto hwnd = reinterpret_cast<HWND>(rootWindow->winId());
            const BOOL useDarkMode = controller.darkTheme() ? TRUE : FALSE;
            constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
            DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &useDarkMode, sizeof(useDarkMode));
        };
        applyTitleBarTheme();
        QObject::connect(&controller, &MainWindow::darkThemeChanged, &app, applyTitleBarTheme);
#endif
    }

    QObject::connect(&splash, &SplashScreen::revealRequested, &app, [&] {
        if (uiRevealed) return;
        uiRevealed = true;
        if (rootWindow) {
#if defined(_WIN32)
            const auto hwnd = reinterpret_cast<HWND>(rootWindow->winId());
            constexpr DWORD kDwmwaCloak = 13;
            const BOOL cloak = TRUE;
            DwmSetWindowAttribute(hwnd, kDwmwaCloak, &cloak, sizeof(cloak));
#endif
            auto handedOver = std::make_shared<bool>(false);
            auto frames = std::make_shared<int>(0);
            auto frameConnection = std::make_shared<QMetaObject::Connection>();
            auto handOver = [=, &splash] {
                if (*handedOver) return;
                *handedOver = true;
                QObject::disconnect(*frameConnection);
#if defined(_WIN32)
                const BOOL uncloak = FALSE;
                DwmSetWindowAttribute(hwnd, kDwmwaCloak, &uncloak, sizeof(uncloak));
#endif
                splash.fadeOut();
            };
            *frameConnection = QObject::connect(
                rootWindow, &QQuickWindow::frameSwapped, &app,
                [frames, handOver] {
                    if (++*frames >= 2) handOver();
                },
                Qt::QueuedConnection);
            QTimer::singleShot(600, &app, handOver);
            QMetaObject::invokeMethod(rootWindow, "reveal");
        } else {
            splash.fadeOut();
        }
        for (const QString& payload : std::exchange(deferredPayloads, {})) {
            QMetaObject::invokeMethod(
                &app, [&, payload] { handleIncomingPayload(payload); }, Qt::QueuedConnection);
        }
    });
    splash.finish();

    const int exitCode = QApplication::exec();
    ORBIT_LOG_INFO("main", "Shutting down");
    return exitCode;
}
