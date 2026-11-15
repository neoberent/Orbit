#include "CommandLine.hpp"

#include "utils/LocalSocket.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStringList>
#include <QThread>
#include <QUrl>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace command_line {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNotRunning = 3;

const char* const kUsage =
    "Usage: orbit [command] [options]\n"
    "\n"
    "Without a command, Orbit opens its window (or brings it to the front).\n"
    "\n"
    "Commands:\n"
    "  add <link|file.torrent>...   Add downloads (http(s) links, magnet links, .torrent files)\n"
    "      -d, --dir <folder>       Save them in this folder instead of the default one\n"
    "      -i, --input <file>       Also add every link in this file, one per line (- = stdin)\n"
    "      --checksum <type:hex>    Verify the file when it finishes (sha256:..., md5:...)\n"
    "      --force                  Add it even if the same link was downloaded before\n"
    "      --mirror <link>          Another link to the same file (repeatable); each serves\n"
    "                               some of the connections\n"
    "  list, ls                     Show every download\n"
    "  status                       Show totals and current speeds\n"
    "  pause <id>... | --all        Pause downloads\n"
    "  resume <id>... | --all       Resume paused or failed downloads\n"
    "  cancel <id>... | --all       Cancel downloads (the partial file is kept)\n"
    "  remove, rm <id>...           Remove finished, paused or failed downloads from the list\n"
    "                               (files on disk are never deleted)\n"
    "  show                         Open Orbit's window\n"
    "  help                         Show this help\n"
    "\n"
    "Options:\n"
    "  --json                       Print the answer as JSON (list, status, add)\n"
    "  --background                 Start Orbit in the tray without opening the window\n"
    "  --headless                   Run without any window or tray (for servers; use the\n"
    "                               web dashboard or these commands to control it)\n"
    "  --version                    Print Orbit's version\n";

bool useColor() {
    return ::isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr;
}

void printLine(const QString& text) {
    std::fputs(text.toLocal8Bit().constData(), stdout);
    std::fputc('\n', stdout);
}

void printError(const QString& text) {
    std::fprintf(stderr, "orbit: %s\n", text.toLocal8Bit().constData());
}

QString formatBytes(double bytes) {
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) {
        bytes /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(static_cast<qint64>(bytes))
                     : QStringLiteral("%1 %2").arg(bytes, 0, 'f', bytes < 10 ? 1 : 0).arg(QLatin1String(units[unit]));
}

QString formatRate(double bytesPerSec) {
    return bytesPerSec <= 0 ? QStringLiteral("-") : formatBytes(bytesPerSec) + QStringLiteral("/s");
}

QString formatEta(double seconds) {
    if (seconds <= 0) return {};
    const auto s = static_cast<qint64>(seconds);
    if (s < 60) return QStringLiteral("%1s").arg(s);
    if (s < 3600) return QStringLiteral("%1m%2s").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1h%2m").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QLatin1Char('0'));
}

QString stateColor(const QString& state) {
    if (state == QLatin1String("Downloading") || state == QLatin1String("Connecting")) return QStringLiteral("\x1b[36m");
    if (state == QLatin1String("Completed") || state == QLatin1String("Seeding")) return QStringLiteral("\x1b[32m");
    if (state == QLatin1String("Error")) return QStringLiteral("\x1b[31m");
    if (state == QLatin1String("Paused") || state == QLatin1String("Cancelled")) return QStringLiteral("\x1b[33m");
    return QStringLiteral("\x1b[2m");
}

QLocalSocket* connectToOrbit(int timeoutMs) {
    auto* socket = new QLocalSocket();
    const QString name = orbit::utils::localServerName(QString::fromLatin1(kControlSocketName));
    QDeadlineTimer deadline(timeoutMs);
    while (true) {
        socket->connectToServer(name);
        if (socket->waitForConnected(500)) return socket;
        if (deadline.hasExpired()) break;
        QThread::msleep(150);
    }
    delete socket;
    return nullptr;
}

std::optional<QJsonObject> send(QLocalSocket* socket, const QJsonObject& request) {
    socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket->waitForBytesWritten(5000)) return std::nullopt;
    QByteArray reply;
    QDeadlineTimer deadline(15000);
    while (!reply.contains('\n')) {
        if (deadline.hasExpired() || !socket->waitForReadyRead(static_cast<int>(deadline.remainingTime()))) return std::nullopt;
        reply += socket->readAll();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(reply.left(reply.indexOf('\n')));
    if (!doc.isObject()) return std::nullopt;
    return doc.object();
}

void printDownloads(const QJsonArray& downloads) {
    if (downloads.isEmpty()) {
        printLine(QStringLiteral("No downloads."));
        return;
    }
    const bool color = useColor();
    const QString dim = color ? QStringLiteral("\x1b[2m") : QString();
    const QString reset = color ? QStringLiteral("\x1b[0m") : QString();
    printLine(dim + QStringLiteral("%1  %2  %3  %4  %5  %6")
                        .arg(QStringLiteral("ID"), -4)
                        .arg(QStringLiteral("STATE"), -11)
                        .arg(QStringLiteral("DONE"), 5)
                        .arg(QStringLiteral("SPEED"), 10)
                        .arg(QStringLiteral("SIZE"), 9)
                        .arg(QStringLiteral("NAME")) +
              reset);
    for (const QJsonValue& value : downloads) {
        const QJsonObject d = value.toObject();
        const QString state = d.value("state").toString();
        const double total = d.value("total").toDouble();
        const QString done = total > 0 ? QStringLiteral("%1%").arg(qRound(d.value("progress").toDouble() * 100)) : QStringLiteral("-");
        const double rate = state == QLatin1String("Seeding") ? d.value("uploadRate").toDouble() : d.value("downloadRate").toDouble();
        QString speed = formatRate(rate);
        if (state == QLatin1String("Seeding") && rate > 0) speed = QStringLiteral("↑") + speed;
        QString name = d.value("name").toString();
        if (name.isEmpty()) name = d.value("source").toString();
        const QString eta = formatEta(d.value("eta").toDouble());
        if (!eta.isEmpty() && (state == QLatin1String("Downloading"))) name += dim + QStringLiteral("  (%1 left)").arg(eta) + reset;
        if (state == QLatin1String("Error") && d.contains("error")) name += dim + QStringLiteral("  - ") + d.value("error").toString() + reset;
        const QString stateText = QStringLiteral("%1").arg(state, -11);
        printLine(QStringLiteral("%1  %2  %3  %4  %5  %6")
                      .arg(d.value("id").toString(), -4)
                      .arg(color ? stateColor(state) + stateText + reset : stateText)
                      .arg(done, 5)
                      .arg(speed, 10)
                      .arg(total > 0 ? formatBytes(total)
                           : d.value("downloaded").toDouble() > 0 ? formatBytes(d.value("downloaded").toDouble())
                                                                  : QStringLiteral("-"), 9)
                      .arg(name));
    }
}

void printStatus(const QJsonObject& s) {
    QStringList parts;
    const std::pair<const char*, const char*> counts[] = {
        {"active", "downloading"}, {"queued", "queued"}, {"paused", "paused"},
        {"seeding", "seeding"}, {"completed", "completed"}, {"failed", "failed"},
    };
    for (const auto& [key, label] : counts) {
        const int n = s.value(QLatin1String(key)).toInt();
        if (n > 0) parts << QStringLiteral("%1 %2").arg(n).arg(QLatin1String(label));
    }
    printLine(QStringLiteral("Orbit is running: ") + (parts.isEmpty() ? QStringLiteral("no downloads") : parts.join(QStringLiteral(", "))));
    printLine(QStringLiteral("Speed: ↓ %1   ↑ %2").arg(formatRate(s.value("downloadRate").toDouble()), formatRate(s.value("uploadRate").toDouble())));
    if (!s.value("networkInterface").toString().isEmpty()) {
        printLine(QStringLiteral("Bound to network interface: ") + s.value("networkInterface").toString());
    }
    if (s.value("metered").toBool()) printLine(QStringLiteral("Paused while the connection is metered"));
}

QString absoluteSource(const QString& arg) {
    QString source = arg;
    if (source.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) source = QUrl(source).toLocalFile();
    if (source.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive) && !source.contains(QLatin1String("://")) &&
        !source.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
        return QFileInfo(source).absoluteFilePath();
    }
    return source;
}

int runCommand(const QString& command, const QStringList& args) {
    bool json = false;
    bool all = false;
    bool force = false;
    QString directory;
    QString checksum;
    QStringList mirrors;
    QStringList positional;
    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        if (a == QLatin1String("--json")) json = true;
        else if (a == QLatin1String("--all") || a == QLatin1String("-a")) all = true;
        else if (a == QLatin1String("--force") || a == QLatin1String("-f")) force = true;
        else if (a == QLatin1String("-d") || a == QLatin1String("--dir")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("%1 needs a folder").arg(a)); return kExitUsage; }
            directory = QFileInfo(args[++i]).absoluteFilePath();
        } else if (a.startsWith(QLatin1String("--dir="))) directory = QFileInfo(a.mid(6)).absoluteFilePath();
        else if (a == QLatin1String("-i") || a == QLatin1String("--input")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("%1 needs a file").arg(a)); return kExitUsage; }
            const QString path = args[++i];
            QFile file;
            const bool opened = path == QLatin1String("-") ? file.open(stdin, QIODevice::ReadOnly | QIODevice::Text)
                                                           : (file.setFileName(path), file.open(QIODevice::ReadOnly | QIODevice::Text));
            if (!opened) { printError(QStringLiteral("can't read %1").arg(path)); return kExitFailed; }
            while (!file.atEnd()) {
                // Lines are capped too, so a huge file without newlines can't fill memory.
                const QString line = QString::fromUtf8(file.readLine(64 * 1024)).trimmed();
                if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) positional << line;
                if (positional.size() > 1000) { printError(QStringLiteral("too many links (1000 at most)")); return kExitUsage; }
            }
        } else if (a == QLatin1String("--mirror")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("--mirror needs a link")); return kExitUsage; }
            mirrors << args[++i];
        } else if (a == QLatin1String("--checksum")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("--checksum needs a value")); return kExitUsage; }
            checksum = args[++i];
        } else if (a == QLatin1String("--")) {
            positional << args.mid(i + 1);
            break;
        } else if (a.startsWith(QLatin1Char('-')) && a.size() > 1) {
            printError(QStringLiteral("unknown option %1 (see orbit help)").arg(a));
            return kExitUsage;
        } else positional << a;
    }

    const bool isAdd = command == QLatin1String("add");
    if (isAdd && positional.isEmpty()) { printError(QStringLiteral("add needs a link or a .torrent file")); return kExitUsage; }
    const bool needsIds = command == QLatin1String("pause") || command == QLatin1String("resume") ||
                          command == QLatin1String("cancel") || command == QLatin1String("remove");
    if (needsIds && positional.isEmpty() && !all) {
        printError(QStringLiteral("%1 needs download ids (from orbit list) or --all").arg(command));
        return kExitUsage;
    }

    QLocalSocket* socket = connectToOrbit(300);
    if (!socket && (isAdd || command == QLatin1String("show"))) {
        // Adding (or showing) is worth starting Orbit for; it stays in the tray.
        QStringList launchArgs;
        if (isAdd) launchArgs << QStringLiteral("--background");
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), launchArgs)) {
            printError(QStringLiteral("couldn't start Orbit"));
            return kExitFailed;
        }
        socket = connectToOrbit(15000);
        if (socket && command == QLatin1String("show")) {
            delete socket;
            return kExitOk;
        }
    }
    if (!socket) {
        if (command == QLatin1String("status") && !json) printLine(QStringLiteral("Orbit isn't running."));
        else printError(QStringLiteral("Orbit isn't running"));
        return kExitNotRunning;
    }

    int exitCode = kExitOk;
    auto request = [&](const QJsonObject& r) -> std::optional<QJsonObject> {
        auto reply = send(socket, r);
        if (!reply) {
            printError(QStringLiteral("Orbit didn't answer"));
            exitCode = kExitFailed;
        }
        return reply;
    };

    if (isAdd) {
        QJsonArray added;
        for (const QString& arg : positional) {
            QJsonObject r{{"type", "add"}, {"source", absoluteSource(arg)}};
            if (!directory.isEmpty()) r.insert("directory", directory);
            if (!checksum.isEmpty()) r.insert("checksum", checksum);
            if (force) r.insert("force", true);
            if (!mirrors.isEmpty()) r.insert("mirrors", QJsonArray::fromStringList(mirrors));
            const auto reply = request(r);
            if (!reply) break;
            if (!reply->value("ok").toBool()) {
                printError(arg + QStringLiteral(": ") + reply->value("error").toString());
                exitCode = kExitFailed;
                continue;
            }
            const QJsonObject d = reply->value("download").toObject();
            added.append(d);
            if (!json) {
                const QString name = d.value("name").toString();
                printLine(QStringLiteral("Added %1  %2").arg(d.value("id").toString(), name.isEmpty() ? arg : name));
            }
        }
        if (json) printLine(QString::fromUtf8(QJsonDocument(added).toJson(QJsonDocument::Indented)).trimmed());
    } else if (command == QLatin1String("list") || command == QLatin1String("status")) {
        if (const auto reply = request(QJsonObject{{"type", command}})) {
            if (json) {
                const QJsonValue value = command == QLatin1String("list") ? reply->value("downloads") : QJsonValue(*reply);
                const QJsonDocument doc = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
                printLine(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed());
            } else if (command == QLatin1String("list")) {
                printDownloads(reply->value("downloads").toArray());
            } else {
                printStatus(*reply);
            }
        }
    } else if (needsIds) {
        QJsonObject r{{"type", command}, {"all", all}};
        if (!all) r.insert("ids", QJsonArray::fromStringList(positional));
        if (const auto reply = request(r)) {
            if (!reply->value("ok").toBool()) {
                printError(reply->value("error").toString());
                exitCode = kExitFailed;
            } else {
                const int changed = reply->value("changed").toInt();
                const QString verb = command == QLatin1String("pause") ? QStringLiteral("Paused")
                                     : command == QLatin1String("resume") ? QStringLiteral("Resumed")
                                     : command == QLatin1String("cancel") ? QStringLiteral("Cancelled")
                                                                          : QStringLiteral("Removed");
                printLine(QStringLiteral("%1 %2 download%3").arg(verb).arg(changed).arg(changed == 1 ? "" : "s"));
            }
        }
    } else if (command == QLatin1String("show")) {
        request(QJsonObject{{"type", "show"}});
    }
    socket->disconnectFromServer();
    delete socket;
    return exitCode;
}

}

std::optional<int> run(int argc, char* argv[]) {
    if (argc < 2) return std::nullopt;
    QString command = QString::fromLocal8Bit(argv[1]);
    if (command == QLatin1String("ls")) command = QStringLiteral("list");
    if (command == QLatin1String("rm")) command = QStringLiteral("remove");

    if (command == QLatin1String("help") || command == QLatin1String("--help") || command == QLatin1String("-h")) {
        std::fputs(kUsage, stdout);
        return kExitOk;
    }
    if (command == QLatin1String("--version") || command == QLatin1String("-v")) {
        std::printf("Orbit %s\n", ORBIT_VERSION);
        return kExitOk;
    }
    static const QStringList commands{
        QStringLiteral("add"), QStringLiteral("list"), QStringLiteral("status"), QStringLiteral("pause"),
        QStringLiteral("resume"), QStringLiteral("cancel"), QStringLiteral("remove"), QStringLiteral("show"),
    };
    if (!commands.contains(command)) return std::nullopt;

    QCoreApplication app(argc, argv);
    QStringList args;
    for (int i = 2; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    return runCommand(command, args);
}

}
