// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

#include "capturecontroller.h"
#include "daemonsocket.h"
#include "longimageitem.h"
#include "pinnedimages.h"
#include "pinnedwindows.h"
#include "virtualpointer.h"
#include "selftestimage.h"

#include <LayerShellQt/window.h>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickView>
#include <QRegion>
#include <QProcess>
#include <QSettings>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>

#include <functional>
#include <chrono>
#include <cmath>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

class CaptureImageProvider final : public QQuickImageProvider {
public:
  explicit CaptureImageProvider(const CaptureController *controller)
      : QQuickImageProvider(QQuickImageProvider::Image),
        m_controller(controller) {}

  QImage requestImage(const QString &id, QSize *size,
                      const QSize &requestedSize) override {
    // "<kind>/<screen>/<generation>"; the generation only defeats caching.
    const QStringList parts = id.split(QLatin1Char('/'));
    if (parts.size() < 2)
      return {};
    bool valid = false;
    const int index = parts[1].toInt(&valid);
    if (!valid || index < 0)
      return {};
    const QImage *image = nullptr;
    if (index >= m_controller->monitors().size())
      return {};
    if (parts[0] == QStringLiteral("screen"))
      image = &m_controller->monitors()[index].image;
    else if (parts[0] == QStringLiteral("mosaic"))
      image = &m_controller->monitors()[index].mosaicImage;
    if (!image)
      return {};
    if (size)
      *size = image->size();
    if (requestedSize.isValid() && !image->isNull())
      return image->scaled(requestedSize, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
    return *image;
  }

private:
  const CaptureController *m_controller;
};

static bool startScrollFixture(CaptureController &controller) {
  QProcess clients;
  clients.start(QStringLiteral("hyprctl"),
                {QStringLiteral("-j"), QStringLiteral("clients")});
  if (!clients.waitForFinished(3000))
    return false;
  QRectF window;
  const auto entries =
      QJsonDocument::fromJson(clients.readAllStandardOutput()).array();
  for (const auto &entry : entries) {
    const auto item = entry.toObject();
    if (!item.value(QStringLiteral("title")).toString()
             .startsWith(QStringLiteral("Omarchy Scroll Fixture")))
      continue;
    const auto at = item.value(QStringLiteral("at")).toArray();
    const auto size = item.value(QStringLiteral("size")).toArray();
    if (at.size() == 2 && size.size() == 2)
      window = QRectF(at[0].toDouble(), at[1].toDouble(),
                      size[0].toDouble(), size[1].toDouble());
    break;
  }
  int screen = -1;
  for (int i = 0; i < controller.monitors().size(); ++i)
    if (controller.monitors()[i].geometry.contains(window.center()))
      screen = i;
  if (window.isEmpty() || screen < 0)
    return false;
  const QRectF target = window.adjusted(20, 55, -20, -20);
  const QPointF origin = controller.monitors()[screen].geometry.topLeft();
  controller.pointerPress(screen, target.left() - origin.x(),
                          target.top() - origin.y());
  controller.pointerMove(screen, target.right() - origin.x(),
                         target.bottom() - origin.y());
  controller.pointerRelease(screen, target.right() - origin.x(),
                            target.bottom() - origin.y());
  controller.startScroll();
  if (controller.scrollState() == int(CaptureController::ScrollState::Choosing)) {
    for (const QVariant &candidate : controller.scrollCandidates()) {
      const auto item = candidate.toMap();
      if (item.value(QStringLiteral("title")).toString()
              .startsWith(QStringLiteral("Omarchy Scroll Fixture"))) {
        controller.chooseScrollWindow(item.value(QStringLiteral("index")).toInt());
        break;
      }
    }
  }
  return controller.scrollState() == int(CaptureController::ScrollState::Capturing);
}

static QQuickItem *findQuickItem(QQuickItem *root, const QString &name) {
  if (root->objectName() == name)
    return root;
  for (auto *child : root->childItems())
    if (auto *found = findQuickItem(child, name))
      return found;
  return nullptr;
}

namespace {
int signalPipe[2] = {-1, -1};

void requestQuit(int) {
  const char byte = 0;
  [[maybe_unused]] const ssize_t written = write(signalPipe[1], &byte, 1);
}
} // namespace

int main(int argc, char **argv) {
  // A plain launch hands the capture to the resident daemon, or to the
  // systemd socket that starts one, and exits before Qt starts. A standalone
  // process with pinned images also listens until its last pin is closed.
  if (argc == 1 && forwardCaptureRequest(daemonSocketPath()))
    return 0;
  QGuiApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  app.setApplicationName(QStringLiteral("omarchy-screenshot"));
  app.setOrganizationName(QStringLiteral("Omarchy"));

  // Parse once before translation so even --help uses the requested language.
  QList<QCommandLineOption> options = {
      {QStringLiteral("language"), QString(), QStringLiteral("locale")}};
  for (const QString &test :
       {QStringLiteral("self-test"), QStringLiteral("ui-self-test"),
        QStringLiteral("scroll-stitch-test"),
        QStringLiteral("scroll-ui-self-test"),
        QStringLiteral("scroll-integration-test"),
        QStringLiteral("scroll-ui-integration-test"),
        QStringLiteral("pin-ui-self-test")}) {
    options.append(QCommandLineOption(test));
    options.last().setFlags(QCommandLineOption::HiddenFromHelp);
  }
  // Resident mode; the README documents both options.
  options.append(QCommandLineOption(QStringLiteral("daemon")));
  options.append(QCommandLineOption(QStringLiteral("idle-timeout"), QString(),
                                    QStringLiteral("seconds")));
  for (int i = options.size() - 2; i < options.size(); ++i)
    options[i].setFlags(QCommandLineOption::HiddenFromHelp);
  QCommandLineParser languageParser;
  languageParser.addHelpOption();
  languageParser.addOptions(options);
  languageParser.parse(app.arguments());
  const QString requestedLanguage = languageParser.isSet(QStringLiteral("language"))
      ? languageParser.value(QStringLiteral("language"))
      : qEnvironmentVariable("OMARCHY_SCREENSHOT_LANGUAGE");
  const QLocale requestedLocale = requestedLanguage.isEmpty()
      ? QLocale::system() : QLocale(requestedLanguage);
  QTranslator translator;
  bool translated = false;
  for (const QString &language : requestedLocale.uiLanguages()) {
    const QLocale locale(language);
    // English is the source language, not a missing catalog to skip over.
    if (locale.language() == QLocale::English || locale.language() == QLocale::C)
      break;
    if (translator.load(locale, QStringLiteral("omarchy-screenshot"),
                        QStringLiteral("_"), QStringLiteral(":/i18n"))) {
      translated = true;
      break;
    }
  }
  if (translated)
    app.installTranslator(&translator);
  const QLocale uiLocale = translated ? QLocale(translator.language())
                                     : QLocale(QLocale::English);
  QLocale::setDefault(uiLocale);
  QGuiApplication::setLayoutDirection(uiLocale.textDirection());

  options[0].setDescription(QCoreApplication::translate(
      "main", "Interface language (for example de, pt_BR or zh_TW)."));
  QCommandLineParser parser;
  parser.setApplicationDescription(QCoreApplication::translate(
      "main", "Capture and annotate screenshots on Hyprland."));
  parser.addHelpOption();
  parser.addOptions(options);
  parser.process(app);

  if (app.arguments().contains(QStringLiteral("--scroll-stitch-test"))) {
    ScrollStitcher stitcher;
    auto frameAt = [](int offset) {
      QImage image(240, 320, QImage::Format_RGB32);
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
          const int contentY = y + offset;
          const QRgb color = y < 20 ? qRgb(25, 45, 65)
                             : y >= 304 ? qRgb(80, 50, 25)
                             : qRgb((contentY * 37 + x * 13) % 256,
                                    (contentY * 19 + x * 7) % 256,
                                    (contentY * 11 + x * 23) % 256);
          image.setPixel(x, y, color);
        }
      return image;
    };
    const QImage first = frameAt(0);
    const auto firstResult = stitcher.append(first);
    const auto unchanged = stitcher.append(first);
    const auto secondResult = stitcher.append(frameAt(96));
    const int secondHeight = stitcher.image().height();
    const auto thirdResult = stitcher.append(frameAt(192));
    const int thirdHeight = stitcher.image().height();
    if (firstResult != ScrollStitcher::Result::Added ||
        unchanged != ScrollStitcher::Result::Unchanged ||
        secondResult != ScrollStitcher::Result::Added ||
        secondHeight != 416 ||
        thirdResult != ScrollStitcher::Result::Added ||
        thirdHeight != 512 ||
        stitcher.image().pixel(80, 400) != frameAt(192).pixel(80, 208)) {
      QTextStream(stderr) << "Scroll stitcher failed: "
                          << int(firstResult) << "," << int(unchanged)
                          << "," << int(secondResult) << "," << secondHeight
                          << "," << int(thirdResult) << "," << thirdHeight
                          << "\n";
      return 2;
    }
    ScrollStitcher sideStitcher;
    const auto sidebarFrame = [&](int offset) {
      QImage image = frameAt(offset);
      for (int y = 20; y < 304; ++y)
        for (int x = 0; x < 24; ++x)
          image.setPixel(x, y,
                         qRgb((y * 7) % 256, (y * 11) % 256,
                              (y * 17) % 256));
      return image;
    };
    if (sideStitcher.append(sidebarFrame(0)) !=
            ScrollStitcher::Result::Added ||
        sideStitcher.append(sidebarFrame(96)) !=
            ScrollStitcher::Result::Added ||
        sideStitcher.image().pixel(8, 340) !=
            sideStitcher.image().pixel(8, 350)) {
      QTextStream(stderr) << "Scroll sidebar de-duplication failed\n";
      return 2;
    }
    ScrollStitcher webStitcher;
    const auto webFrame = [](int offset, int phase) {
      QImage image(360, 520, QImage::Format_RGB32);
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
          const int contentY = y + offset;
          const QRgb color = y < 90
              ? qRgb((x + phase * 71) % 256, 35, 50)
              : x < 78
              ? qRgb(205, (y * 3) % 256, 210)
              : qRgb((contentY * 37 + x * 13) % 256,
                     (contentY * 19 + x * 7) % 256,
                     (contentY * 11 + x * 23) % 256);
          image.setPixel(x, y, color);
        }
      return image;
    };
    if (webStitcher.append(webFrame(0, 0)) !=
            ScrollStitcher::Result::Added ||
        webStitcher.append(webFrame(148, 1)) !=
            ScrollStitcher::Result::Added ||
        webStitcher.lastShift() != 148 ||
        webStitcher.append(webFrame(148, 2)) !=
            ScrollStitcher::Result::Unchanged ||
        webStitcher.image().height() != 668) {
      QTextStream(stderr) << "Scroll animated header/sidebar match failed\n";
      return 2;
    }
    ScrollStitcher textStitcher;
    const auto textRows = [](int offset) {
      QImage image(240, 960, QImage::Format_RGB32);
      image.fill(qRgb(240, 240, 240));
      for (int y = 0; y < image.height(); ++y) {
        const int contentY = y + offset;
        if (contentY % 20 < 5 || contentY % 20 > 14)
          continue;
        for (int x = 0; x < image.width(); ++x) {
          const int value = (contentY * 37 + (x / 6) * 71) % 210;
          image.setPixel(x, y, x % 6 < 3 ? qRgb(value, value, value)
                                        : qRgb(240, 240, 240));
        }
      }
      return image;
    };
    if (textStitcher.append(textRows(0)) != ScrollStitcher::Result::Added ||
        textStitcher.append(textRows(80)) != ScrollStitcher::Result::Added ||
        textStitcher.lastShift() != 80 ||
        textStitcher.append(textRows(80)) != ScrollStitcher::Result::Unchanged) {
      QTextStream(stderr) << "Scroll regular text lines mistaken for unchanged content\n";
      return 2;
    }
    ScrollStitcher hintStitcher;
    const QImage cleanStart = webFrame(0, 0);
    const auto frameWithHint = [&](int offset) {
      QImage image = webFrame(offset, 1);
      QPainter painter(&image);
      painter.fillRect(QRect(178, 8, 174, 44), QColor("#26313d"));
      return image;
    };
    if (hintStitcher.append(cleanStart) != ScrollStitcher::Result::Added ||
        hintStitcher.append(frameWithHint(148)) != ScrollStitcher::Result::Added ||
        hintStitcher.append(frameWithHint(296)) != ScrollStitcher::Result::Added ||
        hintStitcher.image().height() != 816 ||
        hintStitcher.image().pixel(200, 20) != cleanStart.pixel(200, 20) ||
        hintStitcher.image().pixel(200, 800) != webFrame(296, 1).pixel(200, 504)) {
      QTextStream(stderr) << "Scroll stop hint leaked into the long image\n";
      return 2;
    }
    QTextStream(stdout) << "Scroll stitcher overlap, fixed bands, regular text and hint exclusion OK\n";
    return 0;
  }

  // Claim the request socket before capturing anything, so a second daemon
  // leaves quietly instead of flashing a capture nobody asked for.
  const bool daemon = parser.isSet(QStringLiteral("daemon"));
  bool idleTimeoutValid = true;
  const int idleTimeout =
      parser.value(QStringLiteral("idle-timeout")).toInt(&idleTimeoutValid);
  if (parser.isSet(QStringLiteral("idle-timeout")) &&
      (!idleTimeoutValid || idleTimeout < 0)) {
    qCritical() << "--idle-timeout expects a number of seconds";
    return 1;
  }
  int listener = -1;
  // Held until exit; it makes this process the socket's only owner.
  int socketLock = -1;
  std::string ownedSocket;
  if (daemon) {
    listener = activatedSocket();
    if (listener < 0) {
      const std::string path = daemonSocketPath();
      std::string listenError;
      switch (listenForCaptureRequests(path, &listener, &socketLock,
                                       &listenError)) {
      case ListenResult::AlreadyRunning:
        QTextStream(stdout) << "A daemon is already listening on "
                            << QString::fromStdString(path) << '\n';
        return 0;
      case ListenResult::Failed:
        qCritical().noquote() << QString::fromStdString(listenError);
        return 1;
      case ListenResult::Listening:
        ownedSocket = path;
        break;
      }
    }
  }
  // Stop requests (systemctl stop, Ctrl+C) must clean up even while the
  // daemon is still starting; a signal that arrives before the event loop
  // waits in the pipe until it runs.
  std::unique_ptr<QSocketNotifier> quitSignals;
  if (daemon &&
      // Nonblocking, so a burst of signals can never stall the handler; one
      // unread byte is enough to wake the event loop.
      socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                 signalPipe) == 0) {
    quitSignals = std::make_unique<QSocketNotifier>(signalPipe[0],
                                                    QSocketNotifier::Read);
    QObject::connect(quitSignals.get(), &QSocketNotifier::activated, &app,
                     &QCoreApplication::quit);
    for (const int signal : {SIGTERM, SIGINT, SIGHUP})
      std::signal(signal, requestQuit);
  }
  struct SocketCleanup {
    const std::string &path;
    ~SocketCleanup() {
      if (!path.empty())
        unlink(path.c_str());
    }
  } socketCleanup{ownedSocket};

  std::unique_ptr<QTemporaryDir> testSettings;
  if (app.arguments().contains(QStringLiteral("--self-test")) ||
      app.arguments().contains(QStringLiteral("--ui-self-test")) ||
      app.arguments().contains(QStringLiteral("--scroll-ui-self-test")) ||
      app.arguments().contains(QStringLiteral("--pin-ui-self-test")) ||
      app.arguments().contains(QStringLiteral("--scroll-ui-integration-test"))) {
    testSettings = std::make_unique<QTemporaryDir>();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       testSettings->path());
  }

  CaptureController controller;
  QString error;
  // Capturing continues on a worker thread while the overlay loads below.
  if (!controller.startCapture(&error)) {
    qCritical().noquote() << error;
    return 1;
  }

  if (app.arguments().contains(QStringLiteral("--scroll-integration-test"))) {
    // The fixture is found among window candidates, which arrive with the
    // capture.
    if (!controller.finishCapture(&error)) {
      qCritical().noquote() << error;
      return 1;
    }
    if (!startScrollFixture(controller)) {
      QTextStream(stderr) << "Scroll fixture window is missing or cannot start\n";
      return 2;
    }
    const auto waitFor = [&](const std::function<bool()> &ready, int timeout = 10000) {
      QElapsedTimer elapsed;
      elapsed.start();
      while (!ready() && elapsed.elapsed() < timeout)
        QTest::qWait(30);
      return ready();
    };
    if (!waitFor([&] {
          return controller.scrollHeight() > 0 ||
                 controller.scrollState() ==
                     int(CaptureController::ScrollState::Reviewing);
        })) {
      qCritical() << "Fixture capture did not start" << controller.status();
      return 2;
    }
    const int frameHeight = controller.scrollHeight();
    if (!waitFor([&] {
          return controller.scrollHeight() > frameHeight + 70 ||
                 controller.scrollState() ==
                     int(CaptureController::ScrollState::Reviewing);
        }) ||
        controller.scrollHeight() <= frameHeight + 70) {
      qCritical() << "Fixture did not scroll" << controller.status()
                  << controller.scrollHeight();
      QTextStream(stderr) << "No growth: " << controller.scrollHeight()
                          << " state " << controller.scrollState()
                          << " status " << controller.status() << "\n";
      return 2;
    }
    controller.pauseScroll();
    if (!waitFor([&] { return controller.scrollState() ==
                               int(CaptureController::ScrollState::Reviewing); })) {
      qCritical() << "Fixture pause did not enter review" << controller.status();
      return 2;
    }
    const int firstHeight = controller.scrollHeight();
    controller.setTool(QStringLiteral("marker"));
    controller.pointerPress(-1, 45, 45);
    controller.pointerRelease(-1, 45, 45);
    controller.resumeScroll();
    if (!waitFor([&] {
          return controller.scrollHeight() > firstHeight + 70 ||
                 controller.scrollState() ==
                     int(CaptureController::ScrollState::Reviewing);
        }) ||
        controller.scrollHeight() <= firstHeight + 70) {
      qCritical() << "Fixture resume did not extend image"
                  << controller.status() << controller.scrollHeight();
      QTextStream(stderr) << "Resume failed: " << controller.scrollHeight()
                          << " state " << controller.scrollState()
                          << " status " << controller.status() << "\n";
      return 2;
    }
    controller.pauseScroll();
    if (!waitFor([&] { return controller.scrollState() ==
                               int(CaptureController::ScrollState::Reviewing); }) ||
        controller.annotations().size() != 1 ||
        qAbs(controller.selection().bottom() - controller.scrollHeight()) > 1 ||
        controller.renderedImage().isNull()) {
      qCritical() << "Fixture review lost crop or annotations";
      return 2;
    }
    QTextStream(stdout) << "Live scroll, resume, crop extension and annotations OK: "
                        << firstHeight << " -> " << controller.scrollHeight()
                        << " px\n";
    return 0;
  }

  if (app.arguments().contains(QStringLiteral("--self-test"))) {
    if (!controller.finishCapture(&error)) {
      qCritical().noquote() << error;
      return 1;
    }
    if (controller.monitors().size() < 2) {
      qCritical() << "Cross-monitor self-test needs two displays";
      return 2;
    }
    const auto &first = controller.monitors()[0].geometry;
    const auto &second = controller.monitors()[1].geometry;
    controller.pointerPress(0, first.width() * .2, first.height() * .2);
    controller.pointerMove(1, second.width() * .7, second.height() * .7);
    controller.pointerRelease(1, second.width() * .7, second.height() * .7);
    const auto image = controller.renderedImage();
    if (!controller.selected() || image.isNull() || image.width() < 2 ||
        image.height() < 2) {
      qCritical() << "Cross-monitor selection or composition failed";
      return 2;
    }
    for (int i = 0; i < 2; ++i) {
      const QRectF part =
          controller.selection().intersected(controller.monitors()[i].geometry);
      if (part.isEmpty()) {
        qCritical() << "Selection missed monitor" << i;
        return 2;
      }
      const QPointF center = part.center() - controller.selection().topLeft();
      const int sampleX = qBound(
          0,
          qRound(center.x() * image.width() / controller.selection().width()),
          image.width() - 1);
      const int sampleY = qBound(
          0,
          qRound(center.y() * image.height() / controller.selection().height()),
          image.height() - 1);
      if (image.pixelColor(sampleX, sampleY).alpha() == 0) {
        qCritical() << "Transparent pixels in monitor" << i;
        return 2;
      }
    }
    controller.setTool(QStringLiteral("mosaic"));
    controller.pointerPress(0, first.width() * .25, first.height() * .25);
    controller.pointerMove(0, first.width() * .5, first.height() * .45);
    controller.pointerRelease(0, first.width() * .5, first.height() * .45);
    if (controller.annotations().size() != 1 ||
        controller.renderedImage().size() != image.size()) {
      qCritical() << "Mosaic composition failed";
      return 2;
    }
    QVariantList jitter;
    for (const QPointF &point :
         {QPointF(0, 0), QPointF(10, 10), QPointF(20, -10), QPointF(30, 10),
          QPointF(40, 0)})
      jitter.append(QVariantMap{{QStringLiteral("x"), point.x()},
                                {QStringLiteral("y"), point.y()}});
    const auto smoothed = controller.smoothedFreehandPoints(jitter);
    const auto path = CaptureController::freehandPath(jitter);
    if (smoothed.size() != jitter.size() ||
        smoothed[1].toMap().value(QStringLiteral("y")).toDouble() >= 10 ||
        smoothed.first().toMap() != jitter.first().toMap() ||
        smoothed.last().toMap() != jitter.last().toMap() ||
        path.elementCount() <= jitter.size() ||
        path.elementAt(1).type != QPainterPath::CurveToElement) {
      qCritical() << "Freehand smoothing failed";
      return 2;
    }
    controller.undo();
    if (!controller.annotations().isEmpty()) {
      qCritical() << "Undo failed";
      return 2;
    }
    controller.redo();
    if (controller.annotations().size() != 1) {
      qCritical() << "Redo failed";
      return 2;
    }
    controller.undo();
    controller.setTool(QStringLiteral("line"));
    controller.pointerPress(0, first.width() * .25, first.height() * .25);
    controller.pointerMove(0, first.width() * .5, first.height() * .4);
    controller.pointerRelease(0, first.width() * .5, first.height() * .4);
    controller.redo();
    if (controller.annotations().size() != 1 ||
        controller.annotations().first().toMap().value(
            QStringLiteral("type")) != QStringLiteral("line")) {
      qCritical() << "Redo history was not cleared after drawing";
      return 2;
    }
    controller.setTool(QStringLiteral("marker"));
    controller.pointerPress(0, first.width() * .3, first.height() * .3);
    controller.pointerRelease(0, first.width() * .3, first.height() * .3);
    if (controller.annotations().size() != 2 ||
        controller.annotations()
                .last()
                .toMap()
                .value(QStringLiteral("number"))
                .toInt() != 1) {
      qCritical() << "Numbered marker failed";
      return 2;
    }
    for (const auto &variant :
         {QPair(QStringLiteral("rect"), QStringLiteral("fillrect")),
          QPair(QStringLiteral("ellipse"), QStringLiteral("fillellipse")),
          QPair(QStringLiteral("ellipse"), QStringLiteral("spotlight")),
          QPair(QStringLiteral("arrow"), QStringLiteral("doublearrow")),
          QPair(QStringLiteral("arrow"), QStringLiteral("line")),
          QPair(QStringLiteral("pen"), QStringLiteral("highlighter"))}) {
      controller.setTool(variant.second);
      controller.setTool(QStringLiteral("text"));
      controller.activateToolGroup(variant.first);
      if (controller.tool() != variant.second ||
          controller.toolVariants().value(variant.first) != variant.second) {
        qCritical() << "Grouped tool selection failed" << variant.first;
        return 2;
      }
    }
    QTextStream(stdout)
        << "Cross-monitor capture OK: " << image.width() << "x"
        << image.height()
        << ", curve smoothing, grouped tools, redo and marker OK\n";
    return 0;
  }

  qmlRegisterType<LongImageItem>("ScreenshotInternals", 1, 0,
                                 "LongImageItem");
  QQmlEngine engine;
  QObject::connect(&engine, &QQmlEngine::warnings, &app,
                   [](const QList<QQmlError> &warnings) {
                     for (const auto &warning : warnings)
                       QTextStream(stderr) << warning.toString() << '\n';
                   });
  engine.rootContext()->setContextProperty(QStringLiteral("captureController"),
                                           &controller);
  engine.addImageProvider(QStringLiteral("captures"),
                          new CaptureImageProvider(&controller));
  const bool uiTest = app.arguments().contains(QStringLiteral("--ui-self-test"));
  const bool pinUiTest = app.arguments().contains(QStringLiteral("--pin-ui-self-test"));
  PinnedImages pins;
  PinnedWindows pinWindows(&engine, &pins);

  std::vector<std::unique_ptr<QQuickView>> views;
  auto createView = [&](int i) -> std::unique_ptr<QQuickView> {
    const auto &monitor = controller.monitors()[i];
    auto view = std::make_unique<QQuickView>(&engine, nullptr);
    // Qt Quick uses depth testing to order opaque images, including mosaic
    // patches above the captured screen. Keep its default depth/stencil buffer.
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->setColor(Qt::transparent);
    view->setFlags(Qt::FramelessWindowHint);
    view->setScreen(monitor.screen);
    view->setInitialProperties(
        {{QStringLiteral("screenIndex"), i},
         {QStringLiteral("screenRect"), monitor.geometry}});

    auto *layer = LayerShellQt::Window::get(view.get());
    layer->setScope(QStringLiteral("omarchy-screenshot"));
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setAnchors(
        LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) |
        LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft |
        LayerShellQt::Window::AnchorRight);
    // Ignore the bar's reserved area: the frozen image uses the full output.
    layer->setExclusiveZone(-1);
    layer->setKeyboardInteractivity(
        uiTest ? (i == 0 ? LayerShellQt::Window::KeyboardInteractivityExclusive
                         : LayerShellQt::Window::KeyboardInteractivityNone)
               : LayerShellQt::Window::KeyboardInteractivityOnDemand);
    layer->setScreen(monitor.screen);

    view->setSource(
        QUrl(QStringLiteral("qrc:/qt/qml/OmarchyScreenshot/Overlay.qml")));
    if (view->status() != QQuickView::Ready) {
      qCritical() << "Cannot load Overlay.qml for" << monitor.name;
      return {};
    }
    return view;
  };
  for (int i = 0; i < controller.monitors().size(); ++i) {
    auto view = createView(i);
    if (!view)
      return 1;
    views.push_back(std::move(view));
  }
  if (!controller.finishCapture(&error)) {
    qCritical().noquote() << error;
    return 1;
  }
  for (int i = 0; i < controller.monitors().size(); ++i) {
    const auto &monitor = controller.monitors()[i];
    const auto &view = views[i];
    const auto *root = view->rootObject();
    const auto *image =
        root->findChild<QObject *>(QStringLiteral("screenCaptureImage"));
    const QString expectedSource =
        QStringLiteral("image://captures/screen/%1/%2")
            .arg(i)
            .arg(controller.captureGeneration());
    if (root->property("screenIndex").toInt() != i || !image ||
        image->property("source").toUrl().toString() != expectedSource) {
      qCritical() << "Wrong image source for" << monitor.name;
      return 1;
    }
    if (!daemon)
      view->show();
  }
  if (!views.empty() && !daemon)
    (uiTest ? views.front() : views.back())->requestActivate();

  std::unique_ptr<QQuickView> scrollBar;
  std::unique_ptr<QQuickView> longView;
  auto scrollScreen = [&]() -> QScreen * {
    for (const auto &monitor : controller.monitors())
      if (monitor.geometry.contains(controller.scrollRegion().center()))
        return monitor.screen;
    return controller.monitors().first().screen;
  };
  auto scrollScreenRect = [&]() {
    for (const auto &monitor : controller.monitors())
      if (monitor.screen == scrollScreen())
        return monitor.geometry;
    return controller.monitors().first().geometry;
  };
  auto makeScrollView = [&](const QString &file) {
    auto view = std::make_unique<QQuickView>(&engine, nullptr);
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->setColor(Qt::transparent);
    view->setFlags(Qt::FramelessWindowHint);
    view->setScreen(scrollScreen());
    auto *layer = LayerShellQt::Window::get(view.get());
    layer->setScope(QStringLiteral("omarchy-screenshot"));
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setExclusiveZone(-1);
    layer->setScreen(scrollScreen());
    layer->setAnchors(
        LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) |
        LayerShellQt::Window::AnchorBottom |
        LayerShellQt::Window::AnchorLeft |
        LayerShellQt::Window::AnchorRight);
    const bool capturing = file == QStringLiteral("ScrollCapture.qml");
    layer->setKeyboardInteractivity(
        capturing ? LayerShellQt::Window::KeyboardInteractivityNone
                  : LayerShellQt::Window::KeyboardInteractivityOnDemand);
    if (capturing)
      view->setInitialProperties(
          {{QStringLiteral("screenRect"), scrollScreenRect()}});
    view->setSource(QUrl(QStringLiteral("qrc:/qt/qml/OmarchyScreenshot/") +
                         file));
    if (view->status() != QQuickView::Ready)
      qWarning() << "Cannot load" << file;
    return view;
  };
  bool scrollInputPassThrough = false;
  bool scrollInputFocusPending = false;
  auto updateScrollInputMask = [&] {
    if (!scrollBar || !scrollBar->rootObject())
      return;
    QRegion mask(scrollBar->rootObject()
                     ->property("toolbarRect").toRectF().toAlignedRect());
    if (!scrollInputPassThrough)
      mask += controller.scrollRegion()
                  .translated(-scrollScreenRect().topLeft()).toAlignedRect();
    scrollBar->setMask(mask);
    scrollBar->update();
  };
  QObject::connect(&controller, &CaptureController::scrollStateChanged, &app,
                   [&] {
    const auto state = controller.scrollState();
    if (state == int(CaptureController::ScrollState::Capturing)) {
      scrollInputPassThrough = false;
      scrollInputFocusPending = true;
      for (auto &view : views)
        view->hide();
      if (longView)
        longView->hide();
      if (!scrollBar) {
        scrollBar = makeScrollView(QStringLiteral("ScrollCapture.qml"));
        const auto resized = [&] {
          QTimer::singleShot(0, &app, updateScrollInputMask);
        };
        QObject::connect(scrollBar.get(), &QWindow::widthChanged, &app, resized);
        QObject::connect(scrollBar.get(), &QWindow::heightChanged, &app, resized);
        QObject::connect(scrollBar.get(), &QQuickWindow::frameSwapped, &app, [&] {
          if (scrollInputFocusPending && !scrollInputPassThrough) {
            scrollInputFocusPending = false;
            // A submitted frame can still wait on a compositor FIFO barrier.
            // Allow its input region to become current before hit testing it.
            QTimer::singleShot(30, &app, [&] {
              if (!scrollInputPassThrough)
                controller.restoreScrollInputFocus();
            });
          }
        });
      }
      if (scrollBar->status() == QQuickView::Ready)
        scrollBar->show();
      updateScrollInputMask();
    } else if (state == int(CaptureController::ScrollState::Reviewing)) {
      if (scrollBar)
        scrollBar->hide();
      if (!longView)
        longView = makeScrollView(QStringLiteral("LongOverlay.qml"));
      if (longView->status() == QQuickView::Ready) {
        longView->show();
        longView->requestActivate();
      }
    } else {
      if (scrollBar)
        scrollBar->hide();
      if (longView)
        longView->hide();
      // A resident daemon resetting for the next capture has no screenshot
      // to return to.
      if (controller.imagesReady())
        for (auto &view : views)
          view->show();
    }
  });
  // Unmap whichever overlay is showing as soon as an export starts. Success
  // quits with it still hidden; a failure reports through the status line,
  // which brings back only what the export hid, so scroll capture's own
  // status messages never reveal overlays it hid on purpose.
  std::vector<QWindow *> hiddenForExport;
  if (!uiTest) {
    QObject::connect(&controller, &CaptureController::exportingChanged, &app,
                     [&] {
                       if (!controller.exporting())
                         return;
                       for (const auto &view : views)
                         if (view->isVisible())
                           hiddenForExport.push_back(view.get());
                       if (longView && longView->isVisible())
                         hiddenForExport.push_back(longView.get());
                       for (QWindow *window : hiddenForExport)
                         window->hide();
                     });
    QObject::connect(&controller, &CaptureController::statusChanged, &app,
                     [&] {
                       if (hiddenForExport.empty())
                         return;
                       for (QWindow *window : hiddenForExport)
                         window->show();
                       hiddenForExport.back()->requestActivate();
                       hiddenForExport.clear();
                     });
  }
  // Keep click interception active during capture. Only release the input
  // region briefly while delivering the virtual wheel to the underlying app.
  QObject::connect(&controller, &CaptureController::scrollInputAboutToSend,
                   &app, [&] {
    scrollInputPassThrough = true;
    scrollInputFocusPending = false;
    updateScrollInputMask();
  });
  QObject::connect(&controller, &CaptureController::scrollInputSent,
                   &app, [&] {
    scrollInputPassThrough = false;
    scrollInputFocusPending = true;
    updateScrollInputMask();
  });
  QObject::connect(&controller, &CaptureController::scrollAwaitingPaneChanged,
                   &app, [&] { QTimer::singleShot(0, &app, updateScrollInputMask); });

  // Resident mode keeps the engine, views and graphics state alive between
  // captures, so a request only has to capture and show. Exiting is the only
  // way to return the driver's GPU buffers, so an idle daemon can quit and
  // let systemd start a fresh one on the next request.
  std::unique_ptr<QSocketNotifier> requests;
  QTimer idleTimer;
  idleTimer.setSingleShot(true);
  idleTimer.setInterval(std::chrono::seconds(idleTimeout));
  // A press during an export asks for the next capture; it waits for done().
  bool captureQueued = false;
  // The connections below call these, so they live as long as main().
  auto overlayVisible = [&] {
    return !views.empty() && views.back()->isVisible();
  };
  // A capture lasts until it is copied, saved or cancelled, and scrolling
  // capture or an export hides the overlay along the way.
  auto capturing = [&] {
    return overlayVisible() || controller.exporting() ||
           controller.scrollState() !=
               int(CaptureController::ScrollState::Idle);
  };
  auto becomeIdle = [&] {
    for (const auto &view : views)
      view->hide();
    // Scroll views are rare and bound to one output, so they are rebuilt
    // on use. done() can come from their own key handler, hence later.
    if (scrollBar)
      scrollBar.release()->deleteLater();
    if (longView)
      longView.release()->deleteLater();
    // reset() clears the status, which must not reveal what an export hid.
    hiddenForExport.clear();
    controller.saveAnnotationColor();
    controller.reset();
    pinWindows.setSuspended(false);
    if (!daemon && pins.count() == 0 && !captureQueued && !pinUiTest)
      QCoreApplication::quit();
    else if (daemon && idleTimeout > 0 && pins.count() == 0)
      idleTimer.start();
  };
  auto showOverlay = [&] {
    idleTimer.stop();
    pinWindows.setSuspended(true);
    const auto &monitors = controller.monitors();
    for (int i = 0; i < monitors.size(); ++i) {
      QQuickItem *root = views[i]->rootObject();
      root->setProperty("screenRect", monitors[i].geometry);
      QMetaObject::invokeMethod(root, "resetForCapture");
      views[i]->show();
    }
    views.back()->requestActivate();
  };
  auto captureAndShow = [&] {
    if (capturing()) {
      if (controller.exporting())
        captureQueued = true;
      else if (overlayVisible())
        views.back()->requestActivate();
      return;
    }
    pinWindows.setSuspended(true);
    // Commit the hidden pin surfaces before taking the next screenshot.
    QGuiApplication::sync();
    controller.reset();
    QString captureError;
    if (!controller.startCapture(&captureError) ||
        !controller.finishCapture(&captureError)) {
      qWarning().noquote() << captureError;
      becomeIdle();
      return;
    }
    // Outputs can change while the daemon waits; views follow the screens.
    const auto &monitors = controller.monitors();
    if (pins.count() > 0 && !pinWindows.setMonitors(monitors)) {
      qWarning() << "Cannot update pinned screenshot outputs";
      becomeIdle();
      return;
    }
    bool sameOutputs = views.size() == size_t(monitors.size());
    for (int i = 0; sameOutputs && i < monitors.size(); ++i)
      sameOutputs = views[i]->screen() == monitors[i].screen;
    if (!sameOutputs) {
      views.clear();
      for (int i = 0; i < monitors.size(); ++i)
        if (auto view = createView(i))
          views.push_back(std::move(view));
      if (views.size() != size_t(monitors.size())) {
        views.clear();
        becomeIdle();
        return;
      }
    }
    showOverlay();
  };
  auto listenForRequests = [&] {
    if (requests || listener < 0)
      return;
    requests = std::make_unique<QSocketNotifier>(listener, QSocketNotifier::Read);
    QObject::connect(requests.get(), &QSocketNotifier::activated, &app,
                     [listener, captureAndShow] {
                       if (takeCaptureRequests(listener) > 0)
                         captureAndShow();
                     });
  };
  if (!uiTest) {
    QObject::connect(&controller, &CaptureController::done, &app, [&] {
      becomeIdle();
      if (std::exchange(captureQueued, false))
        QTimer::singleShot(0, &app, captureAndShow);
    });
    // A failed export restores the current capture instead of serving a
    // request queued while it was exporting.
    QObject::connect(&controller, &CaptureController::statusChanged, &app, [&] {
      if (!controller.status().isEmpty())
        captureQueued = false;
    });
  }
  QObject::connect(&controller, &CaptureController::pinRequested, &app,
                   [&](const QImage &image, QRectF rect) {
    if (!pinWindows.setMonitors(controller.monitors())) {
      controller.finishPin(false);
      return;
    }
    if (controller.scrollState() == int(CaptureController::ScrollState::Reviewing)) {
      const QRectF screen = scrollScreenRect();
      const qreal scale = std::min({controller.property("reviewScale").toReal(),
                                    (screen.width() - 60) / image.width(),
                                    (screen.height() * .8) / image.height()});
      rect = QRectF(screen.topLeft() + QPointF(30, 30), QSizeF(image.size()) * scale);
    }
    // Without resident mode a pin keeps this process alive. Let subsequent
    // plain launches reuse it, so older pins can be excluded from captures.
    // If another process owns the socket, it serves those launches and cannot
    // hide this process's pins, so refuse the pin instead of leaking it.
    if (!uiTest && !daemon && !requests) {
      const std::string path = pinUiTest
          ? testSettings->filePath(QStringLiteral("omarchy-screenshot.sock")).toStdString()
          : daemonSocketPath();
      std::string listenError;
      if (listenForCaptureRequests(path, &listener, &socketLock, &listenError) !=
          ListenResult::Listening) {
        if (!listenError.empty())
          qWarning().noquote() << QString::fromStdString(listenError);
        controller.finishPin(false);
        return;
      }
      ownedSocket = path;
      listenForRequests();
    }
    controller.finishPin(pins.add(image, rect) >= 0);
  });
  QObject::connect(&pins, &PinnedImages::countChanged, &app, [&] {
    if (pins.count() > 0)
      idleTimer.stop();
    else if (!capturing()) {
      if (!daemon && !pinUiTest)
        QTimer::singleShot(0, &app, [&] {
          if (!capturing() && pins.count() == 0)
            QCoreApplication::quit();
        });
      else if (daemon && idleTimeout > 0)
        idleTimer.start();
    }
  });
  if (daemon) {
    QObject::connect(&idleTimer, &QTimer::timeout, &app, [&] {
      if (capturing() || pins.count() > 0)
        return;
      // Serve a request that queued as the timer fired. One that arrives
      // after this check is never acknowledged, so its client captures.
      if (takeCaptureRequests(listener) > 0)
        captureAndShow();
      else
        QCoreApplication::quit();
    });
    // Draining every queued request coalesces repeated presses.
    listenForRequests();
    // Socket activation starts the daemon for a request that is already
    // waiting; the capture taken while starting up is the one it asked for.
    pollfd pending{listener, POLLIN, 0};
    if (poll(&pending, 1, 0) > 0 && takeCaptureRequests(listener) > 0)
      showOverlay();
    else
      becomeIdle();
  }

  if (pinUiTest) {
    const auto fail = [&](const QString &message) {
      QTextStream(stderr) << message << ": " << error << '\n';
      return 2;
    };
    const auto monitors = controller.monitors();
    if (monitors.size() < 2)
      return fail(QStringLiteral("Pin self-test needs two monitors"));
    const QRectF firstScreen = monitors[0].geometry;
    const QRectF secondScreen = monitors[1].geometry;
    QRectF desktop;
    for (const auto &monitor : monitors)
      desktop = desktop.united(monitor.geometry);
    controller.pointerPress(0, 100, 100);
    controller.pointerMove(0, 360, 260);
    controller.pointerRelease(0, 360, 260);
    controller.setTool(QStringLiteral("fillrect"));
    controller.pointerPress(0, 120, 120);
    controller.pointerMove(0, 150, 150);
    controller.pointerRelease(0, 150, 150);
    const QImage finished = controller.renderedImage();
    const QRectF original = controller.selection();
    views[0]->rootObject()->forceActiveFocus();
    QKeyEvent pinKey(QEvent::KeyPress, Qt::Key_P, Qt::NoModifier, QStringLiteral("p"));
    QCoreApplication::sendEvent(views[0].get(), &pinKey);
    QTest::qWait(150);
    if (pins.count() != 1 || pins.image(1) != finished || controller.selected() ||
        overlayVisible() || pins.geometry(1) != original) {
      qWarning() << "Pin completion diagnostic:" << pins.count()
                 << (pins.image(1) == finished) << controller.selected()
                 << overlayVisible() << pins.geometry(1) << original;
      return fail(QStringLiteral("Pin shortcut, annotation export or capture completion failed"));
    }
    VirtualPointer input;
    const auto release = qScopeGuard([&] { input.setLeftButtonPressed(false, &error); });
    const QPointF grip(70, 70);
    const QPointF start = original.topLeft() + grip;
    const QPointF target = secondScreen.topLeft() + QPointF(120, 120) + grip;
    if (!input.moveTo(start, desktop, &error))
      return fail(QStringLiteral("Cannot position pointer"));
    QTest::qWait(60);
    if (!input.setLeftButtonPressed(true, &error))
      return fail(QStringLiteral("Cannot press pointer"));
    QTest::qWait(50);
    if (pins.draggingId() != 1)
      return fail(QStringLiteral("Pin did not receive real pointer press"));
    if (!input.moveTo(target, desktop, &error))
      return fail(QStringLiteral("Cannot drag across outputs"));
    QTest::qWait(100);
    // Continue after the entire image left its original output: the grab
    // must remain alive until the physical button is released.
    const QPointF finalTarget = target + QPointF(40, 40);
    if (!input.moveTo(finalTarget, desktop, &error))
      return fail(QStringLiteral("Cannot continue cross-output drag"));
    QTest::qWait(80);
    input.setLeftButtonPressed(false, &error);
    QTest::qWait(60);
    const QRectF moved = pins.geometry(1);
    if (QLineF(moved.topLeft(), secondScreen.topLeft() + QPointF(160, 160)).length() > 3 ||
        moved.size() != original.size() || pins.draggingId() != -1)
      return fail(QStringLiteral("Cross-output drag changed size or lost its grab"));
    QImage marker(80, 60, QImage::Format_RGB32);
    marker.fill(QColor("#ed12b7"));
    const QRectF markerRect(firstScreen.topLeft() + QPointF(450, 300), QSizeF(80, 60));
    const int markerId = pins.add(marker, markerRect);
    QTest::qWait(80);
    if (pins.count() != 2)
      return fail(QStringLiteral("Multiple pins failed"));
    const auto &pinViews = pinWindows.views();
    const QImage preview = selfTestLogicalImage(pinViews[1]->grabWindow(), secondScreen.size());
    // The preview holds captured screen pixels, so keep it out of shared /tmp.
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!preview.isNull() && !runtime.isEmpty())
      preview.copy(QRect(QPoint(150, 150), QSize(280, 180))).save(
          runtime + QStringLiteral("/omarchy-pin-preview.png"));
    auto request = std::async(std::launch::async, [&] {
      return forwardCaptureRequest(ownedSocket);
    });
    QElapsedTimer requestTime;
    requestTime.start();
    while (!overlayVisible() && requestTime.elapsed() < 2000)
      QTest::qWait(20);
    if (!overlayVisible() || !request.get())
      return fail(QStringLiteral("Pinned session did not accept another screenshot request"));
    QTest::qWait(100);
    const auto &capturedMonitor = controller.monitors()[0];
    const QPoint sample(qRound(480 * capturedMonitor.image.width() / firstScreen.width()),
                         qRound(330 * capturedMonitor.image.height() / firstScreen.height()));
    if (capturedMonitor.image.pixelColor(sample) == QColor("#ed12b7"))
      return fail(QStringLiteral("Existing pins leaked into the next capture"));
    controller.cancel();
    QTest::qWait(80);
    if (!input.moveTo(moved.center(), desktop, &error))
      return fail(QStringLiteral("Cannot point at pin"));
    QTest::qWait(40);
    if (!input.clickAt(moved.center(), desktop, &error) ||
        !input.clickAt(moved.center(), desktop, &error))
      return fail(QStringLiteral("Cannot double-click pin"));
    QTest::qWait(80);
    if (pins.count() != 2)
      return fail(QStringLiteral("Double-click unexpectedly dismissed a pin"));
    const QPointF close = PinnedImages::closeRect(moved).center();
    input.moveTo(close, desktop, &error);
    QTest::qWait(40);
    input.clickAt(close, desktop, &error);
    QTest::qWait(80);
    if (pins.count() != 1 || !pins.image(1).isNull() || pins.image(markerId).isNull())
      return fail(QStringLiteral("Close button did not dismiss only its pin"));
    pins.close(markerId);
    QTextStream(stdout) << "Pin shortcut, annotations, real cross-output drag, multiple pins, "
                           "capture exclusion and close button OK\n";
    return 0;
  }

  if (app.arguments().contains(QStringLiteral("--scroll-ui-integration-test"))) {
    const auto waitFor = [&](const std::function<bool()> &ready, int timeout = 10000) {
      QElapsedTimer elapsed;
      elapsed.start();
      while (!ready() && elapsed.elapsed() < timeout)
        QTest::qWait(20);
      return ready();
    };
    const auto fail = [&](const QString &message) {
      QTextStream(stderr) << message << ": " << controller.status() << '\n';
      return 2;
    };
    if (!startScrollFixture(controller))
      return fail(QStringLiteral("Missing scroll fixture"));
    QRectF desktop = controller.monitors().first().geometry;
    for (const auto &monitor : controller.monitors())
      desktop = desktop.united(monitor.geometry);
    VirtualPointer input;
    const auto click = [&](const QPointF &point) {
      if (!input.moveTo(point, desktop, &error))
        return false;
      // Allow compositor pointer focus to follow the warp before clicking.
      QTest::qWait(40);
      if (qEnvironmentVariableIsSet("OMARCHY_SCROLL_DIAGNOSTICS")) {
        QProcess cursor;
        cursor.start(QStringLiteral("hyprctl"),
                     {QStringLiteral("-j"), QStringLiteral("cursorpos")});
        cursor.waitForFinished(1000);
        QTextStream(stderr) << "test click point=" << point.x() << ',' << point.y()
                            << " cursor=" << cursor.readAllStandardOutput().trimmed()
                            << " screen=" << scrollScreenRect().x() << ','
                            << scrollScreenRect().y() << " surface="
                            << scrollBar->geometry().x() << ',' << scrollBar->geometry().y()
                            << ' ' << scrollBar->width() << 'x' << scrollBar->height()
                            << '\n';
      }
      return input.clickAt(point, desktop, &error);
    };
    bool movementDelivered = true;
    const auto movement = QObject::connect(
        &controller, &CaptureController::scrollInputSent, &app, [&] {
      movementDelivered &= input.moveTo(
          controller.scrollRegion().topLeft() + QPointF(30, 30), desktop, &error);
    });
    if (!waitFor([&] { return controller.scrollHeight() > 0; }))
      return fail(QStringLiteral("First frame was not captured"));
    const int firstHeight = controller.scrollHeight();
    if (!waitFor([&] {
          return controller.scrollHeight() > firstHeight + 70 ||
                 controller.scrollState() != int(CaptureController::ScrollState::Capturing);
        }) || !movementDelivered ||
        controller.scrollHeight() <= firstHeight + 70 ||
        controller.scrollState() != int(CaptureController::ScrollState::Capturing))
      return fail(QStringLiteral("Mouse movement interrupted scrolling"));
    QObject::disconnect(movement);
    const auto *hint = scrollBar->rootObject()->findChild<QObject *>(
        QStringLiteral("scrollStopHint"));
    if (!hint || hint->property("text").toString() !=
                     QCoreApplication::translate("ScrollCapture",
                                                 "Click to stop capturing"))
      return fail(QStringLiteral("Missing click-to-stop hint"));
    for (auto *item : scrollBar->rootObject()->findChildren<QQuickItem *>())
      if (item->inherits("QQuickImage"))
        return fail(QStringLiteral("Unexpected live screenshot preview"));
    const QPointF stopLocal = controller.scrollRegion().center() - scrollScreenRect().topLeft();
    const QImage liveUi = scrollBar->grabWindow();
    if (liveUi.isNull() || liveUi.pixelColor(stopLocal.toPoint()).alpha() != 0)
      return fail(QStringLiteral("Capture area is not transparent"));
    const auto clickToStop = [&] {
      // The click region must remain active even while visual UI is hidden for capture.
      return waitFor([&] {
               return scrollBar->rootObject()->property("frameHidden").toBool() &&
                      scrollBar->mask().contains(stopLocal.toPoint());
             }) &&
             click(controller.scrollRegion().center()) &&
             waitFor([&] { return controller.scrollStopping(); }, 500) &&
             waitFor([&] {
               return controller.scrollState() == int(CaptureController::ScrollState::Reviewing);
             });
    };
    if (!clickToStop())
      return fail(QStringLiteral("Click in capture area did not stop scrolling"));
    QTest::qWait(150);
    const auto *select = findQuickItem(longView->rootObject(),
        QStringLiteral("longTool_select"));
    const auto *selectHighlight = findQuickItem(longView->rootObject(),
        QStringLiteral("longHighlight_select"));
    const auto *label = findQuickItem(longView->rootObject(),
        QStringLiteral("longLabel_select"));
    const auto luminance = [](QColor color) {
      const auto linear = [](double channel) {
        return channel <= .04045 ? channel / 12.92
                               : std::pow((channel + .055) / 1.055, 2.4);
      };
      return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) +
             .0722 * linear(color.blueF());
    };
    if (!select || !selectHighlight || !label)
      return fail(QStringLiteral("Missing long-image toolbar"));
    for (bool dark : {false, true}) {
      controller.setDarkToolbar(dark);
      QTest::qWait(100);
      const double foreground = luminance(label->property("color").value<QColor>());
      const double background = luminance(selectHighlight->property("color").value<QColor>());
      const double contrast = (std::max(foreground, background) + .05) /
                              (std::min(foreground, background) + .05);
      if (!select->property("selected").toBool() || contrast < 4.5)
        return fail(QStringLiteral("Selected toolbar text has low contrast"));
    }
    const auto grabItem = [&](QQuickItem *item) {
      if (!item)
        return QImage();
      const auto grab = item->grabToImage();
      if (!grab || !waitFor([&] { return !grab->image().isNull(); }))
        return QImage();
      return grab->image();
    };
    for (const QString &action : {QStringLiteral("zoom_in"),
                                  QStringLiteral("zoom_out"),
                                  QStringLiteral("resume")}) {
      const QString object = QStringLiteral("longGlyph_") + action;
      const QImage glyph = grabItem(findQuickItem(longView->rootObject(), object));
      int drawn = 0;
      for (int y = 0; y < glyph.height(); ++y)
        for (int x = 0; x < glyph.width(); ++x)
          drawn += glyph.pixelColor(x, y).alpha() > 0;
      if (glyph.isNull() || drawn < 15 || glyph.pixelColor(0, 0).alpha() != 0)
        return fail(QStringLiteral("Missing or opaque toolbar icon: ") + action);
    }
    auto *toolbarItem = findQuickItem(longView->rootObject(),
        QStringLiteral("longToolbar"));
    grabItem(toolbarItem).save(QStringLiteral("/tmp/omarchy-scroll-tools-dark.png"));
    controller.setDarkToolbar(false);
    QTest::qWait(100);
    grabItem(toolbarItem).save(QStringLiteral("/tmp/omarchy-scroll-tools-light.png"));
    const int reviewedHeight = controller.scrollHeight();
    controller.setTool(QStringLiteral("marker"));
    controller.pointerPress(-1, 45, 45);
    controller.pointerRelease(-1, 45, 45);
    auto *resume = findQuickItem(longView->rootObject(),
        QStringLiteral("longTool_resume"));
    if (!resume)
      return fail(QStringLiteral("Missing resume button"));
    const QPointF resumePoint = resume->mapToItem(
        longView->rootObject(), QPointF(resume->width() / 2, resume->height() / 2)) +
        scrollScreenRect().topLeft();
    if (!click(resumePoint) ||
        !waitFor([&] { return controller.scrollHeight() > reviewedHeight + 30; }) ||
        !clickToStop() || controller.annotations().size() != 1)
      return fail(QStringLiteral("Resume, stop or annotation preservation failed"));
    QTextStream(stdout) << "Mouse movement continues; real click stops; transparent capture UI; "
                           "toolbar contrast and icons; resume keeps annotations OK\n";
    return 0;
  }
  if (app.arguments().contains(QStringLiteral("--scroll-ui-self-test"))) {
    scrollBar = makeScrollView(QStringLiteral("ScrollCapture.qml"));
    longView = makeScrollView(QStringLiteral("LongOverlay.qml"));
    if (scrollBar->status() != QQuickView::Ready ||
        longView->status() != QQuickView::Ready)
      return 1;
    longView->show();
    QTest::qWait(150);
    QTextStream(stdout) << "Scroll capture and long preview QML ready\n";
    return 0;
  }

  auto sendMouse = [&](QEvent::Type type, const QPointF &local,
                       Qt::MouseButton button, Qt::MouseButtons buttons) {
    const QPointF global = controller.monitors()[0].geometry.topLeft() + local;
    QMouseEvent event(type, local, global, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(views[0].get(), &event);
  };
  auto toolbarCenter = [&](int index) {
    QVariant position;
    QMetaObject::invokeMethod(views[0]->rootObject(), "toolbarButtonCenter",
                              Q_RETURN_ARG(QVariant, position),
                              Q_ARG(QVariant, index));
    const auto point = position.toMap();
    return QPointF(point.value(QStringLiteral("x")).toDouble(),
                   point.value(QStringLiteral("y")).toDouble());
  };
  auto visibleHandleCount = [&] {
    QVariant count;
    QMetaObject::invokeMethod(views[0]->rootObject(),
                              "visibleResizeHandleCount",
                              Q_RETURN_ARG(QVariant, count));
    return count.toInt();
  };
  QImage baseline;
  bool toolbarWorked = false;
  bool colorPickerWorked = false;
  bool colorPersistenceWorked = false;
  bool toolbarThemeWorked = false;
  bool toolbarPaddingWorked = false;
  bool toolbarOrderWorked = false;
  bool groupedToolbarWorked = false;
  bool redoShortcutWorked = false;
  bool escapeCloses = false;
  QString selectedTestColor;
  bool annotationColorsKept = false;
  bool coloredTextExported = false;
  bool coloredPenExported = false;
  bool cursorIconRendered = false;
  bool handlesInitiallyVisible = false;
  bool handlesHiddenForMosaic = false;
  bool handleResizeWorked = false;
  bool arrowResizeWorked = false;
  bool repeatingArrowResizeWorked = false;
  bool leftHandHotkeysWorked = false;
  bool variantFirstDragWorked = false;
  bool hoverDescriptionWorked = false;
  bool dragPreviewWorked = false;
  bool mosaicExportChanged = false;
  bool rectangleShapeWorked = false;
  bool mosaicPixelsVisible = false;
  bool mosaicDraftWindowMatchesImage = false;
  bool mosaicWindowMatchesImage = false;
  bool mosaicMaskTransparent = false;
  bool dragBorderPixelsVisible = false;
  bool dragBorderGone = false;
  bool penDraftWorked = false;
  bool penExportChanged = false;
  bool penPreviewVisible = false;
  bool editorFocused = false;
  bool multilineEditing = false;
  bool editorBorderDashed = false;
  bool editorBackgroundTransparent = false;
  bool exportedTextRows = false;
  bool doubleClickCopied = false;
  if (uiTest) {
    const auto windowShowsMosaic = [&] {
      // grabToImage() renders into an offscreen target with its own depth
      // buffer, so it cannot detect incorrect stacking in the actual window.
      const auto &monitor = controller.monitors()[0];
      const QImage window = selfTestLogicalImage(
          views[0]->grabWindow(), QSizeF(views[0]->width(), views[0]->height()));
      if (window.width() <= 650 || window.height() <= 450 ||
          monitor.mosaicImage.isNull())
        return false;
      int matches = 0;
      int samples = 0;
      // Sample block centers inside the mosaic, away from its drag border.
      for (int y = 306; y < 450; y += 12)
        for (int x = 306; x < 650; x += 12) {
          const QColor expected = monitor.mosaicImage.pixelColor(
              x * monitor.mosaicImage.width() / monitor.geometry.width(),
              y * monitor.mosaicImage.height() / monitor.geometry.height());
          const QColor actual = window.pixelColor(x, y);
          ++samples;
          if (qAbs(expected.red() - actual.red()) <= 1 &&
              qAbs(expected.green() - actual.green()) <= 1 &&
              qAbs(expected.blue() - actual.blue()) <= 1)
            ++matches;
        }
      if (matches != samples)
        QTextStream(stdout) << "Mosaic window diagnostic: " << matches << "/"
                            << samples << " samples match\n";
      return matches == samples;
    };
    controller.pointerPress(0, 100, 100);
    controller.pointerMove(0, 900, 600);
    controller.pointerRelease(0, 900, 600);
    baseline = controller.renderedImage();
    QTimer::singleShot(100, &app, [&, windowShowsMosaic] {
      views[0]->requestActivate();
      auto displayedAction = [&](int index) {
        QVariant action;
        QMetaObject::invokeMethod(views[0]->rootObject(),
                                  "toolbarDisplayedTool",
                                  Q_RETURN_ARG(QVariant, action),
                                  Q_ARG(QVariant, index));
        return action.toString();
      };
      QVariant closeButton;
      QMetaObject::invokeMethod(views[0]->rootObject(), "toolbarHasAction",
                                Q_RETURN_ARG(QVariant, closeButton),
                                Q_ARG(QVariant, QStringLiteral("cancel")));
      toolbarOrderWorked =
          displayedAction(7) == QStringLiteral("marker") &&
          displayedAction(8) == QStringLiteral("scroll") &&
          displayedAction(9) == QStringLiteral("undo") &&
          displayedAction(10) == QStringLiteral("redo") &&
          !closeButton.toBool();
      handlesInitiallyVisible = visibleHandleCount() == 8;
      std::function<QQuickItem *(QQuickItem *, const QString &)>
          findVisualItem =
              [&](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name)
          return item;
        for (QQuickItem *child : item->childItems())
          if (auto *found = findVisualItem(child, name))
            return found;
        return nullptr;
      };
      auto *cursorIcon = findVisualItem(views[0]->rootObject(),
                                        QStringLiteral("selectCursorGlyph"));
      if (!cursorIcon)
        QTextStream(stdout) << "Cursor diagnostic: visual item missing\n";
      if (cursorIcon) {
        const QSizeF logicalSize(cursorIcon->width(), cursorIcon->height());
        auto grab = cursorIcon->grabToImage();
        if (!grab)
          QTextStream(stdout) << "Cursor diagnostic: grab unavailable\n";
        if (grab)
          QObject::connect(
              grab.get(), &QQuickItemGrabResult::ready, &app, [&, grab, logicalSize] {
                const QImage image = selfTestLogicalImage(grab->image(), logicalSize);
                cursorIconRendered = image.width() >= 16 &&
                                     image.height() >= 20 &&
                                     image.pixelColor(2, 3).alpha() > 0 &&
                                     image.pixelColor(15, 19).alpha() == 0;
                if (!cursorIconRendered)
                  QTextStream(stdout)
                      << "Cursor diagnostic: " << image.width() << "x"
                      << image.height()
                      << " alpha=" << image.pixelColor(2, 3).alpha() << ","
                      << image.pixelColor(15, 19).alpha() << "\n";
              });
      }
      auto *colorButton = views[0]->rootObject()->findChild<QQuickItem *>(
          QStringLiteral("colorButton"));
      auto *colorPanel = views[0]->rootObject()->findChild<QQuickItem *>(
          QStringLiteral("colorPanel"));
      auto *toolbarItem = views[0]->rootObject()->findChild<QQuickItem *>(
          QStringLiteral("toolbar"));
      auto *saturationValueField =
          views[0]->rootObject()->findChild<QQuickItem *>(
              QStringLiteral("saturationValueField"));
      auto *hueField = views[0]->rootObject()->findChild<QQuickItem *>(
          QStringLiteral("hueField"));
      auto *highlight = findVisualItem(views[0]->rootObject(),
                                       QStringLiteral("selectHighlight"));
      auto *content = findVisualItem(views[0]->rootObject(),
                                     QStringLiteral("selectContent"));
      toolbarPaddingWorked = highlight && content &&
                             highlight->width() - content->width() >= 8 &&
                             highlight->height() - content->height() >= 8;
      if (colorButton && colorPanel && saturationValueField && hueField) {
        const QPointF buttonCenter = colorButton->mapToItem(
            views[0]->rootObject(),
            QPointF(colorButton->width() / 2, colorButton->height() / 2));
        sendMouse(QEvent::MouseButtonPress, buttonCenter, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, buttonCenter, Qt::LeftButton,
                  Qt::NoButton);
        const bool opened = colorPanel->isVisible();
        auto themeCenter = [&](int index) {
          QVariant position;
          QMetaObject::invokeMethod(views[0]->rootObject(), "themeButtonCenter",
                                    Q_RETURN_ARG(QVariant, position),
                                    Q_ARG(QVariant, index));
          const auto point = position.toMap();
          return QPointF(point.value(QStringLiteral("x"), -1).toDouble(),
                         point.value(QStringLiteral("y"), -1).toDouble());
        };
        const QPointF lightCenter = themeCenter(0);
        const QPointF darkCenter = themeCenter(1);
        if (opened && toolbarItem && darkCenter.x() >= 0 &&
            lightCenter.x() >= 0) {
          const QColor lightSurface = toolbarItem->property("color").value<QColor>();
          sendMouse(QEvent::MouseButtonPress, darkCenter, Qt::LeftButton,
                    Qt::LeftButton);
          sendMouse(QEvent::MouseButtonRelease, darkCenter, Qt::LeftButton,
                    Qt::NoButton);
          const bool darkApplied = controller.darkToolbar() &&
                                   toolbarItem->property("color").value<QColor>() !=
                                       lightSurface;
          CaptureController restoredTheme;
          const bool darkPersisted = restoredTheme.darkToolbar();
          sendMouse(QEvent::MouseButtonPress, lightCenter, Qt::LeftButton,
                    Qt::LeftButton);
          sendMouse(QEvent::MouseButtonRelease, lightCenter, Qt::LeftButton,
                    Qt::NoButton);
          toolbarThemeWorked = darkApplied && darkPersisted &&
                               !controller.darkToolbar() &&
                               toolbarItem->property("color").value<QColor>() ==
                                   lightSurface &&
                               colorPanel->isVisible();
        }
        const bool colorButtonUnselected =
            colorButton->property("color").value<QColor>().alpha() == 0;
        const QPointF huePoint = hueField->mapToItem(
            views[0]->rootObject(),
            QPointF(hueField->width() / 2, hueField->height() * .42));
        sendMouse(QEvent::MouseButtonPress, huePoint, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, huePoint, Qt::LeftButton,
                  Qt::NoButton);
        const QPointF palettePoint = saturationValueField->mapToItem(
            views[0]->rootObject(),
            QPointF(saturationValueField->width() * .72,
                    saturationValueField->height() * .22));
        sendMouse(QEvent::MouseButtonPress, palettePoint, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, palettePoint, Qt::LeftButton,
                  Qt::NoButton);
        selectedTestColor = controller.annotationColor();
        colorPickerWorked = opened && colorButtonUnselected &&
                            colorPanel->isVisible() &&
                            selectedTestColor != QStringLiteral("#ff4b55") &&
                            !views[0]->rootObject()->findChild<QQuickItem *>(
                                QStringLiteral("colorHexInput")) &&
                            !views[0]->rootObject()->findChild<QQuickItem *>(
                                QStringLiteral("colorApplyButton"));
        QKeyEvent colorKey(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier,
                           QStringLiteral("q"));
        QCoreApplication::sendEvent(views[0].get(), &colorKey);
        const bool shortcutClosed = !colorPanel->isVisible();
        controller.saveAnnotationColor();
        CaptureController restored;
        colorPersistenceWorked =
            restored.annotationColor() == selectedTestColor;
        QCoreApplication::sendEvent(views[0].get(), &colorKey);
        colorPickerWorked &=
            shortcutClosed && colorPanel->isVisible() && controller.selected();
        QKeyEvent closePanel(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier,
                             QStringLiteral("q"));
        QCoreApplication::sendEvent(views[0].get(), &closePanel);
        colorPickerWorked &= !colorPanel->isVisible();
        sendMouse(QEvent::MouseButtonPress, buttonCenter, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, buttonCenter, Qt::LeftButton,
                  Qt::NoButton);
        QVariant swatchPosition;
        QMetaObject::invokeMethod(views[0]->rootObject(), "colorPresetCenter",
                                  Q_RETURN_ARG(QVariant, swatchPosition),
                                  Q_ARG(QVariant, 1));
        const auto swatchPoint = swatchPosition.toMap();
        const QPointF swatchCenter(
            swatchPoint.value(QStringLiteral("x"), -1).toDouble(),
            swatchPoint.value(QStringLiteral("y"), -1).toDouble());
        if (swatchCenter.x() >= 0 && colorPanel->isVisible()) {
          sendMouse(QEvent::MouseButtonPress, swatchCenter, Qt::LeftButton,
                    Qt::LeftButton);
          sendMouse(QEvent::MouseButtonRelease, swatchCenter, Qt::LeftButton,
                    Qt::NoButton);
        }
        colorPickerWorked &=
            swatchCenter.x() >= 0 && !colorPanel->isVisible() &&
            controller.annotationColor() == QStringLiteral("#ff9d42");
        controller.setAnnotationColor(selectedTestColor);
      }
      const QPointF rectGroupButton = toolbarCenter(1);
      sendMouse(QEvent::MouseButtonPress, rectGroupButton, Qt::LeftButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseButtonRelease, rectGroupButton, Qt::LeftButton,
                Qt::NoButton);
      QCoreApplication::processEvents();
      auto *variantPanel = views[0]->rootObject()->findChild<QQuickItem *>(
          QStringLiteral("variantPanel"));
      QVariant optionPosition;
      QMetaObject::invokeMethod(views[0]->rootObject(), "variantOptionCenter",
                                Q_RETURN_ARG(QVariant, optionPosition),
                                Q_ARG(QVariant, 1));
      const auto optionPoint = optionPosition.toMap();
      const QPointF roundRectOption(
          optionPoint.value(QStringLiteral("x"), -1).toDouble(),
          optionPoint.value(QStringLiteral("y"), -1).toDouble());
      const bool groupOpened = variantPanel && variantPanel->isVisible() &&
                               roundRectOption.x() >= 0;
      if (groupOpened) {
        sendMouse(QEvent::MouseButtonPress, roundRectOption, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, roundRectOption, Qt::LeftButton,
                  Qt::NoButton);
      }
      QVariant displayedTool;
      QMetaObject::invokeMethod(views[0]->rootObject(), "toolbarDisplayedTool",
                                Q_RETURN_ARG(QVariant, displayedTool),
                                Q_ARG(QVariant, 1));
      groupedToolbarWorked =
          groupOpened && !variantPanel->isVisible() &&
          controller.tool() == QStringLiteral("roundrect") &&
          controller.toolVariants().value(QStringLiteral("rect")) ==
              QStringLiteral("roundrect") &&
          displayedTool.toString() == QStringLiteral("roundrect");
      controller.setTool(QStringLiteral("line"));
      QKeyEvent groupShortcut(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier,
                              QStringLiteral("r"));
      QCoreApplication::sendEvent(views[0].get(), &groupShortcut);
      groupedToolbarWorked &=
          controller.tool() == QStringLiteral("roundrect");
      QVariant standaloneLine;
      QVariant standaloneSpotlight;
      QMetaObject::invokeMethod(views[0]->rootObject(), "toolbarHasAction",
                                Q_RETURN_ARG(QVariant, standaloneLine),
                                Q_ARG(QVariant, QStringLiteral("line")));
      QMetaObject::invokeMethod(views[0]->rootObject(), "toolbarHasAction",
                                Q_RETURN_ARG(QVariant, standaloneSpotlight),
                                Q_ARG(QVariant, QStringLiteral("spotlight")));
      groupedToolbarWorked &=
          !standaloneLine.toBool() && !standaloneSpotlight.toBool();
      for (const auto &variant :
           {std::tuple(3, 3, QStringLiteral("arrow"), QStringLiteral("line")),
            std::tuple(2, 2, QStringLiteral("ellipse"),
                       QStringLiteral("spotlight"))}) {
        const int buttonIndex = std::get<0>(variant);
        const QPointF groupButton = toolbarCenter(buttonIndex);
        sendMouse(QEvent::MouseButtonPress, groupButton, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, groupButton, Qt::LeftButton,
                  Qt::NoButton);
        QCoreApplication::processEvents();
        QVariant variantPosition;
        QMetaObject::invokeMethod(views[0]->rootObject(), "variantOptionCenter",
                                  Q_RETURN_ARG(QVariant, variantPosition),
                                  Q_ARG(QVariant, std::get<1>(variant)));
        const auto point = variantPosition.toMap();
        const QPointF option(point.value(QStringLiteral("x"), -1).toDouble(),
                             point.value(QStringLiteral("y"), -1).toDouble());
        const bool opened = variantPanel->isVisible() && option.x() >= 0;
        if (opened) {
          sendMouse(QEvent::MouseButtonPress, option, Qt::LeftButton,
                    Qt::LeftButton);
          sendMouse(QEvent::MouseButtonRelease, option, Qt::LeftButton,
                    Qt::NoButton);
        }
        QVariant shown;
        QMetaObject::invokeMethod(views[0]->rootObject(),
                                  "toolbarDisplayedTool",
                                  Q_RETURN_ARG(QVariant, shown),
                                  Q_ARG(QVariant, buttonIndex));
        groupedToolbarWorked &=
            opened && !variantPanel->isVisible() &&
            controller.tool() == std::get<3>(variant) &&
            controller.toolVariants().value(std::get<2>(variant)) ==
                std::get<3>(variant) &&
            shown.toString() == std::get<3>(variant);
      }
      const QPointF mosaicButton = toolbarCenter(6);
      sendMouse(QEvent::MouseButtonPress, mosaicButton, Qt::LeftButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseButtonRelease, mosaicButton, Qt::LeftButton,
                Qt::NoButton);
      toolbarWorked = controller.tool() == QStringLiteral("mosaic");
      handlesHiddenForMosaic = visibleHandleCount() == 0;
      sendMouse(QEvent::MouseButtonPress, QPointF(250, 250), Qt::LeftButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseMove, QPointF(700, 500), Qt::NoButton,
                Qt::LeftButton);
      const auto draft = controller.draft();
      const auto start = draft.value(QStringLiteral("start")).toMap();
      const auto end = draft.value(QStringLiteral("end")).toMap();
      dragPreviewWorked =
          draft.value(QStringLiteral("type")).toString() ==
              QStringLiteral("mosaic") &&
          draft.value(QStringLiteral("points")).toList().isEmpty() &&
          end.value(QStringLiteral("x")).toDouble() >
              start.value(QStringLiteral("x")).toDouble() &&
          end.value(QStringLiteral("y")).toDouble() >
              start.value(QStringLiteral("y")).toDouble() &&
          views[0]->rootObject()->property("mosaicDraftBorderVisible").toBool();
      auto *border = views[0]->rootObject()->findChild<QQuickItem *>(
          QStringLiteral("mosaicDraftBorder"));
      if (border) {
        auto grab = border->grabToImage();
        if (grab)
          QObject::connect(
              grab.get(), &QQuickItemGrabResult::ready, &app, [&, grab] {
                const QImage image = grab->image();
                dragBorderPixelsVisible =
                    image.width() > 10 && image.height() > 10 &&
                    image.pixelColor(1, 1).alpha() > 0 &&
                    image.pixelColor(1, 1).name() == selectedTestColor &&
                    image.pixelColor(image.width() / 2, image.height() / 2)
                            .alpha() == 0;
              });
      }
      QTimer::singleShot(50, &app, [&, windowShowsMosaic] {
        mosaicDraftWindowMatchesImage = windowShowsMosaic();
      });
      QTimer::singleShot(100, &app, [&, windowShowsMosaic] {
        sendMouse(QEvent::MouseButtonRelease, QPointF(700, 500), Qt::LeftButton,
                  Qt::NoButton);
        dragBorderGone = !views[0]
                              ->rootObject()
                              ->property("mosaicDraftBorderVisible")
                              .toBool();
        const QImage withMosaic = controller.renderedImage();
        if (withMosaic.size() == baseline.size()) {
          for (int y = 0; y < baseline.height() && !mosaicExportChanged; ++y)
            for (int x = 0; x < baseline.width(); ++x)
              if (baseline.pixel(x, y) != withMosaic.pixel(x, y)) {
                mosaicExportChanged = true;
                break;
              }
        }
        if (!controller.annotations().isEmpty()) {
          const auto &monitor = controller.monitors()[0];
          const auto item = controller.annotations().first().toMap();
          const auto shape = CaptureController::mosaicPath(item);
          rectangleShapeWorked =
              item.value(QStringLiteral("points")).toList().isEmpty() &&
              shape.contains(monitor.geometry.topLeft() + QPointF(475, 375)) &&
              !shape.contains(monitor.geometry.topLeft() + QPointF(710, 260));
        }
        QTimer::singleShot(50, &app, [&, windowShowsMosaic] {
          mosaicWindowMatchesImage = windowShowsMosaic();
          auto *overlay = views[0]->rootObject()->findChild<QQuickItem *>(
              QStringLiteral("mosaicOverlay"));
          if (!overlay)
            return;
          const QSizeF logicalSize(overlay->width(), overlay->height());
          auto grab = overlay->grabToImage();
          if (!grab)
            return;
          QObject::connect(
              grab.get(), &QQuickItemGrabResult::ready, &app, [&, grab, logicalSize] {
                const QImage image = selfTestLogicalImage(grab->image(), logicalSize);
                if (image.width() > 710 && image.height() > 500) {
                  mosaicPixelsVisible = image.pixelColor(475, 375).alpha() > 0;
                  mosaicMaskTransparent =
                      image.pixelColor(710, 260).alpha() == 0;
                }
              });
        });
      });
      QTimer::singleShot(300, &app, [&] {
        controller.setTool(QStringLiteral("text"));
        const QImage beforeText = controller.renderedImage();
        views[0]->requestActivate();
        const QPointF local(750, 550);
        const QPointF global =
            controller.monitors()[0].geometry.topLeft() + local;
        QMouseEvent press(QEvent::MouseButtonPress, local, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(views[0].get(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(views[0].get(), &release);
        editorFocused =
            views[0]->rootObject()->property("editorFocused").toBool();
        if (!editorFocused)
          QTextStream(stdout)
              << "Text focus diagnostic: active=" << views[0]->isActive()
              << " focusWindow="
              << (QGuiApplication::focusWindow() == views[0].get()) << "\n";
        QKeyEvent letters(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier,
                          QStringLiteral("HI"));
        QCoreApplication::sendEvent(views[0].get(), &letters);
        QKeyEvent newline(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier,
                          QStringLiteral("\r"));
        QCoreApplication::sendEvent(views[0].get(), &newline);
        QKeyEvent secondLine(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier,
                             QStringLiteral("BY"));
        QCoreApplication::sendEvent(views[0].get(), &secondLine);
        multilineEditing =
            views[0]->rootObject()->property("editorText").toString() ==
                QStringLiteral("HI\nBY") &&
            views[0]->rootObject()->property("editorRows").toInt() == 2;
        QTimer::singleShot(35, &app, [&] {
          auto *editor = views[0]->rootObject()->findChild<QQuickItem *>(
              QStringLiteral("textEditor"));
          if (!editor)
            return;
          auto grab = editor->grabToImage();
          if (!grab)
            return;
          QObject::connect(
              grab.get(), &QQuickItemGrabResult::ready, &app, [&, grab] {
                const QImage image = grab->image();
                if (image.width() < 40 || image.height() < 40)
                  return;
                int painted = 0, clear = 0;
                for (int x = 2; x < image.width() - 2; ++x)
                  image.pixelColor(x, 0).alpha() > 16 ? ++painted : ++clear;
                editorBorderDashed = painted > 5 && clear > 5;
                editorBackgroundTransparent =
                    image.pixelColor(image.width() - 10, image.height() / 2)
                        .alpha() == 0;
              });
        });
        QTimer::singleShot(120, &app, [&, beforeText] {
          QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                          QStringLiteral("\r"));
          QCoreApplication::sendEvent(views[0].get(), &enter);
          if (controller.annotations().size() < 2)
            return;
          const auto annotation = controller.annotations()[1].toMap();
          const auto start = annotation.value(QStringLiteral("start")).toMap();
          const QImage afterText = controller.renderedImage();
          if (afterText.size() != beforeText.size())
            return;
          const qreal scale =
              afterText.width() / controller.selection().width();
          const int textX =
              qRound((start.value(QStringLiteral("x")).toDouble() -
                      controller.selection().x()) *
                     scale);
          const qreal firstY = (start.value(QStringLiteral("y")).toDouble() -
                                controller.selection().y()) *
                               scale;
          const qreal lineHeight =
              views[0]->rootObject()->property("textLineHeight").toDouble() *
              scale;
          bool rowsChanged[2] = {false, false};
          bool customColorPainted = false;
          for (int row = 0; row < 2; ++row)
            for (int y = qMax(
                     0, qFloor(firstY + row * lineHeight - lineHeight / 2));
                 y < qMin(afterText.height(),
                          qCeil(firstY + row * lineHeight + lineHeight / 2));
                 ++y)
              for (int x = qMax(0, textX);
                   x < qMin(afterText.width(), textX + 80); ++x)
                if (afterText.pixel(x, y) != beforeText.pixel(x, y)) {
                  rowsChanged[row] = true;
                  customColorPainted |=
                      afterText.pixelColor(x, y).name() == selectedTestColor;
                }
          exportedTextRows = rowsChanged[0] && rowsChanged[1];
          coloredTextExported = customColorPainted;
        });
      });
      QTimer::singleShot(530, &app, [&] {
        const QImage beforePen = controller.renderedImage();
        controller.setAnnotationColor(QStringLiteral("#4d94ff"));
        const bool oldImageUnchanged = beforePen == controller.renderedImage();
        controller.setTool(QStringLiteral("pen"));
        controller.pointerPress(0, 150, 200);
        controller.pointerMove(0, 200, 208);
        controller.pointerMove(0, 250, 192);
        controller.pointerMove(0, 300, 208);
        controller.pointerMove(0, 350, 200);
        penDraftWorked = controller.draft()
                             .value(QStringLiteral("points"))
                             .toList()
                             .size() >= 5;
        controller.pointerRelease(0, 350, 200);
        const QImage afterPen = controller.renderedImage();
        penExportChanged = beforePen != afterPen;
        if (beforePen.size() == afterPen.size())
          for (int y = 0; y < afterPen.height() && !coloredPenExported; ++y)
            for (int x = 0; x < afterPen.width(); ++x)
              if (beforePen.pixel(x, y) != afterPen.pixel(x, y) &&
                  afterPen.pixelColor(x, y).name() ==
                      QStringLiteral("#4d94ff")) {
                coloredPenExported = true;
                break;
              }
        annotationColorsKept =
            oldImageUnchanged && controller.annotations().size() == 3 &&
            controller.annotations()[1].toMap().value(
                QStringLiteral("color")) == selectedTestColor &&
            controller.annotations()[2].toMap().value(
                QStringLiteral("color")) == QStringLiteral("#4d94ff");
        QTimer::singleShot(50, &app, [&] {
          auto *canvas = views[0]->rootObject()->findChild<QQuickItem *>(
              QStringLiteral("marksCanvas"));
          if (!canvas)
            return;
          const QSizeF logicalSize(canvas->width(), canvas->height());
          const QPoint strokePoint =
              canvas->mapFromItem(views[0]->rootObject(), QPointF(200, 202))
                  .toPoint();
          const QPoint clearPoint =
              canvas->mapFromItem(views[0]->rootObject(), QPointF(200, 208))
                  .toPoint();
          auto grab = canvas->grabToImage();
          if (!grab)
            return;
          QObject::connect(
              grab.get(), &QQuickItemGrabResult::ready, &app,
              [&, grab, logicalSize, strokePoint, clearPoint] {
                const QImage image = selfTestLogicalImage(grab->image(), logicalSize);
                if (!image.rect().contains(strokePoint) ||
                    !image.rect().contains(clearPoint))
                  return;
                const QColor stroke = image.pixelColor(strokePoint);
                penPreviewVisible =
                    stroke.alpha() > 0 && stroke.blue() > stroke.red() &&
                    image.pixelColor(clearPoint).alpha() == 0;
              });
        });
      });
      QTimer::singleShot(800, &app, [&] {
        views[0]->requestActivate();
        views[0]->rootObject()->forceActiveFocus();
        QKeyEvent selectKey(QEvent::KeyPress, Qt::Key_V, Qt::NoModifier,
                            QStringLiteral("v"));
        QCoreApplication::sendEvent(views[0].get(), &selectKey);
        const bool selectKeyWorked =
            controller.tool() == QStringLiteral("select") &&
            visibleHandleCount() == 8;
        QKeyEvent mosaicKey(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier,
                            QStringLiteral("g"));
        QCoreApplication::sendEvent(views[0].get(), &mosaicKey);
        const bool mosaicKeyWorked =
            controller.tool() == QStringLiteral("mosaic") &&
            visibleHandleCount() == 0;
        QKeyEvent markerKey(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier,
                            QStringLiteral("b"));
        QCoreApplication::sendEvent(views[0].get(), &markerKey);
        const bool markerKeyWorked =
            controller.tool() == QStringLiteral("marker") &&
            visibleHandleCount() == 0;
        QKeyEvent penKey(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier,
                         QStringLiteral("d"));
        QCoreApplication::sendEvent(views[0].get(), &penKey);
        leftHandHotkeysWorked = selectKeyWorked && mosaicKeyWorked &&
                                markerKeyWorked &&
                                controller.tool() == QStringLiteral("pen") &&
                                visibleHandleCount() == 0;
        const int annotationCount = controller.annotations().size();
        QKeyEvent undoKey(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier,
                           QStringLiteral("z"));
        QCoreApplication::sendEvent(views[0].get(), &undoKey);
        const bool undone = controller.annotations().size() + 1 == annotationCount;
        QKeyEvent redoKey(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier,
                           QStringLiteral("x"));
        QCoreApplication::sendEvent(views[0].get(), &redoKey);
        redoShortcutWorked = undone &&
                             controller.annotations().size() == annotationCount;
        sendMouse(QEvent::MouseMove, toolbarCenter(6), Qt::NoButton,
                  Qt::NoButton);
        const QString mosaicTooltip =
            views[0]->rootObject()->property("toolbarTooltipText").toString();
        sendMouse(QEvent::MouseMove, toolbarCenter(5), Qt::NoButton,
                  Qt::NoButton);
        const QString textTooltip =
            views[0]->rootObject()->property("toolbarTooltipText").toString();
        hoverDescriptionWorked =
            mosaicTooltip == QCoreApplication::translate(
                "AnnotationToolbar", "Mosaic · Drag to redact") &&
            textTooltip == QCoreApplication::translate(
                "AnnotationToolbar", "Text · Click to type");
        if (!hoverDescriptionWorked)
          QTextStream(stdout) << "Tooltip diagnostic: text=" << textTooltip
                              << " mosaic=" << mosaicTooltip << "\n";
      });
    });
    QTimer::singleShot(1600, &app, [&] {
      const auto *root = views[0]->rootObject();
      QVariant preview;
      QMetaObject::invokeMethod(const_cast<QQuickItem *>(root), "previewState",
                                Q_RETURN_ARG(QVariant, preview));
      const auto state = preview.toMap();
      const bool mosaicVisible =
          state.value(QStringLiteral("mosaicVisible")).toBool() &&
          state.value(QStringLiteral("mosaicWidth")).toDouble() > 0 &&
          state.value(QStringLiteral("mosaicImageReady")).toBool();
      const bool textVisible =
          state.value(QStringLiteral("textVisible")).toBool() &&
          state.value(QStringLiteral("textValue")).toString() ==
              QStringLiteral("HI\nBY");
      const qreal textLineHeight =
          state.value(QStringLiteral("textLineHeight")).toDouble();
      const bool previewRowsCentered =
          state.value(QStringLiteral("textRows")).toInt() == 2 &&
          qAbs(state.value(QStringLiteral("firstLineCenter")).toDouble() -
               textLineHeight / 2) < 0.1 &&
          qAbs(state.value(QStringLiteral("secondLineCenter")).toDouble() -
               textLineHeight * 1.5) < 0.1;
      QTextStream(stdout)
          << "qmlCount=" << root->property("annotationCount").toInt()
          << " mosaicExists="
          << state.value(QStringLiteral("mosaicExists")).toBool()
          << " textExists="
          << state.value(QStringLiteral("textExists")).toBool() << "\n";
      bool capturesReady = true;
      for (const auto &view : views)
        capturesReady &= view->rootObject()->property("captureReady").toBool();
      const QRectF beforeResize = controller.selection();
      QKeyEvent selectKey(QEvent::KeyPress, Qt::Key_V, Qt::NoModifier,
                          QStringLiteral("v"));
      views[0]->rootObject()->forceActiveFocus();
      QCoreApplication::sendEvent(views[0].get(), &selectKey);
      sendMouse(QEvent::MouseButtonPress, QPointF(88, 88), Qt::LeftButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseMove, QPointF(70, 70), Qt::NoButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseButtonRelease, QPointF(70, 70), Qt::LeftButton,
                Qt::NoButton);
      const QRectF afterResize = controller.selection();
      const bool cornerResizeWorked =
          qAbs(afterResize.left() - (beforeResize.left() - 30)) < 0.1 &&
          qAbs(afterResize.top() - (beforeResize.top() - 30)) < 0.1 &&
          qAbs(afterResize.right() - beforeResize.right()) < 0.1 &&
          qAbs(afterResize.bottom() - beforeResize.bottom()) < 0.1;
      const auto &monitorGeometry = controller.monitors()[0].geometry;
      const qreal middleX = afterResize.center().x() - monitorGeometry.x();
      const qreal topY = afterResize.top() - monitorGeometry.y();
      sendMouse(QEvent::MouseButtonPress, QPointF(middleX, topY - 12),
                Qt::LeftButton, Qt::LeftButton);
      sendMouse(QEvent::MouseMove, QPointF(middleX, topY - 30), Qt::NoButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseButtonRelease, QPointF(middleX, topY - 30),
                Qt::LeftButton, Qt::NoButton);
      const QRectF afterTopResize = controller.selection();
      handleResizeWorked =
          cornerResizeWorked &&
          qAbs(afterTopResize.top() - (afterResize.top() - 30)) < 0.1 &&
          qAbs(afterTopResize.left() - afterResize.left()) < 0.1 &&
          qAbs(afterTopResize.right() - afterResize.right()) < 0.1 &&
          qAbs(afterTopResize.bottom() - afterResize.bottom()) < 0.1;
      const_cast<QQuickItem *>(root)->forceActiveFocus();
      auto pressArrow = [&](Qt::Key key, Qt::KeyboardModifiers modifiers,
                            bool repeated = false) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), repeated);
        QCoreApplication::sendEvent(views[0].get(), &event);
      };
      pressArrow(Qt::Key_Up, Qt::NoModifier);
      pressArrow(Qt::Key_Down, Qt::NoModifier);
      pressArrow(Qt::Key_Left, Qt::NoModifier);
      pressArrow(Qt::Key_Right, Qt::NoModifier);
      const QRectF expanded = controller.selection();
      arrowResizeWorked = expanded == afterTopResize.adjusted(-1, -1, 1, 1);
      pressArrow(Qt::Key_Up, Qt::ShiftModifier);
      pressArrow(Qt::Key_Down, Qt::ShiftModifier);
      pressArrow(Qt::Key_Left, Qt::ShiftModifier);
      pressArrow(Qt::Key_Right, Qt::ShiftModifier);
      arrowResizeWorked &= controller.selection() == afterTopResize;
      pressArrow(Qt::Key_Right, Qt::NoModifier);
      for (int i = 0; i < 3; ++i)
        pressArrow(Qt::Key_Right, Qt::NoModifier, true);
      repeatingArrowResizeWorked =
          controller.selection() == afterTopResize.adjusted(0, 0, 4, 0);
      pressArrow(Qt::Key_Right, Qt::ShiftModifier);
      for (int i = 0; i < 3; ++i)
        pressArrow(Qt::Key_Right, Qt::ShiftModifier, true);
      repeatingArrowResizeWorked &= controller.selection() == afterTopResize;

      const QPointF rectGroupButton = toolbarCenter(1);
      sendMouse(QEvent::MouseButtonPress, rectGroupButton, Qt::LeftButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseButtonRelease, rectGroupButton, Qt::LeftButton,
                Qt::NoButton);
      auto *activeVariantPanel =
          root->findChild<QQuickItem *>(QStringLiteral("variantPanel"));
      const bool variantOpened =
          activeVariantPanel && activeVariantPanel->isVisible();
      const int beforeFirstDrag = controller.annotations().size();
      sendMouse(QEvent::MouseButtonPress, QPointF(500, 350), Qt::LeftButton,
                Qt::LeftButton);
      const bool firstPressStartedDrawing =
          variantOpened && !activeVariantPanel->isVisible() &&
          controller.draft().value(QStringLiteral("type")).toString() ==
              QStringLiteral("roundrect");
      sendMouse(QEvent::MouseMove, QPointF(560, 400), Qt::NoButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseButtonRelease, QPointF(560, 400), Qt::LeftButton,
                Qt::NoButton);
      variantFirstDragWorked =
          firstPressStartedDrawing &&
          controller.annotations().size() == beforeFirstDrag + 1 &&
          controller.annotations()
                  .last()
                  .toMap()
                  .value(QStringLiteral("type"))
                  .toString() == QStringLiteral("roundrect");

      controller.setTool(QStringLiteral("pen"));
      controller.setTool(QStringLiteral("text"));
      const QPointF altTextSpot(450, 350);
      QTest::mouseClick(views[0].get(), Qt::LeftButton, Qt::NoModifier,
                        altTextSpot.toPoint());
      auto *altTextEditor =
          root->findChild<QQuickItem *>(QStringLiteral("textEditor"));
      const bool altEditorOpened = altTextEditor && altTextEditor->isVisible();
      const int beforeAltAnnotations = controller.annotations().size();
      QKeyEvent altText(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier,
                        QStringLiteral("ALT"));
      QCoreApplication::sendEvent(views[0].get(), &altText);
      QKeyEvent altFromEditor(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
      QCoreApplication::sendEvent(views[0].get(), &altFromEditor);
      const bool altFromEditorWorked =
          altEditorOpened && !altTextEditor->isVisible() &&
          controller.tool() == QStringLiteral("pen") &&
          controller.annotations().size() == beforeAltAnnotations + 1 &&
          controller.annotations()
                  .last()
                  .toMap()
                  .value(QStringLiteral("text"))
                  .toString() == QStringLiteral("ALT");
      controller.setTool(QStringLiteral("text"));
      QKeyEvent altFromTool(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
      QCoreApplication::sendEvent(views[0].get(), &altFromTool);
      const bool altFromToolWorked =
          controller.tool() == QStringLiteral("pen") &&
          controller.annotations().size() == beforeAltAnnotations + 1;

      // Replace wl-copy only for this event test so the user's clipboard stays intact.
      QTemporaryDir clipboardTest;
      const QString fakeCopy = clipboardTest.filePath(QStringLiteral("wl-copy"));
      const QString clipboardFile =
          clipboardTest.filePath(QStringLiteral("copied.png"));
      QFile copyScript(fakeCopy);
      const bool stubReady =
          clipboardTest.isValid() && copyScript.open(QIODevice::WriteOnly) &&
          copyScript.write("#!/bin/sh\ncat > \"$OMARCHY_SCREENSHOT_TEST_CLIPBOARD\"\n") > 0;
      copyScript.close();
      if (stubReady) {
        QFile::setPermissions(fakeCopy, QFile::ReadOwner | QFile::WriteOwner |
                                           QFile::ExeOwner | QFile::ReadGroup |
                                           QFile::ExeGroup | QFile::ReadOther |
                                           QFile::ExeOther);
        const QByteArray originalPath = qgetenv("PATH");
        const bool hadTestClipboard =
            qEnvironmentVariableIsSet("OMARCHY_SCREENSHOT_TEST_CLIPBOARD");
        const QByteArray originalTestClipboard =
            qgetenv("OMARCHY_SCREENSHOT_TEST_CLIPBOARD");
        qputenv("PATH", QFile::encodeName(clipboardTest.path()) + ':' +
                            originalPath);
        qputenv("OMARCHY_SCREENSHOT_TEST_CLIPBOARD",
                QFile::encodeName(clipboardFile));
        bool closedAfterCopy = false;
        const auto doneConnection =
            QObject::connect(&controller, &CaptureController::done, &app,
                             [&] { closedAfterCopy = true; });
        auto *textEditor =
            root->findChild<QQuickItem *>(QStringLiteral("textEditor"));
        if (textEditor)
          textEditor->setVisible(false);
        const_cast<QQuickItem *>(root)->forceActiveFocus();
        const QPointF blankSpot(450, 350);
        auto verifyDoubleClick = [&](const QString &tool) {
          controller.setTool(tool);
          const QImage expectedCopy = controller.renderedImage();
          const int annotationCount = controller.annotations().size();
          closedAfterCopy = false;
          QFile::remove(clipboardFile);
          QTest::mouseDClick(views[0].get(), Qt::LeftButton, Qt::NoModifier,
                             blankSpot.toPoint());
          // Copying finishes on a worker thread; passed checks the outcome.
          (void)QTest::qWaitFor([&] { return closedAfterCopy; }, 5000);
          QFile captured(clipboardFile);
          const QImage clipboardImage = captured.open(QIODevice::ReadOnly)
                                            ? QImage::fromData(captured.readAll())
                                            : QImage();
          const bool passed =
              closedAfterCopy && !clipboardImage.isNull() &&
              clipboardImage.convertToFormat(QImage::Format_ARGB32) ==
                  expectedCopy.convertToFormat(QImage::Format_ARGB32) &&
              controller.annotations().size() == annotationCount;
          return passed;
        };
        doubleClickCopied = verifyDoubleClick(QStringLiteral("select")) &&
                            verifyDoubleClick(QStringLiteral("marker")) &&
                            verifyDoubleClick(QStringLiteral("text"));
        QObject::disconnect(doneConnection);
        qputenv("PATH", originalPath);
        if (hadTestClipboard)
          qputenv("OMARCHY_SCREENSHOT_TEST_CLIPBOARD",
                  originalTestClipboard);
        else
          qunsetenv("OMARCHY_SCREENSHOT_TEST_CLIPBOARD");
      }
      int escapeSignals = 0;
      const auto escapeConnection =
          QObject::connect(&controller, &CaptureController::done, &app,
                           [&] { ++escapeSignals; });
      const QPointF blankSpot(450, 350);
      controller.setTool(QStringLiteral("text"));
      QTest::mouseClick(views[0].get(), Qt::LeftButton, Qt::NoModifier,
                        blankSpot.toPoint());
      auto *textEditor =
          root->findChild<QQuickItem *>(QStringLiteral("textEditor"));
      const bool editorOpened = textEditor && textEditor->isVisible();
      QKeyEvent escapeText(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QCoreApplication::sendEvent(views[0].get(), &escapeText);
      const bool editorEscapeClosed = editorOpened && escapeSignals == 1;
      if (textEditor)
        textEditor->setVisible(false);
      controller.setTool(QStringLiteral("select"));
      const_cast<QQuickItem *>(root)->forceActiveFocus();
      const QPointF rectButton = toolbarCenter(1);
      sendMouse(QEvent::MouseButtonPress, rectButton, Qt::LeftButton,
                Qt::LeftButton);
      sendMouse(QEvent::MouseButtonRelease, rectButton, Qt::LeftButton,
                Qt::NoButton);
      auto *variantPanel =
          root->findChild<QQuickItem *>(QStringLiteral("variantPanel"));
      const bool panelOpened = variantPanel && variantPanel->isVisible();
      QKeyEvent escapePanel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QCoreApplication::sendEvent(views[0].get(), &escapePanel);
      escapeCloses = editorEscapeClosed && panelOpened && escapeSignals == 2;
      QObject::disconnect(escapeConnection);

      // Check long translations on a narrow output without changing desktop settings.
      auto *previewRoot = const_cast<QQuickItem *>(root);
      previewRoot->setWidth(380);
      variantPanel->setVisible(false);
      auto *toolbar = root->findChild<QQuickItem *>(QStringLiteral("toolbar"));
      auto *tooltip = root->findChild<QQuickItem *>(QStringLiteral("toolbarTooltip"));
      auto *label = root->findChild<QQuickItem *>(QStringLiteral("tooltipLabel"));
      // The overlay only mirrors the toolbar's tooltip state, read-only.
      auto *toolbarHost =
          root->findChild<QQuickItem *>(QStringLiteral("annotationToolbar"));
      bool translationLayoutWorked = toolbar && tooltip && label && toolbarHost;
      bool contextualTextHintWorked = false;
      if (translationLayoutWorked) {
        toolbarHost->setProperty("toolbarTooltipText", QCoreApplication::translate(
            "AnnotationToolbar", "Selection · Arrow keys expand, Shift+arrows shrink (1 px)"));
        toolbarHost->setProperty("toolbarTooltipX", 190);
        toolbarHost->setProperty("toolbarTooltipY", toolbar->y());
        toolbarHost->setProperty("toolbarTooltipVisible", true);
        QTest::qWait(50);
        translationLayoutWorked = tooltip->isVisible() && tooltip->x() >= 0 &&
            tooltip->x() + tooltip->width() <= previewRoot->width() &&
            label->property("contentWidth").toReal() <= label->width() + 1 &&
            label->height() <= tooltip->height() - 12 &&
            label->property("horizontalAlignment").toInt() ==
                (uiLocale.textDirection() == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft);

        QFile blockedDirectory(testSettings->filePath(QStringLiteral("not-a-directory")));
        const bool blocked = blockedDirectory.open(QIODevice::WriteOnly);
        blockedDirectory.close();
        const QByteArray oldSaveDirectory = qgetenv("OMARCHY_SCREENSHOT_DIR");
        const bool hadSaveDirectory = qEnvironmentVariableIsSet("OMARCHY_SCREENSHOT_DIR");
        const QString errorPath = blockedDirectory.fileName() +
            QStringLiteral("/<test-folder>/a-long-directory-name-for-wrapping");
        qputenv("OMARCHY_SCREENSHOT_DIR", QFile::encodeName(errorPath));
        controller.save();
        if (hadSaveDirectory)
          qputenv("OMARCHY_SCREENSHOT_DIR", oldSaveDirectory);
        else
          qunsetenv("OMARCHY_SCREENSHOT_DIR");
        auto *status = root->findChild<QQuickItem *>(QStringLiteral("statusPanel"));
        auto *statusLabel = root->findChild<QQuickItem *>(QStringLiteral("statusText"));
        QTest::qWait(50);
        translationLayoutWorked &= blocked && status && statusLabel &&
            status->isVisible() && status->width() <= previewRoot->width() - 16 &&
            statusLabel->property("text").toString() ==
                CaptureController::tr("Cannot create directory: %1").arg(errorPath) &&
            statusLabel->property("textFormat").toInt() == 0 &&
            statusLabel->property("contentWidth").toReal() <= statusLabel->width() + 1 &&
            statusLabel->height() <= status->height() - 16;

        // Optional review crops contain only this app's UI, never captured desktop pixels.
        const QString artifacts = qEnvironmentVariable("OMARCHY_SCREENSHOT_TEST_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
          root->findChild<QQuickItem *>(QStringLiteral("screenCaptureImage"))->setVisible(false);
          root->findChild<QQuickItem *>(QStringLiteral("mosaicOverlay"))->setVisible(false);
          root->findChild<QQuickItem *>(QStringLiteral("selectionBorder"))->setVisible(false);
          root->findChild<QQuickItem *>(QStringLiteral("selectionDimensions"))->setVisible(false);
          translationLayoutWorked &= QDir().mkpath(artifacts);
          for (bool dark : {false, true}) {
            controller.setDarkToolbar(dark);
            auto grab = previewRoot->grabToImage();
            if (!grab) {
              translationLayoutWorked = false;
              continue;
            }
            QSignalSpy ready(grab.get(), &QQuickItemGrabResult::ready);
            if (!ready.wait(1000)) {
              translationLayoutWorked = false;
              continue;
            }
            const QRectF crop = QRectF(tooltip->x(), tooltip->y(), tooltip->width(),
                                       tooltip->height()).united(
                QRectF(toolbar->x(), toolbar->y(), toolbar->width(), toolbar->height()))
                                   .adjusted(-6, -6, 6, 6);
            const qreal scale = grab->image().width() / previewRoot->width();
            const QRect pixels = QRectF(crop.topLeft() * scale, crop.size() * scale)
                                     .toAlignedRect();
            const QString path = QDir(artifacts).filePath(
                uiLocale.name() + (dark ? QStringLiteral("-dark.png")
                                       : QStringLiteral("-light.png")));
            translationLayoutWorked &= grab->image().copy(pixels).save(path);
            const QRect statusPixels = QRectF(status->x() * scale, status->y() * scale,
                status->width() * scale, status->height() * scale).toAlignedRect();
            translationLayoutWorked &= grab->image().copy(statusPixels).save(
                QDir(artifacts).filePath(uiLocale.name() + QStringLiteral("-status.png")));
          }
        }

        // The editing hint must replace hover text, wrap at the edges and disappear
        // on both confirmation paths. Enter stays in text mode; Alt returns to pen.
        contextualTextHintWorked = textEditor != nullptr;
        if (textEditor) {
          controller.setTool(QStringLiteral("pen"));
          controller.setTool(QStringLiteral("text"));
          toolbarHost->setProperty("toolbarTooltipVisible", false);
          const int originalScreenIndex = root->property("screenIndex").toInt();
          for (const QPointF position : {QPointF(12, 20), QPointF(370, 350)}) {
            // Simulate editing on an output that does not own the toolbar.
            previewRoot->setProperty("screenIndex", position.x() == 12 ? originalScreenIndex : -1);
            previewRoot->setProperty("textX", position.x());
            previewRoot->setProperty("textY", position.y());
            textEditor->setVisible(true);
            QTest::qWait(50);
            contextualTextHintWorked &= tooltip->isVisible() &&
                toolbar->isVisible() == (position.x() == 12) &&
                label->property("text").toString() == QCoreApplication::translate(
                    "AnnotationToolbar", "Enter: confirm · Shift+Enter: new line · Alt: confirm & return to previous tool") &&
                tooltip->x() >= 8 && tooltip->x() + tooltip->width() <= previewRoot->width() - 8 &&
                tooltip->y() >= 8 && tooltip->y() + tooltip->height() <= previewRoot->height() - 8 &&
                label->property("contentWidth").toReal() <= label->width() + 1 &&
                !QRectF(tooltip->position(), tooltip->size()).intersects(
                    QRectF(textEditor->position(), textEditor->size()));
            if (!artifacts.isEmpty() && position.x() == 370) {
              auto grab = previewRoot->grabToImage();
              QSignalSpy ready(grab.get(), &QQuickItemGrabResult::ready);
              if (ready.wait(1000)) {
                const qreal scale = grab->image().width() / previewRoot->width();
                const QRectF crop = QRectF(tooltip->position(), tooltip->size()).united(
                    QRectF(textEditor->position(), textEditor->size())).adjusted(-6, -6, 6, 6);
                contextualTextHintWorked &= grab->image().copy(
                    QRectF(crop.topLeft() * scale, crop.size() * scale).toAlignedRect()).save(
                        QDir(artifacts).filePath(uiLocale.name() + QStringLiteral("-editing.png")));
              } else {
                contextualTextHintWorked = false;
              }
            }
          }
          previewRoot->setProperty("screenIndex", originalScreenIndex);
          auto *input = root->findChild<QQuickItem *>(QStringLiteral("textEditorInput"));
          input->forceActiveFocus();
          QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
          QCoreApplication::sendEvent(views[0].get(), &enter);
          contextualTextHintWorked &= !textEditor->isVisible() && !tooltip->isVisible() &&
              controller.tool() == QStringLiteral("text");
          textEditor->setVisible(true);
          input->forceActiveFocus();
          QKeyEvent alt(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
          QCoreApplication::sendEvent(views[0].get(), &alt);
          contextualTextHintWorked &= !textEditor->isVisible() && !tooltip->isVisible() &&
              controller.tool() == QStringLiteral("pen");
        }
      }
      QTextStream(stdout) << "Mosaic preview: " << mosaicVisible
                          << ", toolbar: " << toolbarWorked
                          << ", toolbar order: " << toolbarOrderWorked
                          << ", grouped tools: " << groupedToolbarWorked
                          << ", color picker: " << colorPickerWorked
                          << ", saved color: " << colorPersistenceWorked
                          << ", toolbar theme: " << toolbarThemeWorked
                          << ", button padding: " << toolbarPaddingWorked
                          << ", old/new colors: " << annotationColorsKept
                          << ", colored text export: " << coloredTextExported
                          << ", colored pen export: " << coloredPenExported
                          << ", cursor icon: " << cursorIconRendered
                          << ", eight handles: " << handlesInitiallyVisible
                          << ", handles hidden: " << handlesHiddenForMosaic
                          << ", enlarged hit: " << handleResizeWorked
                          << ", arrow resize: " << arrowResizeWorked
                          << ", held arrow resize: "
                          << repeatingArrowResizeWorked
                          << ", left-hand keys: " << leftHandHotkeysWorked
                          << ", variant first drag: " << variantFirstDragWorked
                          << ", redo X: " << redoShortcutWorked
                          << ", escape closes: " << escapeCloses
                          << ", hover text: " << hoverDescriptionWorked
                          << ", text Alt: " << altFromEditorWorked
                          << ", tool Alt: " << altFromToolWorked
                          << ", drag: " << dragPreviewWorked
                          << ", export changed: " << mosaicExportChanged
                          << ", rectangle shape: " << rectangleShapeWorked
                          << ", visible pixels: " << mosaicPixelsVisible
                          << ", draft window: " << mosaicDraftWindowMatchesImage
                          << ", committed window: " << mosaicWindowMatchesImage
                          << ", transparent outside: " << mosaicMaskTransparent
                          << ", drag border: " << dragBorderPixelsVisible
                          << ", border gone: " << dragBorderGone
                          << ", pen draft: " << penDraftWorked
                          << ", pen export: " << penExportChanged
                          << ", pen preview: " << penPreviewVisible
                          << ", text preview: " << textVisible
                          << ", multiline edit: " << multilineEditing
                          << ", dashed border: " << editorBorderDashed
                          << ", transparent editor: "
                          << editorBackgroundTransparent
                          << ", centered lines: " << previewRowsCentered
                          << ", exported rows: " << exportedTextRows
                          << ", editor focused: " << editorFocused
                          << ", captures ready: " << capturesReady
                          << ", double-click copy: " << doubleClickCopied
                          << ", translation layout: " << translationLayoutWorked
                          << ", contextual text hint: " << contextualTextHintWorked << "\n";
      app.exit(mosaicVisible && toolbarWorked && toolbarOrderWorked &&
                       groupedToolbarWorked && redoShortcutWorked &&
                       escapeCloses &&
                       colorPickerWorked &&
                       colorPersistenceWorked && toolbarThemeWorked &&
                       toolbarPaddingWorked &&
                       annotationColorsKept && coloredTextExported &&
                       coloredPenExported &&
                       cursorIconRendered && handlesInitiallyVisible &&
                       handlesHiddenForMosaic && handleResizeWorked &&
                       arrowResizeWorked && repeatingArrowResizeWorked &&
                       leftHandHotkeysWorked && variantFirstDragWorked &&
                       hoverDescriptionWorked &&
                       altFromEditorWorked && altFromToolWorked &&
                       dragPreviewWorked && mosaicExportChanged &&
                       rectangleShapeWorked && mosaicPixelsVisible &&
                       mosaicDraftWindowMatchesImage && mosaicWindowMatchesImage &&
                       mosaicMaskTransparent && dragBorderPixelsVisible &&
                       dragBorderGone && penDraftWorked && penExportChanged &&
                       penPreviewVisible && textVisible && multilineEditing &&
                       editorBorderDashed && editorBackgroundTransparent &&
                       previewRowsCentered && exportedTextRows &&
                       editorFocused && capturesReady && doubleClickCopied &&
                       translationLayoutWorked && contextualTextHintWorked
                   ? 0
                   : 2);
    });
  }
  return app.exec();
}
