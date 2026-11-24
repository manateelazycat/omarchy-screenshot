// SPDX-License-Identifier: GPL-3.0-only

#include "capturecontroller.h"
#include "daemonsocket.h"

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <cstring>
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <vector>

// forwardCaptureRequest() waits for the daemon's answer, so the client runs
// on its own thread while the test plays the daemon.
static std::future<bool> forwardLater(const std::string &path) {
  return std::async(std::launch::async,
                    [path] { return forwardCaptureRequest(path); });
}

// Drains until the expected number of requests arrived or two seconds pass.
static int takeWithin(int listener, int expected) {
  int taken = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (taken < expected && std::chrono::steady_clock::now() < deadline) {
    pollfd ready{listener, POLLIN, 0};
    poll(&ready, 1, 50);
    taken += takeCaptureRequests(listener);
  }
  return taken;
}

// Fakes hyprctl and grim so a CaptureController captures without a compositor.
static bool installFakeCapture(const QTemporaryDir &temp) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
  auto write = [&](const QString &name, const QByteArray &data,
                   bool executable = false) {
    QFile file(temp.filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
      return false;
    file.close();
    return !executable || file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                              QFile::ExeOwner);
  };
  const QJsonObject monitor{{"name", QGuiApplication::primaryScreen()->name()},
                            {"width", 400}, {"height", 300}, {"scale", 1},
                            {"x", 0}, {"y", 0}};
  QImage screen(400, 300, QImage::Format_RGB32);
  screen.fill(Qt::darkGreen);
  if (!write(QStringLiteral("monitors.json"),
             QJsonDocument(QJsonArray{monitor}).toJson()) ||
      !screen.save(temp.filePath(QStringLiteral("screen.png"))) ||
      !write(QStringLiteral("hyprctl"),
             "#!/bin/sh\n"
             "if [ \"$2\" = monitors ]; then /usr/bin/cat \"$DAEMON_TEST_DIR/monitors.json\"; "
             "elif [ \"$2\" = getoption ]; then printf '{\"bool\":false}'; "
             "elif [ \"$1\" = eval ]; then "
             "case \"$2\" in *true*) printf hidden > \"$DAEMON_TEST_DIR/cursor\";; "
             "*) printf visible > \"$DAEMON_TEST_DIR/cursor\";; esac; printf ok; "
             "else printf '[]'; fi\n",
             true) ||
      !write(QStringLiteral("grim"),
             "#!/bin/sh\n"
             "[ \"$(/usr/bin/cat \"$DAEMON_TEST_DIR/cursor\")\" = hidden ] || exit 1\n"
             "/usr/bin/cat \"$DAEMON_TEST_DIR/screen.png\"\n",
             true))
    return false;
  qputenv("DAEMON_TEST_DIR", QFile::encodeName(temp.path()));
  qputenv("XDG_RUNTIME_DIR", QFile::encodeName(temp.path()));
  qputenv("PATH", QFile::encodeName(temp.path()));
  qputenv("XDG_SESSION_TYPE", "wayland");
  qputenv("HYPRLAND_INSTANCE_SIGNATURE", "test");
  // Keep the in-process capture away from any real compositor.
  qunsetenv("WAYLAND_DISPLAY");
  return true;
}

class DaemonTest : public QObject {
  Q_OBJECT
private slots:
  void socketPathFollowsRuntimeDir() {
    qputenv("XDG_RUNTIME_DIR", "/run/user/4242");
    QCOMPARE(daemonSocketPath(), std::string("/run/user/4242/omarchy-screenshot.sock"));
    qunsetenv("XDG_RUNTIME_DIR");
    QVERIFY(daemonSocketPath().empty());
  }

  void activationNeedsMatchingPid() {
    QCOMPARE(activatedSocket(42, "42", "1"), 3);
    QCOMPARE(activatedSocket(42, "43", "1"), -1);
    QCOMPARE(activatedSocket(42, "42", "0"), -1);
    QCOMPARE(activatedSocket(42, "42x", "1"), -1);
    QCOMPARE(activatedSocket(42, "", "1"), -1);
    QCOMPARE(activatedSocket(42, nullptr, "1"), -1);
    QCOMPARE(activatedSocket(42, "42", nullptr), -1);
  }

  void listensForwardsAndRefusesSecondDaemon() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = dir.filePath(QStringLiteral("daemon.sock")).toStdString();
    QVERIFY(!forwardCaptureRequest(path));

    int listener = -1;
    int lock = -1;
    std::string error;
    QCOMPARE(listenForCaptureRequests(path, &listener, &lock, &error),
             ListenResult::Listening);
    struct stat info;
    QCOMPARE(stat(path.c_str(), &info), 0);
    QCOMPARE(info.st_mode & 0777, mode_t(0600));

    int second = -1;
    int secondLock = -1;
    QCOMPARE(listenForCaptureRequests(path, &second, &secondLock, &error),
             ListenResult::AlreadyRunning);
    QCOMPARE(second, -1);
    QCOMPARE(secondLock, -1);

    auto firstRequest = forwardLater(path);
    auto secondRequest = forwardLater(path);
    QCOMPARE(takeWithin(listener, 2), 2);
    QVERIFY(firstRequest.get());
    QVERIFY(secondRequest.get());
    QCOMPARE(takeCaptureRequests(listener), 0);
    close(listener);
    close(lock);
  }

  // A daemon that exits before accepting leaves the request unanswered, and
  // the client must then capture itself rather than report success.
  void unansweredRequestFallsBack() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = dir.filePath(QStringLiteral("daemon.sock")).toStdString();
    int listener = -1;
    int lock = -1;
    std::string error;
    QCOMPARE(listenForCaptureRequests(path, &listener, &lock, &error),
             ListenResult::Listening);
    auto pending = forwardLater(path);
    pollfd queued{listener, POLLIN, 0};
    QCOMPARE(poll(&queued, 1, 2000), 1);
    close(listener);
    close(lock);
    QVERIFY(!pending.get());
  }

  // A client that gave up waiting has captured by itself, so the daemon must
  // not serve its request when it finally reads it.
  void withdrawnRequestIsNotServed() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = dir.filePath(QStringLiteral("daemon.sock")).toStdString();
    int listener = -1;
    int lock = -1;
    std::string error;
    QCOMPARE(listenForCaptureRequests(path, &listener, &lock, &error),
             ListenResult::Listening);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path.c_str());
    const int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    QCOMPARE(::connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof address), 0);
    QCOMPARE(send(client, "capture\n", 8, MSG_NOSIGNAL), ssize_t(8));
    close(client);
    QCOMPARE(takeCaptureRequests(listener), 0);
    close(listener);
    close(lock);
  }

  // systemd can hold the socket with no daemon behind it; a daemon started
  // by hand must leave it alone without its probe counting as a capture.
  void probeOfAHeldSocketIsNoRequest() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = dir.filePath(QStringLiteral("daemon.sock")).toStdString();
    const int held = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path.c_str());
    QCOMPARE(bind(held, reinterpret_cast<const sockaddr *>(&address), sizeof address), 0);
    QCOMPARE(listen(held, 8), 0);

    int listener = -1;
    int lock = -1;
    std::string error;
    QCOMPARE(listenForCaptureRequests(path, &listener, &lock, &error),
             ListenResult::AlreadyRunning);
    QCOMPARE(takeCaptureRequests(held), 0);

    // Neither a wrong message nor a client that stalls counts.
    const int other = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    QCOMPARE(::connect(other, reinterpret_cast<const sockaddr *>(&address), sizeof address), 0);
    QCOMPARE(send(other, "probe\n", 6, MSG_NOSIGNAL), ssize_t(6));
    const int stalled = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    QCOMPARE(::connect(stalled, reinterpret_cast<const sockaddr *>(&address), sizeof address), 0);
    QCOMPARE(send(stalled, "cap", 3, MSG_NOSIGNAL), ssize_t(3));
    auto request = forwardLater(path);
    QCOMPARE(takeWithin(held, 1), 1);
    QVERIFY(request.get());
    close(other);
    close(stalled);
    close(held);
  }

  // A daemon stuck before accepting fills its backlog. New clients must fall
  // back at once instead of blocking in connect(), and a starting daemon must
  // still treat the busy socket as taken.
  void fullBacklogFallsBackAndIsNotStolen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = dir.filePath(QStringLiteral("daemon.sock")).toStdString();
    const int stuck = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path.c_str());
    QCOMPARE(bind(stuck, reinterpret_cast<const sockaddr *>(&address), sizeof address), 0);
    QCOMPARE(listen(stuck, 1), 0);
    std::vector<int> queued;
    for (int i = 0; i < 16; ++i) {
      const int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      if (::connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0) {
        close(client);
        break;
      }
      queued.push_back(client);
    }
    QVERIFY(queued.size() < 16);

    const auto started = std::chrono::steady_clock::now();
    QVERIFY(!forwardCaptureRequest(path));
    QVERIFY(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    int listener = -1;
    int lock = -1;
    std::string error;
    QCOMPARE(listenForCaptureRequests(path, &listener, &lock, &error),
             ListenResult::AlreadyRunning);
    struct stat info;
    QCOMPARE(stat(path.c_str(), &info), 0);
    for (const int client : queued)
      close(client);
    close(stuck);
  }

  void replacesStaleSocketButNotOtherFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = dir.filePath(QStringLiteral("daemon.sock")).toStdString();
    int listener = -1;
    int lock = -1;
    std::string error;
    QCOMPARE(listenForCaptureRequests(path, &listener, &lock, &error),
             ListenResult::Listening);
    // A crashed daemon leaves its socket file behind; the kernel drops its lock.
    close(listener);
    close(lock);
    QVERIFY(!forwardCaptureRequest(path));
    QCOMPARE(listenForCaptureRequests(path, &listener, &lock, &error),
             ListenResult::Listening);
    auto request = forwardLater(path);
    QCOMPARE(takeWithin(listener, 1), 1);
    QVERIFY(request.get());
    close(listener);
    close(lock);

    const QString other = dir.filePath(QStringLiteral("notes.txt"));
    QFile file(other);
    QVERIFY(file.open(QIODevice::WriteOnly) && file.write("keep") == 4);
    file.close();
    QCOMPARE(listenForCaptureRequests(other.toStdString(), &listener, &lock, &error),
             ListenResult::Failed);
    QVERIFY(QFile::exists(other));
  }

  void resetPreparesTheNextCapture() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QByteArray path = qgetenv("PATH");
    QVERIFY(installFakeCapture(temp));

    CaptureController controller;
    QString error;
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QFile cursorState(temp.filePath(QStringLiteral("cursor")));
    QVERIFY(cursorState.open(QIODevice::ReadOnly));
    QCOMPARE(cursorState.readAll(), QByteArray("visible"));
    cursorState.close();
    const int firstGeneration = controller.captureGeneration();
    controller.pointerPress(0, 10, 10);
    controller.pointerMove(0, 120, 90);
    controller.pointerRelease(0, 120, 90);
    controller.setTool(QStringLiteral("fillrect"));
    controller.pointerPress(0, 20, 20);
    controller.pointerMove(0, 60, 60);
    controller.pointerRelease(0, 60, 60);
    QVERIFY(controller.selected());
    QCOMPARE(controller.annotations().size(), 1);
    QCOMPARE(controller.toolVariants().value(QStringLiteral("rect")).toString(),
             QStringLiteral("fillrect"));

    QSignalSpy pinRequested(&controller, &CaptureController::pinRequested);
    QSignalSpy done(&controller, &CaptureController::done);
    const QImage finished = controller.renderedImage();
    const QRectF selection = controller.selection();
    controller.pin();
    QCOMPARE(pinRequested.count(), 1);
    QCOMPARE(pinRequested.first()[0].value<QImage>(), finished);
    QCOMPARE(pinRequested.first()[1].toRectF(), selection);
    QCOMPARE(done.count(), 0);
    controller.finishPin(false);
    QVERIFY(controller.selected());
    QVERIFY(!controller.status().isEmpty());
    controller.finishPin(true);
    QCOMPARE(done.count(), 1);

    controller.reset();
    QVERIFY(!controller.selected());
    QVERIFY(!controller.imagesReady());
    QVERIFY(controller.selection().isEmpty());
    QVERIFY(controller.annotations().isEmpty());
    QVERIFY(controller.monitors().isEmpty());
    QCOMPARE(pinRequested.first()[0].value<QImage>(), finished);
    QCOMPARE(controller.tool(), QStringLiteral("select"));
    QCOMPARE(controller.toolVariants().value(QStringLiteral("rect")).toString(),
             QStringLiteral("rect"));
    QCOMPARE(controller.scrollState(), int(CaptureController::ScrollState::Idle));

    QVERIFY2(controller.initialize(&error), qPrintable(error));
    QVERIFY(controller.imagesReady());
    QCOMPARE(controller.monitors().size(), 1);
    QVERIFY(controller.captureGeneration() > firstGeneration);
    qputenv("PATH", path);
  }

  // The last plain rectangle marks what gets exported: the crop drops the
  // frame itself while keeping the annotations inside it.
  void rectFrameCropsExport() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QByteArray path = qgetenv("PATH");
    QVERIFY(installFakeCapture(temp));

    CaptureController controller;
    QString error;
    QVERIFY2(controller.initialize(&error), qPrintable(error));
    controller.pointerPress(0, 10, 10);
    controller.pointerMove(0, 120, 90);
    controller.pointerRelease(0, 120, 90);
    QVERIFY(controller.selected());
    controller.setTool(QStringLiteral("fillrect"));
    controller.pointerPress(0, 30, 30);
    controller.pointerMove(0, 50, 50);
    controller.pointerRelease(0, 50, 50);
    controller.setTool(QStringLiteral("rect"));
    controller.pointerPress(0, 20, 20);
    controller.pointerMove(0, 60, 60);
    controller.pointerRelease(0, 60, 60);
    QCOMPARE(controller.annotations().size(), 2);

    const QImage cropped = controller.renderedImage();
    QCOMPARE(cropped.size(), QSize(40, 40));
    // The frame stroke is not painted, the mark inside still is.
    QCOMPARE(cropped.pixelColor(0, 0), QColor(Qt::darkGreen));
    QCOMPARE(cropped.pixelColor(39, 39), QColor(Qt::darkGreen));
    QCOMPARE(cropped.pixelColor(15, 15), QColor(QStringLiteral("#ff4b55")));
    QCOMPARE(cropped.pixelColor(35, 35), QColor(Qt::darkGreen));

    QSignalSpy pinRequested(&controller, &CaptureController::pinRequested);
    controller.pin();
    QCOMPARE(pinRequested.count(), 1);
    QCOMPARE(pinRequested.first()[0].value<QImage>(), cropped);
    QCOMPARE(pinRequested.first()[1].toRectF(), QRectF(20, 20, 40, 40));

    // A newer frame wins and the older rectangle stays an ordinary mark.
    controller.pointerPress(0, 25, 25);
    controller.pointerMove(0, 80, 70);
    controller.pointerRelease(0, 80, 70);
    QCOMPARE(controller.annotations().size(), 3);
    QCOMPARE(controller.renderedImage().size(), QSize(55, 45));

    controller.undo();
    QCOMPARE(controller.renderedImage().size(), QSize(40, 40));
    controller.undo();
    QCOMPARE(controller.renderedImage().size(), QSize(110, 80));
    qputenv("PATH", path);
  }
};

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  app.setOrganizationName(QStringLiteral("OmarchyTest"));
  app.setApplicationName(QStringLiteral("daemon-test"));
  DaemonTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "daemon.moc"
