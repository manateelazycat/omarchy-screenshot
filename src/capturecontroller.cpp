// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

#include "capturecontroller.h"
#include "cursorcaptureguard.h"
#include "ocr.h"
#include "virtualpointer.h"

#include "screencapture.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace {
QByteArray run(const QString &program, const QStringList &arguments,
               QString *error) {
  QProcess process;
  process.start(program, arguments);
  if (!process.waitForStarted(3000) || !process.waitForFinished(15000) ||
      process.exitCode() != 0) {
    if (error) {
      QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
      if (detail.isEmpty())
        detail = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
      *error = QStringLiteral("%1: %2").arg(program, detail);
    }
    return {};
  }
  return process.readAllStandardOutput();
}

bool copyBytes(const QByteArray &bytes, const QString &mimeType,
               QString *error) {
  QProcess process;
  process.start(QStringLiteral("wl-copy"),
                {QStringLiteral("--type"), mimeType});
  if (!process.waitForStarted(3000)) {
    if (error)
      *error = CaptureController::tr("Cannot start wl-copy: %1")
                   .arg(process.errorString());
    return false;
  }
  process.write(bytes);
  process.closeWriteChannel();
  if (!process.waitForFinished(10000) || process.exitCode() != 0) {
    if (error)
      *error = CaptureController::tr("Failed to copy to the clipboard: %1")
                   .arg(QString::fromUtf8(process.readAllStandardError()));
    return false;
  }
  return true;
}

QVariantMap pointMap(const QPointF &p) {
  return {{QStringLiteral("x"), p.x()}, {QStringLiteral("y"), p.y()}};
}

QPointF fromMap(const QVariant &value) {
  const auto map = value.toMap();
  return {map.value(QStringLiteral("x")).toDouble(),
          map.value(QStringLiteral("y")).toDouble()};
}

QPointF limited(const QPointF &p, const QRectF &rect) {
  return {std::clamp(p.x(), rect.left(), rect.right()),
          std::clamp(p.y(), rect.top(), rect.bottom())};
}

int resizeHandleAt(const QRectF &rect, const QPointF &point,
                   qreal hitRadius = 15) {
  const qreal middleX = rect.center().x();
  const qreal middleY = rect.center().y();
  struct Handle {
    QPointF center;
    int edges;
  };
  const Handle handles[] = {
      {{rect.left(), rect.top()}, 1 | 4},
      {{middleX, rect.top()}, 4},
      {{rect.right(), rect.top()}, 2 | 4},
      {{rect.left(), middleY}, 1},
      {{rect.right(), middleY}, 2},
      {{rect.left(), rect.bottom()}, 1 | 8},
      {{middleX, rect.bottom()}, 8},
      {{rect.right(), rect.bottom()}, 2 | 8},
  };
  qreal closest = std::numeric_limits<qreal>::max();
  int edges = 0;
  for (const auto &handle : handles) {
    const qreal dx = point.x() - handle.center.x();
    const qreal dy = point.y() - handle.center.y();
    if (std::abs(dx) > hitRadius || std::abs(dy) > hitRadius)
      continue;
    const qreal distance = dx * dx + dy * dy;
    if (distance < closest) {
      closest = distance;
      edges = handle.edges;
    }
  }
  return edges;
}

QVector<QPointF> filteredFreehandPoints(const QVariantList &raw) {
  QVector<QPointF> samples;
  samples.reserve(raw.size());
  for (const QVariant &value : raw)
    samples.append(fromMap(value));
  QVector<QPointF> points = samples;
  // A 1-2-1 filter suppresses alternating mouse jitter while keeping both ends
  // at the exact press and release positions.
  for (int i = 1; i + 1 < points.size(); ++i)
    points[i] = (samples[i - 1] + samples[i] * 2 + samples[i + 1]) / 4;
  return points;
}

QVariantMap translatedAnnotation(QVariantMap item, const QPointF &delta) {
  for (const QString &key : {QStringLiteral("start"), QStringLiteral("end")}) {
    if (item.contains(key))
      item.insert(key, pointMap(fromMap(item.value(key)) + delta));
  }
  if (item.contains(QStringLiteral("points"))) {
    QVariantList shifted;
    for (const QVariant &point : item.value(QStringLiteral("points")).toList())
      shifted.append(pointMap(fromMap(point) + delta));
    item.insert(QStringLiteral("points"), shifted);
  }
  return item;
}

QPainterPath spotlightHoles(const QVariantList &annotations) {
  QPainterPath holes;
  holes.setFillRule(Qt::WindingFill);
  for (const QVariant &annotation : annotations) {
    const QVariantMap item = annotation.toMap();
    if (item.value(QStringLiteral("type")).toString() ==
        QStringLiteral("spotlight"))
      holes.addEllipse(QRectF(fromMap(item.value(QStringLiteral("start"))),
                              fromMap(item.value(QStringLiteral("end"))))
                           .normalized());
  }
  return holes;
}
} // namespace

CaptureController::CaptureController(QObject *parent) : QObject(parent) {
  const QColor stored(
      QSettings()
          .value(QStringLiteral("drawing/color"), m_annotationColor)
          .toString());
  if (stored.isValid() && stored.alpha() == 255)
    m_annotationColor = stored.name(QColor::HexRgb);
  m_darkToolbar = QSettings().value(QStringLiteral("toolbar/dark"), false).toBool();
  m_colorSaveTimer.setSingleShot(true);
  m_colorSaveTimer.setInterval(250);
  connect(&m_colorSaveTimer, &QTimer::timeout, this,
          &CaptureController::saveAnnotationColor);
  m_scrollTimer.setSingleShot(true);
  connect(&m_scrollTimer, &QTimer::timeout, this,
          &CaptureController::prepareScrollFrame);
}

CaptureController::~CaptureController() {
  if (m_scrollCaptureThread)
    m_scrollCaptureThread->wait();
  if (m_exportThread)
    m_exportThread->wait();
  if (m_ocrThread)
    m_ocrThread->wait();
  saveAnnotationColor();
}

bool CaptureController::initialize(QString *error) {
  return startCapture(error) && finishCapture(error);
}

CaptureController::CaptureResult
CaptureController::captureMonitors(const QStringList &names,
                                   const QList<bool> &direct) {
  CaptureResult result;
  CursorCaptureGuard cursor(&result.error);
  if (!cursor.ready())
    return result;
  QStringList wanted;
  for (int i = 0; i < names.size(); ++i)
    wanted.append(direct[i] ? names[i] : QString());
  result.images = captureOutputs(wanted, 2000);

  // grim covers what the protocol could not; its outputs are fetched in
  // parallel. PPM skips the PNG compression that dominates grim's runtime.
  std::vector<std::unique_ptr<QProcess>> fallbacks(names.size());
  for (int i = 0; i < names.size(); ++i) {
    if (!result.images[i].isNull())
      continue;
    fallbacks[i] = std::make_unique<QProcess>();
    fallbacks[i]->start(QStringLiteral("grim"),
                        {QStringLiteral("-t"), QStringLiteral("ppm"),
                         QStringLiteral("-o"), names[i], QStringLiteral("-")});
  }
  for (int i = 0; i < names.size(); ++i) {
    QProcess *process = fallbacks[i].get();
    if (!process)
      continue;
    if (!process->waitForStarted(3000) || !process->waitForFinished(15000) ||
        process->exitCode() != 0) {
      result.error = QStringLiteral("grim: %1").arg(
          QString::fromUtf8(process->readAllStandardError()).trimmed());
      return result;
    }
    result.images[i] = QImage::fromData(process->readAllStandardOutput());
  }
  for (int i = 0; i < names.size(); ++i) {
    const QImage &image = result.images[i];
    if (image.isNull()) {
      result.error = tr("Cannot capture monitor %1.").arg(names[i]);
      return result;
    }
    result.mosaics.append(image.scaled(qMax(1, image.width() / 12),
                                       qMax(1, image.height() / 12),
                                       Qt::IgnoreAspectRatio,
                                       Qt::FastTransformation));
  }
  return result;
}

bool CaptureController::finishCapture(QString *error) {
  if (!m_capture.valid())
    return m_imagesReady;
  const CaptureResult result = m_capture.get();
  addWindowCandidates(m_clients.get());
  if (!result.error.isEmpty()) {
    if (error)
      *error = result.error;
    return false;
  }
  for (int i = 0; i < m_monitors.size(); ++i) {
    m_monitors[i].image = result.images[i];
    m_monitors[i].mosaicImage = result.mosaics[i];
  }
  m_imagesReady = true;
  ++m_captureGeneration;
  emit imagesReadyChanged();
  return true;
}

QVariantMap CaptureController::defaultToolVariants() {
  return {{QStringLiteral("rect"), QStringLiteral("rect")},
          {QStringLiteral("ellipse"), QStringLiteral("ellipse")},
          {QStringLiteral("arrow"), QStringLiteral("arrow")},
          {QStringLiteral("pen"), QStringLiteral("pen")}};
}

void CaptureController::reset() {
  cancelPointerAction();
  // Stop scrolling capture first; the generation bump drops any frame or
  // wheel step still queued for the old capture.
  ++m_scrollGeneration;
  m_scrollTimer.stop();
  m_scrollStitcher.reset();
  m_scrollMosaicImage = {};
  m_scrollRegion = {};
  m_scrollWindowAddress.clear();
  m_scrollWindowGeometry = {};
  m_scrollPauseRequested = false;
  m_scrollResumeCheck = false;
  m_scrollNeedsPane = false;
  m_scrollAwaitingPane = false;
  m_scrollPaneSelected = false;
  m_scrollInputSent = false;
  m_scrollReviewInitialized = false;
  m_scrollReviewedHeight = 0;
  m_scrollUnchangedFrames = 0;
  m_scrollNoMatchRetries = 0;
  m_scrollSteps = 1;
  ++m_scrollRevision;
  m_scrollState = ScrollState::Idle;
  m_reviewScale = 1;
  m_monitors.clear();
  m_candidates.clear();
  m_annotations.clear();
  m_redoAnnotations.clear();
  m_selected = false;
  m_selection = {};
  m_hovered = {};
  m_status.clear();
  m_imagesReady = false;
  m_toolBeforeText = QStringLiteral("select");
  m_tool = QStringLiteral("select");
  m_toolVariants = defaultToolVariants();
  ++m_ocrGeneration;
  m_ocrVisible = false;
  m_ocrCopied = false;
  m_ocrPreviewImage = {};
  m_ocrText.clear();
  m_ocrError.clear();
  emit scrollStateChanged();
  emit scrollImageChanged();
  emit scrollAwaitingPaneChanged();
  emit scrollStoppingChanged();
  emit reviewScaleChanged();
  emit selectionChanged();
  emit hoveredChanged();
  emit selectedChanged();
  emit toolChanged();
  emit toolVariantsChanged();
  emit annotationsChanged();
  emit statusChanged();
  emit imagesReadyChanged();
  emit ocrChanged();
}

bool CaptureController::startCapture(QString *error) {
  if (qEnvironmentVariable("XDG_SESSION_TYPE") != QStringLiteral("wayland") ||
      qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
    if (error)
      *error = tr("Run this application in a Hyprland Wayland session.");
    return false;
  }

  QJsonParseError parseError;
  auto monitorData = QJsonDocument::fromJson(
      run(QStringLiteral("hyprctl"),
          {QStringLiteral("-j"), QStringLiteral("monitors")}, error),
      &parseError);
  if (!monitorData.isArray() || monitorData.array().isEmpty()) {
    if (error && error->isEmpty())
      *error = tr("Cannot read monitor information: %1")
                   .arg(parseError.errorString());
    return false;
  }

  const auto screens = QGuiApplication::screens();
  QStringList names;
  QList<bool> direct;
  for (const auto &entry : monitorData.array()) {
    const auto data = entry.toObject();
    CaptureMonitor monitor;
    monitor.name = data.value(QStringLiteral("name")).toString();
    monitor.id = data.value(QStringLiteral("id")).toInt(-1);
    monitor.workspace = data.value(QStringLiteral("activeWorkspace"))
                            .toObject()
                            .value(QStringLiteral("id"))
                            .toInt(-1);
    monitor.screen = nullptr;
    for (QScreen *screen : screens) {
      if (screen->name() == monitor.name) {
        monitor.screen = screen;
        break;
      }
    }
    if (!monitor.screen) {
      if (error)
        *error = tr("Qt cannot find monitor %1.").arg(monitor.name);
      return false;
    }
    const qreal scale = data.value(QStringLiteral("scale")).toDouble(1.0);
    if (scale <= 0) {
      if (error)
        *error = tr("Invalid scale for monitor %1.").arg(monitor.name);
      return false;
    }
    qreal width = data.value(QStringLiteral("width")).toDouble() / scale;
    qreal height = data.value(QStringLiteral("height")).toDouble() / scale;
    const int transform = data.value(QStringLiteral("transform")).toInt();
    if (transform == 1 || transform == 3 || transform == 5 || transform == 7)
      std::swap(width, height);
    monitor.geometry =
        QRectF(data.value(QStringLiteral("x")).toDouble(),
               data.value(QStringLiteral("y")).toDouble(), width, height);

    names.append(monitor.name);
    direct.append(transform == 0);
    m_candidates.push_back({monitor.geometry, false});
    m_monitors.push_back(std::move(monitor));
  }
  m_capture = std::async(std::launch::async, [names, direct] {
    return captureMonitors(names, direct);
  });
  // Hyprland answers IPC only after it finishes the copy, so wait off-thread.
  m_clients = std::async(std::launch::async, [] {
    return run(QStringLiteral("hyprctl"),
               {QStringLiteral("-j"), QStringLiteral("clients")}, nullptr);
  });
  return true;
}

void CaptureController::addWindowCandidates(const QByteArray &json) {
  const auto clients = QJsonDocument::fromJson(json);
  for (const auto &entry : clients.array()) {
    const auto data = entry.toObject();
    if (!data.value(QStringLiteral("mapped")).toBool() ||
        data.value(QStringLiteral("hidden")).toBool() ||
        !data.value(QStringLiteral("visible")).toBool(true))
      continue;
    const int monitorId = data.value(QStringLiteral("monitor")).toInt(-1);
    int monitorIndex = -1;
    for (int i = 0; i < m_monitors.size(); ++i) {
      if (m_monitors[i].id == monitorId) {
        monitorIndex = i;
        break;
      }
    }
    if (monitorIndex < 0)
      continue;
    const int workspace = data.value(QStringLiteral("workspace"))
                              .toObject()
                              .value(QStringLiteral("id"))
                              .toInt(-1);
    if (workspace != m_monitors[monitorIndex].workspace &&
        !data.value(QStringLiteral("pinned")).toBool())
      continue;
    const auto at = data.value(QStringLiteral("at")).toArray();
    const auto size = data.value(QStringLiteral("size")).toArray();
    if (at.size() < 2 || size.size() < 2)
      continue;
    const QRectF rect(at[0].toDouble(), at[1].toDouble(), size[0].toDouble(),
                      size[1].toDouble());
    if (rect.isValid())
      m_candidates.push_back(
          {rect, true, data.value(QStringLiteral("address")).toString(),
           data.value(QStringLiteral("title")).toString(), monitorIndex});
  }
}

QVariantList CaptureController::scrollCandidates() const {
  QVariantList result;
  if (!m_selected || m_scrollState == ScrollState::Reviewing)
    return result;
  for (const Candidate &candidate : m_candidates) {
    if (!candidate.window || candidate.address.isEmpty() ||
        !candidate.geometry.intersects(m_selection))
      continue;
    const QRectF visible = candidate.geometry.intersected(m_selection);
    if (visible.width() < 64 || visible.height() < 64)
      continue;
    result.append(QVariantMap{
        {QStringLiteral("index"), result.size()},
        {QStringLiteral("x"), visible.x()},
        {QStringLiteral("y"), visible.y()},
        {QStringLiteral("width"), visible.width()},
        {QStringLiteral("height"), visible.height()},
        {QStringLiteral("title"), candidate.title}});
  }
  return result;
}

void CaptureController::setScrollState(ScrollState state) {
  if (m_scrollState == state)
    return;
  m_scrollState = state;
  emit scrollStateChanged();
}

void CaptureController::startScroll() {
  if (m_scrollState != ScrollState::Idle || !m_selected ||
      !m_annotations.isEmpty() || m_selection.isEmpty())
    return;
  const int count = scrollCandidates().size();
  if (!count) {
    setStatus(tr("No window to capture in the selection"));
    return;
  }
  if (count == 1)
    chooseScrollWindow(0);
  else
    setScrollState(ScrollState::Choosing);
}

void CaptureController::cancelScrollChoice() {
  if (m_scrollState == ScrollState::Choosing)
    setScrollState(ScrollState::Idle);
}

void CaptureController::chooseScrollWindow(int index) {
  if (m_scrollState != ScrollState::Idle &&
      m_scrollState != ScrollState::Choosing)
    return;
  int seen = 0;
  for (const Candidate &candidate : m_candidates) {
    if (!candidate.window || candidate.address.isEmpty() ||
        !candidate.geometry.intersects(m_selection))
      continue;
    const QRectF visible = candidate.geometry.intersected(m_selection);
    if (visible.width() < 64 || visible.height() < 64)
      continue;
    if (seen++ != index)
      continue;
    if (candidate.monitorIndex < 0 ||
        candidate.monitorIndex >= m_monitors.size())
      return;
    m_scrollRegion = visible.intersected(
        m_monitors[candidate.monitorIndex].geometry);
    if (m_scrollRegion.width() < 64 || m_scrollRegion.height() < 64) {
      setStatus(tr("The scroll area is too small"));
      return;
    }
    m_scrollWindowAddress = candidate.address;
    m_scrollWindowGeometry = candidate.geometry;
    if (!m_virtualPointer)
      m_virtualPointer = std::make_unique<VirtualPointer>();
    if (!m_virtualPointer->available()) {
      setStatus(tr("This compositor lacks the virtual pointer that scrolling capture needs"));
      return;
    }
    m_scrollPoint = m_scrollRegion.center();
    m_scrollStitcher.reset();
    setStatus({});
    m_scrollMosaicImage = {};
    m_scrollReviewInitialized = false;
    m_scrollReviewedHeight = 0;
    m_scrollUnchangedFrames = 0;
    m_scrollNoMatchRetries = 0;
    m_scrollSteps = 1;
    m_scrollPauseRequested = false;
    emit scrollStoppingChanged();
    m_scrollResumeCheck = false;
    m_scrollNeedsPane = false;
    m_scrollAwaitingPane = false;
    m_scrollPaneSelected = false;
    m_scrollInputSent = false;
    ++m_scrollGeneration;
    ++m_scrollRevision;
    emit scrollImageChanged();
    setScrollState(ScrollState::Capturing);
    m_scrollTimer.start(220);
    return;
  }
}

bool CaptureController::scrollWindowUnchanged() {
  const auto clients = QJsonDocument::fromJson(
      run(QStringLiteral("hyprctl"),
          {QStringLiteral("-j"), QStringLiteral("clients")}, nullptr));
  for (const QJsonValue &entry : clients.array()) {
    const QJsonObject data = entry.toObject();
    if (data.value(QStringLiteral("address")).toString() !=
        m_scrollWindowAddress)
      continue;
    const auto at = data.value(QStringLiteral("at")).toArray();
    const auto size = data.value(QStringLiteral("size")).toArray();
    if (at.size() < 2 || size.size() < 2)
      return false;
    const QRectF now(at[0].toDouble(), at[1].toDouble(), size[0].toDouble(),
                     size[1].toDouble());
    return now == m_scrollWindowGeometry;
  }
  return false;
}

bool CaptureController::focusScrollWindow() {
  if (!scrollWindowUnchanged())
    return false;
  bool validAddress = false;
  const qulonglong address = m_scrollWindowAddress.toULongLong(&validAddress, 16);
  if (!validAddress)
    return false;
  QString error;
  run(QStringLiteral("hyprctl"),
      {QStringLiteral("eval"),
       QStringLiteral("hl.dispatch(hl.dsp.focus({ window = \"address:0x%1\" }))")
           .arg(QString::number(address, 16))},
      &error);
  return error.isEmpty();
}

void CaptureController::prepareScrollFrame() {
  if (m_scrollState != ScrollState::Capturing || m_scrollAwaitingPane)
    return;
  emit scrollFrameAboutToCapture();
  const int generation = m_scrollGeneration;
  const auto capture = [this, generation] {
    if (generation == m_scrollGeneration)
      captureScrollFrame();
  };
  // The first frame and its successors must see the same focused window.
  // Focusing only when sending the first wheel event can change decorations,
  // opacity or page content between frames and make overlap detection fail.
  if (!m_scrollInputSent) {
    if (!focusScrollWindow()) {
      setStatus(tr("The window moved or closed, so scrolling cannot start"));
      finishScrollCapture();
      return;
    }
    QTimer::singleShot(250, this, capture);
  } else {
    QTimer::singleShot(100, this, capture);
  }
}

void CaptureController::captureScrollFrame() {
  if (m_scrollState != ScrollState::Capturing || m_scrollAwaitingPane)
    return;
  if (m_scrollCaptureThread) {
    // A cancelled generation may still be restoring the cursor. Keep the UI
    // responsive while waiting for it before capturing the final/resumed frame.
    const int generation = m_scrollGeneration;
    QTimer::singleShot(20, this, [this, generation] {
      if (generation == m_scrollGeneration)
        captureScrollFrame();
    });
    return;
  }
  const QRect region = m_scrollRegion.toAlignedRect();
  const QString geometry =
      QStringLiteral("%1,%2 %3x%4")
          .arg(region.x()).arg(region.y()).arg(region.width()).arg(region.height());
  const int generation = m_scrollGeneration;
  m_scrollCaptureThread = QThread::create([this, generation, geometry] {
    QString error;
    const QImage frame = captureRegionWithoutCursor(geometry, &error);
    QMetaObject::invokeMethod(this, [this, generation, frame, error] {
      if (generation == m_scrollGeneration &&
          m_scrollState == ScrollState::Capturing && !m_scrollAwaitingPane)
        finishScrollFrame(frame, error);
    }, Qt::QueuedConnection);
  });
  connect(m_scrollCaptureThread, &QThread::finished, m_scrollCaptureThread,
          &QObject::deleteLater);
  m_scrollCaptureThread->start();
}

void CaptureController::finishScrollFrame(const QImage &frame,
                                         const QString &error) {
  emit scrollFrameCaptured();
  if (frame.isNull()) {
    setStatus(error.isEmpty() ? tr("Cannot capture the scroll area") : error);
    finishScrollCapture();
    return;
  }
  const ScrollStitcher::Result result = m_scrollStitcher.append(frame);
  if (qEnvironmentVariableIsSet("OMARCHY_SCROLL_DIAGNOSTICS"))
    QTextStream(stderr) << "scroll frame result=" << int(result)
                        << " input=" << m_scrollInputSent
                        << " resume=" << m_scrollResumeCheck
                        << " steps=" << m_scrollSteps
                        << " height=" << scrollHeight() << '\n';
  if (result == ScrollStitcher::Result::NoMatch) {
    // Wait for smooth scrolling or lazy rendering without sending more input.
    if ((m_scrollInputSent || m_scrollResumeCheck) &&
        m_scrollNoMatchRetries++ < 2) {
      m_scrollTimer.start(350);
      return;
    }
    setStatus(tr("Frames could not be stitched reliably; kept what was captured"));
    finishScrollCapture();
    return;
  }
  if (result == ScrollStitcher::Result::TooLarge) {
    setStatus(tr("The long image reached its maximum length; scrolling stopped"));
    finishScrollCapture();
    return;
  }
  if (result == ScrollStitcher::Result::Added) {
    if (m_scrollInputSent && m_scrollStitcher.lastShift() > 0) {
      // Learn how far this particular window moves for one wheel step.
      // Keep the next jump well below the visible height so frames overlap.
      const int suggested = qRound(
          m_scrollSteps * m_scrollRegion.height() * 0.4 /
          m_scrollStitcher.lastShift());
      m_scrollSteps = std::clamp(suggested, 1,
                                std::min(12, m_scrollSteps * 3));
    }
    m_scrollNeedsPane = false;
    m_scrollUnchangedFrames = 0;
    m_scrollMosaicImage = {};
    ++m_scrollRevision;
    emit scrollImageChanged();
  } else if (!m_scrollResumeCheck) {
    ++m_scrollUnchangedFrames;
  }
  m_scrollNoMatchRetries = 0;
  if (m_scrollResumeCheck)
    m_scrollResumeCheck = false;
  if (m_scrollPauseRequested || m_scrollUnchangedFrames >= 2) {
    if (m_scrollUnchangedFrames >= 2) {
      m_scrollNeedsPane = true;
      setStatus(tr("Reached the bottom or the window stopped responding; choose a scroll area to continue"));
    }
    finishScrollCapture();
    return;
  }
  const int generation = m_scrollGeneration;
  QTimer::singleShot(80, this, [this, generation] {
    if (generation == m_scrollGeneration)
      prepareScrollStep();
  });
}

void CaptureController::prepareScrollStep() {
  if (m_scrollState != ScrollState::Capturing || m_scrollPauseRequested ||
      m_scrollAwaitingPane)
    return;
  // Focus while clicks are still intercepted: the hyprctl round trip would
  // otherwise widen the window in which a stop click reaches the app below.
  if (!focusScrollWindow()) {
    setStatus(tr("The window moved or closed; kept what was captured"));
    finishScrollCapture();
    return;
  }
  emit scrollInputAboutToSend();
  const int generation = m_scrollGeneration;
  QTimer::singleShot(80, this, [this, generation] {
    if (generation == m_scrollGeneration)
      sendScrollStep();
  });
}

void CaptureController::sendScrollStep() {
  if (m_scrollState != ScrollState::Capturing || m_scrollPauseRequested ||
      m_scrollAwaitingPane)
    return;
  // The window may have moved or closed during the delay; never send the
  // wheel to whatever now sits under the scroll point.
  if (!scrollWindowUnchanged()) {
    setStatus(tr("The window moved or closed; kept what was captured"));
    finishScrollCapture();
    return;
  }
  QRectF desktop = m_monitors.first().geometry;
  for (const auto &monitor : m_monitors)
    desktop = desktop.united(monitor.geometry);
  QString error;
  if (!m_virtualPointer->scrollAt(m_scrollPoint, desktop, m_scrollSteps,
                                  &error)) {
    setStatus(tr("Cannot scroll the window: %1").arg(error));
    finishScrollCapture();
    return;
  }
  m_scrollInputSent = true;
  m_scrollInputTimer.start();
  emit scrollInputSent();
  m_scrollTimer.start(550);
}

void CaptureController::restoreScrollInputFocus() {
  if (m_scrollState != ScrollState::Capturing || !m_virtualPointer)
    return;
  QString error;
  if (!m_virtualPointer->refreshFocus(&error)) {
    setStatus(error);
    finishScrollCapture();
  }
}

void CaptureController::finishScrollCapture() {
  ++m_scrollGeneration;
  m_scrollTimer.stop();
  if (m_scrollStitcher.image().isNull()) {
    setScrollState(ScrollState::Idle);
    return;
  }
  const QSize size = m_scrollStitcher.image().size();
  if (!m_scrollReviewInitialized) {
    // The long image replaces the canvas only now; a capture that fails
    // before its first frame returns to the screenshot with redo intact.
    m_redoAnnotations.clear();
    m_selected = true;
    setSelection(QRectF(QPointF(0, 0), QSizeF(size)));
    m_scrollReviewInitialized = true;
  } else if (qAbs(m_selection.bottom() - m_scrollReviewedHeight) < 1) {
    setSelection(QRectF(m_selection.topLeft(),
                        QPointF(m_selection.right(), size.height())));
  }
  m_scrollReviewedHeight = size.height();
  if (m_tool == QStringLiteral("mosaic") || !m_scrollMosaicImage.isNull() ||
      std::any_of(m_annotations.cbegin(), m_annotations.cend(),
                  [](const QVariant &value) {
                    return value.toMap().value(QStringLiteral("type")) ==
                           QStringLiteral("mosaic");
                  }))
    prepareScrollMosaic();
  setScrollState(ScrollState::Reviewing);
}

void CaptureController::pauseScroll() {
  if (qEnvironmentVariableIsSet("OMARCHY_SCROLL_DIAGNOSTICS"))
    QTextStream(stderr) << "scroll pause requested state=" << scrollState() << '\n';
  if (m_scrollState != ScrollState::Capturing || m_scrollPauseRequested)
    return;
  if (m_scrollAwaitingPane) {
    m_scrollAwaitingPane = false;
    emit scrollAwaitingPaneChanged();
    finishScrollCapture();
    return;
  }
  m_scrollPauseRequested = true;
  emit scrollStoppingChanged();
  ++m_scrollGeneration;
  m_scrollTimer.stop();
  // Stop input immediately, then capture the final settled position.
  const int remaining = m_scrollInputSent
      ? std::max(180, 550 - int(m_scrollInputTimer.elapsed())) : 180;
  m_scrollTimer.start(remaining);
}

void CaptureController::resumeScroll() {
  if (m_scrollState != ScrollState::Reviewing)
    return;
  setStatus({});
  ++m_scrollGeneration;
  m_scrollPauseRequested = false;
  emit scrollStoppingChanged();
  m_scrollResumeCheck = true;
  m_scrollUnchangedFrames = 0;
  m_scrollNoMatchRetries = 0;
  m_scrollInputSent = false;
  setScrollState(ScrollState::Capturing);
  if (m_scrollNeedsPane) {
    m_scrollAwaitingPane = true;
    m_scrollPaneSelected = false;
    emit scrollAwaitingPaneChanged();
    setStatus(tr("Click the area to scroll, then click Continue scrolling"));
  } else
    m_scrollTimer.start(220);
}

void CaptureController::continueAfterPane() {
  if (m_scrollState != ScrollState::Capturing || !m_scrollAwaitingPane ||
      !m_scrollPaneSelected)
    return;
  m_scrollInputSent = false;
  m_scrollAwaitingPane = false;
  emit scrollAwaitingPaneChanged();
  setStatus({});
  m_scrollTimer.start(220);
}

void CaptureController::selectScrollPane(qreal x, qreal y) {
  if (m_scrollState != ScrollState::Capturing || !m_scrollAwaitingPane ||
      !m_scrollRegion.contains(QPointF(x, y)))
    return;
  m_scrollPoint = QPointF(x, y);
  m_scrollPaneSelected = true;
  emit scrollAwaitingPaneChanged();
}

void CaptureController::prepareScrollMosaic() {
  const QImage &image = m_scrollStitcher.image();
  if (image.isNull() || !m_scrollMosaicImage.isNull())
    return;
  const QImage coarse =
      image.scaled(qMax(1, image.width() / 12),
                   qMax(1, image.height() / 12),
                   Qt::IgnoreAspectRatio, Qt::FastTransformation);
  m_scrollMosaicImage = coarse.scaled(
      image.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

QPointF CaptureController::globalPoint(int screenIndex, qreal x,
                                       qreal y) const {
  if (screenIndex == -1 && m_scrollState == ScrollState::Reviewing)
    return QPointF(x, y);
  if (screenIndex < 0 || screenIndex >= m_monitors.size())
    return {};
  return m_monitors[screenIndex].geometry.topLeft() + QPointF(x, y);
}

QRectF CaptureController::candidateAt(const QPointF &point) const {
  QRectF best;
  qreal bestArea = std::numeric_limits<qreal>::max();
  for (const auto &candidate : m_candidates) {
    if (!candidate.geometry.contains(point))
      continue;
    const qreal area = candidate.geometry.width() * candidate.geometry.height();
    if (area < bestArea || (area == bestArea && candidate.window)) {
      best = candidate.geometry;
      bestArea = area;
    }
  }
  return best;
}

void CaptureController::setSelection(const QRectF &rect) {
  if (m_selection == rect)
    return;
  m_selection = rect;
  emit selectionChanged();
}

void CaptureController::setHovered(const QRectF &rect) {
  if (m_hovered == rect)
    return;
  m_hovered = rect;
  emit hoveredChanged();
}

void CaptureController::setStatus(const QString &message) {
  m_status = message;
  emit statusChanged();
}

int CaptureController::toolbarScreen() const {
  int best = -1;
  qreal area = 0;
  for (int i = 0; i < m_monitors.size(); ++i) {
    const auto shared = m_selection.intersected(m_monitors[i].geometry);
    const qreal sharedArea = shared.width() * shared.height();
    if (sharedArea > area) {
      best = i;
      area = sharedArea;
    }
  }
  return best;
}

void CaptureController::setTool(const QString &tool) {
  if (tool != QStringLiteral("select") && tool != QStringLiteral("rect") &&
      tool != QStringLiteral("ellipse") && tool != QStringLiteral("arrow") &&
      tool != QStringLiteral("pen") && tool != QStringLiteral("text") &&
      tool != QStringLiteral("mosaic") && tool != QStringLiteral("line") &&
      tool != QStringLiteral("highlighter") &&
      tool != QStringLiteral("spotlight") && tool != QStringLiteral("marker") &&
      tool != QStringLiteral("roundrect") &&
      tool != QStringLiteral("fillrect") &&
      tool != QStringLiteral("fillellipse") &&
      tool != QStringLiteral("curvedarrow") &&
      tool != QStringLiteral("doublearrow"))
    return;
  QString group;
  if (tool == QStringLiteral("rect") || tool == QStringLiteral("roundrect") ||
      tool == QStringLiteral("fillrect"))
    group = QStringLiteral("rect");
  else if (tool == QStringLiteral("ellipse") ||
           tool == QStringLiteral("fillellipse") ||
           tool == QStringLiteral("spotlight"))
    group = QStringLiteral("ellipse");
  else if (tool == QStringLiteral("arrow") ||
           tool == QStringLiteral("curvedarrow") ||
           tool == QStringLiteral("doublearrow") ||
           tool == QStringLiteral("line"))
    group = QStringLiteral("arrow");
  else if (tool == QStringLiteral("pen") ||
           tool == QStringLiteral("highlighter"))
    group = QStringLiteral("pen");
  if (!group.isEmpty() && m_toolVariants.value(group).toString() != tool) {
    m_toolVariants.insert(group, tool);
    emit toolVariantsChanged();
  }
  if (tool == QStringLiteral("mosaic") &&
      m_scrollState == ScrollState::Reviewing)
    prepareScrollMosaic();
  if (m_tool == tool)
    return;
  if (tool == QStringLiteral("text"))
    m_toolBeforeText = m_tool;
  m_tool = tool;
  emit toolChanged();
}

void CaptureController::restoreToolBeforeText() {
  if (m_tool == QStringLiteral("text"))
    setTool(m_toolBeforeText);
}

void CaptureController::activateToolGroup(const QString &group) {
  if (group != QStringLiteral("rect") && group != QStringLiteral("ellipse") &&
      group != QStringLiteral("arrow") && group != QStringLiteral("pen"))
    return;
  setTool(m_toolVariants.value(group).toString());
}

void CaptureController::setAnnotationColor(const QString &color) {
  const QColor parsed(color);
  if (!parsed.isValid() || parsed.alpha() != 255)
    return;
  const QString normalized = parsed.name(QColor::HexRgb);
  if (m_annotationColor == normalized)
    return;
  m_annotationColor = normalized;
  m_colorDirty = true;
  m_colorSaveTimer.start();
  emit annotationColorChanged();
}

void CaptureController::setAnnotationColorFromHsv(qreal hue, qreal saturation,
                                                  qreal value) {
  setAnnotationColor(
      QColor::fromHsvF(std::clamp(hue, qreal(0), qreal(1)),
                       std::clamp(saturation, qreal(0), qreal(1)),
                       std::clamp(value, qreal(0), qreal(1)))
          .name(QColor::HexRgb));
}

void CaptureController::saveAnnotationColor() {
  if (!m_colorDirty)
    return;
  m_colorSaveTimer.stop();
  QSettings settings;
  settings.setValue(QStringLiteral("drawing/color"), m_annotationColor);
  settings.sync();
  m_colorDirty = false;
}

void CaptureController::setDarkToolbar(bool dark) {
  if (m_darkToolbar == dark)
    return;
  m_darkToolbar = dark;
  QSettings settings;
  settings.setValue(QStringLiteral("toolbar/dark"), dark);
  settings.sync();
  emit darkToolbarChanged();
}

void CaptureController::adjustSelectionEdge(int key, bool shrink) {
  if (!m_selected || m_tool != QStringLiteral("select") ||
      m_selection.isEmpty() || m_monitors.isEmpty())
    return;

  QRectF desktop;
  if (m_scrollState == ScrollState::Reviewing)
    desktop = QRectF(QPointF(0, 0), m_scrollStitcher.image().size());
  else {
    desktop = m_monitors.first().geometry;
    for (const auto &monitor : m_monitors)
      desktop = desktop.united(monitor.geometry);
  }

  qreal left = m_selection.left();
  qreal right = m_selection.right();
  qreal top = m_selection.top();
  qreal bottom = m_selection.bottom();
  switch (key) {
  case Qt::Key_Up:
    top = shrink ? std::min(top + 1, bottom - 2)
                 : std::max(top - 1, desktop.top());
    break;
  case Qt::Key_Down:
    bottom = shrink ? std::max(bottom - 1, top + 2)
                    : std::min(bottom + 1, desktop.bottom());
    break;
  case Qt::Key_Left:
    left = shrink ? std::min(left + 1, right - 2)
                  : std::max(left - 1, desktop.left());
    break;
  case Qt::Key_Right:
    right = shrink ? std::max(right - 1, left + 2)
                   : std::min(right + 1, desktop.right());
    break;
  default:
    return;
  }
  const QRectF next(QPointF(left, top), QPointF(right, bottom));
  if (next != m_selection) {
    m_redoAnnotations.clear();
    setSelection(next);
  }
}

void CaptureController::pointerMove(int screenIndex, qreal x, qreal y) {
  const QPointF point = globalPoint(screenIndex, x, y);
  if (m_drag == Drag::None) {
    if (!m_selected)
      setHovered(candidateAt(point));
    return;
  }
  if (QLineF(m_press, point).length() > 4)
    m_moved = true;
  if (m_drag == Drag::Select) {
    if (m_moved)
      setSelection(QRectF(m_press, point).normalized());
  } else if (m_drag == Drag::Move) {
    const QPointF delta = point - m_press;
    QRectF next = m_initialSelection.translated(delta);
    if (m_scrollState == ScrollState::Reviewing) {
      const QRectF bounds(QPointF(0, 0), m_scrollStitcher.image().size());
      next.moveLeft(std::clamp(next.left(), bounds.left(),
                               bounds.right() - next.width()));
      next.moveTop(std::clamp(next.top(), bounds.top(),
                              bounds.bottom() - next.height()));
    }
    setSelection(next);
    if (m_scrollState != ScrollState::Reviewing &&
        !m_initialAnnotations.isEmpty()) {
      m_annotations.clear();
      for (const QVariant &item : m_initialAnnotations)
        m_annotations.append(translatedAnnotation(item.toMap(), delta));
      emit annotationsChanged();
    }
  } else if (m_drag == Drag::Resize) {
    qreal left = m_initialSelection.left(), right = m_initialSelection.right();
    qreal top = m_initialSelection.top(), bottom = m_initialSelection.bottom();
    if (m_resizeEdges & 1)
      left = std::min(point.x(), right - 2);
    if (m_resizeEdges & 2)
      right = std::max(point.x(), left + 2);
    if (m_resizeEdges & 4)
      top = std::min(point.y(), bottom - 2);
    if (m_resizeEdges & 8)
      bottom = std::max(point.y(), top + 2);
    if (m_scrollState == ScrollState::Reviewing) {
      left = std::clamp(left, qreal(0), qreal(m_scrollStitcher.image().width()));
      right = std::clamp(right, qreal(0), qreal(m_scrollStitcher.image().width()));
      top = std::clamp(top, qreal(0), qreal(m_scrollStitcher.image().height()));
      bottom = std::clamp(bottom, qreal(0), qreal(m_scrollStitcher.image().height()));
    }
    setSelection(QRectF(QPointF(left, top), QPointF(right, bottom)));
  } else if (m_drag == Drag::Draw) {
    const QPointF inside = limited(point, m_selection);
    if (m_tool == QStringLiteral("pen") ||
        m_tool == QStringLiteral("highlighter")) {
      auto points = m_draft.value(QStringLiteral("points")).toList();
      if (points.isEmpty() ||
          QLineF(fromMap(points.last()), inside).length() >= 2) {
        points.append(pointMap(inside));
        m_draft.insert(QStringLiteral("points"), points);
      }
    }
    m_draft.insert(QStringLiteral("end"), pointMap(inside));
    emit draftChanged();
  }
}

void CaptureController::pointerPress(int screenIndex, qreal x, qreal y) {
  const QPointF point = globalPoint(screenIndex, x, y);
  // Review coordinates are image pixels; hit areas are meant in screen pixels.
  const qreal hitScale =
      m_scrollState == ScrollState::Reviewing && m_reviewScale > 0
          ? m_reviewScale : 1;
  m_press = point;
  m_moved = false;
  if (m_selected && m_tool != QStringLiteral("select")) {
    if (!m_selection.contains(point) || m_tool == QStringLiteral("text"))
      return;
    m_drag = Drag::Draw;
    m_draft = {{QStringLiteral("type"), m_tool},
               {QStringLiteral("color"), m_annotationColor},
               {QStringLiteral("start"), pointMap(point)},
               {QStringLiteral("end"), pointMap(point)}};
    if (m_tool == QStringLiteral("pen") ||
        m_tool == QStringLiteral("highlighter"))
      m_draft.insert(QStringLiteral("points"), QVariantList{pointMap(point)});
    emit draftChanged();
    return;
  }
  if (m_selected && m_tool == QStringLiteral("select")) {
    m_resizeEdges = resizeHandleAt(m_selection, point, 15 / hitScale);
    if (m_resizeEdges) {
      m_initialSelection = m_selection;
      m_drag = Drag::Resize;
      return;
    }
  }
  const qreal edge = 7 / hitScale;
  if (m_selected && m_selection.adjusted(-edge, -edge, edge, edge).contains(point)) {
    m_initialSelection = m_selection;
    m_resizeEdges = 0;
    if (std::abs(point.x() - m_selection.left()) <= edge)
      m_resizeEdges |= 1;
    if (std::abs(point.x() - m_selection.right()) <= edge)
      m_resizeEdges |= 2;
    if (std::abs(point.y() - m_selection.top()) <= edge)
      m_resizeEdges |= 4;
    if (std::abs(point.y() - m_selection.bottom()) <= edge)
      m_resizeEdges |= 8;
    m_drag = m_resizeEdges ? Drag::Resize : Drag::Move;
    if (m_drag == Drag::Move && m_scrollState != ScrollState::Reviewing)
      m_initialAnnotations = m_annotations;
    return;
  }
  if (m_scrollState == ScrollState::Reviewing)
    return;
  if (!m_annotations.isEmpty()) {
    m_annotations.clear();
    emit annotationsChanged();
  }
  m_redoAnnotations.clear();
  m_selected = false;
  emit selectedChanged();
  setSelection({});
  setHovered(candidateAt(point));
  m_drag = Drag::Select;
}

void CaptureController::pointerRelease(int screenIndex, qreal x, qreal y) {
  if (m_drag == Drag::None)
    return;
  pointerMove(screenIndex, x, y);
  if (m_drag == Drag::Draw && (m_tool == QStringLiteral("pen") ||
                               m_tool == QStringLiteral("highlighter"))) {
    const QPointF release = limited(globalPoint(screenIndex, x, y), m_selection);
    auto points = m_draft.value(QStringLiteral("points")).toList();
    if (!points.isEmpty() && fromMap(points.last()) != release) {
      points.append(pointMap(release));
      m_draft.insert(QStringLiteral("points"), points);
      emit draftChanged();
    }
  }
  if (m_drag == Drag::Select) {
    if (!m_moved)
      setSelection(candidateAt(m_press));
    if (m_selection.width() >= 2 && m_selection.height() >= 2) {
      m_selected = true;
      emit selectedChanged();
      setHovered({});
    } else
      setSelection({});
  } else if (m_drag == Drag::Draw &&
             (m_moved || m_tool == QStringLiteral("marker"))) {
    const QRectF area = QRectF(fromMap(m_draft.value(QStringLiteral("start"))),
                               fromMap(m_draft.value(QStringLiteral("end"))))
                            .normalized();
    if (m_tool != QStringLiteral("mosaic") ||
        (area.width() >= 2 && area.height() >= 2)) {
      if (m_tool == QStringLiteral("marker")) {
        int number = 1;
        for (const QVariant &mark : m_annotations)
          if (mark.toMap().value(QStringLiteral("type")).toString() ==
              QStringLiteral("marker"))
            number = std::max(
                number,
                mark.toMap().value(QStringLiteral("number")).toInt() + 1);
        m_draft.insert(QStringLiteral("number"), number);
      }
      appendAnnotation(m_draft);
    }
  }
  if (m_drag == Drag::Draw) {
    m_draft.clear();
    emit draftChanged();
  }
  if (m_moved && (m_drag == Drag::Move || m_drag == Drag::Resize))
    m_redoAnnotations.clear();
  m_drag = Drag::None;
  m_initialAnnotations.clear();
}

void CaptureController::cancelPointerAction() {
  m_drag = Drag::None;
  m_resizeEdges = 0;
  m_initialAnnotations.clear();
  if (!m_draft.isEmpty()) {
    m_draft.clear();
    emit draftChanged();
  }
}

void CaptureController::addText(qreal x, qreal y, const QString &text) {
  if (!m_selected || text.trimmed().isEmpty())
    return;
  const QPointF point = limited(QPointF(x, y), m_selection);
  appendAnnotation(QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("start"), pointMap(point)},
                               {QStringLiteral("color"), m_annotationColor},
                               {QStringLiteral("text"), text}});
}

void CaptureController::appendAnnotation(const QVariantMap &item) {
  m_annotations.append(item);
  m_redoAnnotations.clear();
  emit annotationsChanged();
}

void CaptureController::undo() {
  if (m_annotations.isEmpty())
    return;
  m_redoAnnotations.append(m_annotations.last());
  m_annotations.removeLast();
  emit annotationsChanged();
}

void CaptureController::redo() {
  if (m_redoAnnotations.isEmpty())
    return;
  m_annotations.append(m_redoAnnotations.takeLast());
  emit annotationsChanged();
}

void CaptureController::paintAnnotation(QPainter &painter,
                                        const QVariantMap &item) const {
  const QString type = item.value(QStringLiteral("type")).toString();
  const QPointF start = fromMap(item.value(QStringLiteral("start")));
  const QPointF end = fromMap(item.value(QStringLiteral("end")));
  QColor color(item.value(QStringLiteral("color")).toString());
  if (!color.isValid())
    color = QColor(QStringLiteral("#ff4b55"));
  painter.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  if (type == QStringLiteral("rect"))
    painter.drawRect(QRectF(start, end).normalized());
  else if (type == QStringLiteral("roundrect"))
    painter.drawRoundedRect(QRectF(start, end).normalized(), 10, 10);
  else if (type == QStringLiteral("fillrect"))
    painter.fillRect(QRectF(start, end).normalized(), color);
  else if (type == QStringLiteral("fillellipse")) {
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(start, end).normalized());
  } else if (type == QStringLiteral("ellipse"))
    painter.drawEllipse(QRectF(start, end).normalized());
  else if (type == QStringLiteral("line"))
    painter.drawLine(start, end);
  else if (type == QStringLiteral("arrow")) {
    painter.drawLine(start, end);
    const qreal angle = std::atan2(end.y() - start.y(), end.x() - start.x());
    const qreal wing = 12;
    painter.drawLine(end, end - QPointF(std::cos(angle - .55) * wing,
                                        std::sin(angle - .55) * wing));
    painter.drawLine(end, end - QPointF(std::cos(angle + .55) * wing,
                                        std::sin(angle + .55) * wing));
  } else if (type == QStringLiteral("curvedarrow") ||
             type == QStringLiteral("doublearrow")) {
    const QPointF delta = end - start;
    const QPointF control =
        (start + end) / 2 + QPointF(-delta.y(), delta.x()) * .22;
    QPainterPath curve(start);
    curve.quadTo(control, end);
    painter.drawPath(curve);
    const auto head = [&](const QPointF &tip, const QPointF &from) {
      const qreal angle = std::atan2(tip.y() - from.y(), tip.x() - from.x());
      painter.drawLine(tip, tip - QPointF(std::cos(angle - .55) * 12,
                                          std::sin(angle - .55) * 12));
      painter.drawLine(tip, tip - QPointF(std::cos(angle + .55) * 12,
                                          std::sin(angle + .55) * 12));
    };
    head(end, control);
    if (type == QStringLiteral("doublearrow"))
      head(start, control);
  } else if (type == QStringLiteral("pen") ||
             type == QStringLiteral("highlighter")) {
    const auto points = item.value(QStringLiteral("points")).toList();
    if (points.isEmpty())
      return;
    if (type == QStringLiteral("highlighter")) {
      color.setAlpha(88);
      painter.setPen(
          QPen(color, 18, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    painter.drawPath(freehandPath(points));
  } else if (type == QStringLiteral("marker")) {
    painter.setBrush(color);
    painter.drawEllipse(start, 13, 13);
    painter.setPen(QColor(Qt::white));
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(14);
    painter.setFont(font);
    painter.drawText(
        QRectF(start.x() - 13, start.y() - 13, 26, 26), Qt::AlignCenter,
        QString::number(item.value(QStringLiteral("number")).toInt()));
  } else if (type == QStringLiteral("text")) {
    QFont font = painter.font();
    font.setPixelSize(22);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const qreal lineHeight = metrics.lineSpacing();
    const qreal baseline =
        (lineHeight - metrics.height()) / 2 + metrics.ascent();
    const auto lines = item.value(QStringLiteral("text"))
                           .toString()
                           .split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (int i = 0; i < lines.size(); ++i)
      painter.drawText(QPointF(start.x(), start.y() - lineHeight / 2 +
                                              i * lineHeight + baseline),
                       lines[i]);
  }
}

QVariantList
CaptureController::smoothedFreehandPoints(const QVariantList &raw) const {
  QVariantList smoothed;
  for (const QPointF &point : filteredFreehandPoints(raw))
    smoothed.append(pointMap(point));
  return smoothed;
}

QPainterPath CaptureController::freehandPath(const QVariantList &raw) {
  QPainterPath path;
  const auto points = filteredFreehandPoints(raw);
  if (points.isEmpty())
    return path;
  path.moveTo(points.first());
  if (points.size() == 1)
    return path;
  if (points.size() == 2) {
    path.lineTo(points.last());
    return path;
  }
  for (int i = 1; i + 1 < points.size(); ++i)
    path.quadTo(points[i], (points[i] + points[i + 1]) / 2);
  path.quadTo(points.last(), points.last());
  return path;
}

QPainterPath CaptureController::mosaicPath(const QVariantMap &item) {
  QPainterPath result;
  if (item.value(QStringLiteral("type")).toString() != QStringLiteral("mosaic"))
    return result;
  result.addRect(QRectF(fromMap(item.value(QStringLiteral("start"))),
                        fromMap(item.value(QStringLiteral("end"))))
                     .normalized());
  return result;
}

void CaptureController::paintScrollPreview(QPainter &painter,
                                           const QRectF &source,
                                           const QRectF &target) const {
  if (m_scrollStitcher.image().isNull() || source.isEmpty() || target.isEmpty())
    return;
  painter.save();
  painter.setClipRect(target);
  painter.translate(target.topLeft());
  painter.scale(target.width() / source.width(),
                target.height() / source.height());
  painter.translate(-source.topLeft());
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(source, m_scrollStitcher.image(), source);
  const auto paintMosaic = [&](const QVariantMap &item) {
    if (item.value(QStringLiteral("type")) != QStringLiteral("mosaic"))
      return;
    const QPainterPath shape = mosaicPath(item);
    if (shape.isEmpty() || !shape.boundingRect().intersects(source) ||
        m_scrollMosaicImage.isNull())
      return;
    painter.save();
    painter.setClipPath(shape, Qt::IntersectClip);
    painter.drawImage(source, m_scrollMosaicImage, source);
    painter.restore();
  };
  for (const QVariant &annotation : m_annotations)
    paintMosaic(annotation.toMap());
  paintMosaic(m_draft);
  QPainterPath holes = spotlightHoles(m_annotations);
  if (m_draft.value(QStringLiteral("type")) ==
      QStringLiteral("spotlight"))
    holes.addEllipse(QRectF(fromMap(m_draft.value(QStringLiteral("start"))),
                            fromMap(m_draft.value(QStringLiteral("end"))))
                         .normalized());
  if (!holes.isEmpty()) {
    QPainterPath outer;
    outer.addRect(source);
    painter.fillPath(outer.subtracted(holes), QColor(0, 0, 0, 150));
  }
  painter.setRenderHint(QPainter::Antialiasing);
  for (const QVariant &annotation : m_annotations) {
    const QVariantMap item = annotation.toMap();
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("mosaic") &&
        type != QStringLiteral("spotlight"))
      paintAnnotation(painter, item);
  }
  if (!m_draft.isEmpty() &&
      m_draft.value(QStringLiteral("type")) != QStringLiteral("mosaic"))
    paintAnnotation(painter, m_draft);
  painter.restore();
}

QImage CaptureController::selectionBaseImage() const {
  if (!m_selected || m_selection.isEmpty())
    return {};
  if (m_scrollState == ScrollState::Reviewing) {
    const QRect source = m_selection.toAlignedRect().intersected(
        QRect(QPoint(0, 0), m_scrollStitcher.image().size()));
    if (source.isEmpty())
      return {};
    return m_scrollStitcher.image().copy(source);
  }
  qreal scale = 1;
  for (const auto &monitor : m_monitors)
    scale = std::max(scale, monitor.image.width() / monitor.geometry.width());
  const QSize size(qMax(1, qRound(m_selection.width() * scale)),
                   qMax(1, qRound(m_selection.height() * scale)));
  QImage result(size, QImage::Format_ARGB32_Premultiplied);
  if (result.isNull())
    return {};
  result.fill(Qt::transparent);
  {
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const auto &monitor : m_monitors) {
      const QRectF shared = m_selection.intersected(monitor.geometry);
      if (shared.isEmpty())
        continue;
      const QRectF source(
          (shared.x() - monitor.geometry.x()) * monitor.image.width() /
              monitor.geometry.width(),
          (shared.y() - monitor.geometry.y()) * monitor.image.height() /
              monitor.geometry.height(),
          shared.width() * monitor.image.width() / monitor.geometry.width(),
          shared.height() * monitor.image.height() / monitor.geometry.height());
      const QRectF target((shared.x() - m_selection.x()) * scale,
                          (shared.y() - m_selection.y()) * scale,
                          shared.width() * scale, shared.height() * scale);
      painter.drawImage(target, monitor.image, source);
    }
  }
  return result;
}

QImage CaptureController::renderedImage() const {
  if (!m_selected || m_selection.isEmpty())
    return {};
  if (m_scrollState == ScrollState::Reviewing) {
    const QRect source = m_selection.toAlignedRect().intersected(
        QRect(QPoint(0, 0), m_scrollStitcher.image().size()));
    if (source.isEmpty())
      return {};
    QImage result(source.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull())
      return {};
    result.fill(Qt::transparent);
    QPainter painter(&result);
    paintScrollPreview(painter, source, QRectF(QPointF(0, 0), source.size()));
    return result;
  }
  QImage result = selectionBaseImage();
  if (result.isNull())
    return {};
  qreal scale = 1;
  for (const auto &monitor : m_monitors)
    scale = std::max(scale, monitor.image.width() / monitor.geometry.width());
  for (const QVariant &annotation : m_annotations) {
    const auto item = annotation.toMap();
    if (item.value(QStringLiteral("type")).toString() !=
        QStringLiteral("mosaic"))
      continue;
    const QPainterPath shape = mosaicPath(item);
    if (shape.isEmpty())
      continue;
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.scale(scale, scale);
    painter.translate(-m_selection.topLeft());
    painter.setClipRect(m_selection);
    painter.setClipPath(shape, Qt::IntersectClip);
    for (const auto &monitor : m_monitors) {
      if (!shape.boundingRect().intersects(monitor.geometry))
        continue;
      painter.drawImage(monitor.geometry, monitor.mosaicImage);
    }
  }
  const QPainterPath holes = spotlightHoles(m_annotations);
  if (!holes.isEmpty()) {
    QPainter painter(&result);
    painter.scale(scale, scale);
    painter.translate(-m_selection.topLeft());
    QPainterPath outer;
    outer.addRect(m_selection);
    painter.fillPath(outer.subtracted(holes), QColor(0, 0, 0, 150));
  }
  for (const QVariant &annotation : m_annotations) {
    const auto item = annotation.toMap();
    if (item.value(QStringLiteral("type")).toString() ==
            QStringLiteral("mosaic") ||
        item.value(QStringLiteral("type")).toString() ==
            QStringLiteral("spotlight"))
      continue;
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(scale, scale);
    painter.translate(-m_selection.topLeft());
    paintAnnotation(painter, item);
  }
  return result;
}

void CaptureController::exportInBackground(std::function<QString()> job) {
  if (m_exporting)
    return;
  m_exporting = true;
  emit exportingChanged();
  m_exportThread = QThread::create([this, job = std::move(job)] {
    const QString error = job();
    QMetaObject::invokeMethod(
        this, [this, error] { finishExport(error); }, Qt::QueuedConnection);
  });
  connect(m_exportThread, &QThread::finished, m_exportThread,
          &QObject::deleteLater);
  m_exportThread->start();
}

void CaptureController::finishExport(const QString &error) {
  m_exporting = false;
  emit exportingChanged();
  if (error.isEmpty())
    emit done();
  else
    setStatus(error);
}

void CaptureController::copy() {
  const auto image = renderedImage();
  if (image.isNull())
    return;
  exportInBackground([image] {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    QString error;
    copyBytes(png, QStringLiteral("image/png"), &error);
    return error;
  });
}

void CaptureController::save() {
  const auto image = renderedImage();
  if (image.isNull())
    return;
  QString directory = qEnvironmentVariable("OMARCHY_SCREENSHOT_DIR");
  if (directory.isEmpty())
    directory =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  if (directory.isEmpty())
    directory = QDir::homePath() + QStringLiteral("/Pictures");
  if (!QDir().mkpath(directory)) {
    setStatus(tr("Cannot create directory: %1").arg(directory));
    return;
  }
  const QString filename = QStringLiteral("screenshot-%1.png")
                               .arg(QDateTime::currentDateTime().toString(
                                   QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz")));
  const QString path = QDir(directory).filePath(filename);
  exportInBackground([image, path] {
    if (!image.save(path, "PNG"))
      return tr("Cannot save: %1").arg(path);
    QTextStream(stdout) << path << '\n';
    return QString();
  });
}

void CaptureController::pin() {
  if (m_exporting)
    return;
  const QImage image = renderedImage();
  if (!image.isNull())
    emit pinRequested(image, m_selection);
}

void CaptureController::finishPin(bool success) {
  if (success)
    emit done();
  else
    setStatus(tr("Cannot pin screenshot."));
}

void CaptureController::startOcr() {
  if (m_ocrRunning || !m_selected || m_selection.isEmpty())
    return;
  const QImage image = selectionBaseImage();
  if (image.isNull()) {
    m_ocrVisible = true;
    m_ocrRunning = false;
    m_ocrPreviewImage = {};
    m_ocrText.clear();
    m_ocrError = tr("OCR is not available.");
    emit ocrChanged();
    return;
  }

  m_ocrVisible = true;
  m_ocrRunning = true;
  m_ocrCopied = false;
  m_ocrPreviewImage = image;
  m_ocrText.clear();
  m_ocrError.clear();
  const int generation = ++m_ocrGeneration;
  emit ocrChanged();

  m_ocrThread = QThread::create([this, image, generation] {
    if (!m_ocr)
      m_ocr = std::make_unique<OcrEngine>();
    const OcrResult result = m_ocr->recognize(image);
    QMetaObject::invokeMethod(
        this, [this, generation, result] { finishOcr(generation, result); },
        Qt::QueuedConnection);
  });
  connect(m_ocrThread, &QThread::finished, m_ocrThread, &QObject::deleteLater);
  m_ocrThread->start();
}

void CaptureController::closeOcr() {
  if (!m_ocrVisible)
    return;
  ++m_ocrGeneration;
  m_ocrVisible = false;
  m_ocrCopied = false;
  m_ocrPreviewImage = {};
  m_ocrText.clear();
  m_ocrError.clear();
  emit ocrChanged();
}

void CaptureController::copyOcrText() {
  if (m_ocrText.isEmpty())
    return;
  QString error;
  if (copyBytes(m_ocrText.toUtf8(),
                QStringLiteral("text/plain;charset=utf-8"), &error)) {
    m_ocrCopied = true;
    m_ocrError.clear();
  } else {
    m_ocrError = error;
  }
  emit ocrChanged();
}

void CaptureController::finishOcr(int generation, const OcrResult &result) {
  m_ocrRunning = false;
  if (generation != m_ocrGeneration) {
    emit ocrChanged();
    return;
  }
  if (result.error != OcrError::None) {
    qWarning().noquote() << "OCR:" << result.detail;
    m_ocrError = tr("OCR is not available.");
    emit ocrChanged();
    return;
  }
  if (result.lines.isEmpty()) {
    m_ocrError = tr("No text found.");
    emit ocrChanged();
    return;
  }

  QStringList lines;
  lines.reserve(result.lines.size());
  for (const OcrLine &line : result.lines)
    lines.append(line.text);
  m_ocrText = lines.join(QLatin1Char('\n'));
  m_ocrCopied = false;
  emit ocrChanged();
}

void CaptureController::cancel() { emit done(); }
