// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPainterPath>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <memory>

#include "scrollstitcher.h"

#include <functional>
#include <future>

class QScreen;
class QPainter;
class VirtualPointer;
class QThread;
class OcrEngine;
struct OcrResult;

struct CaptureMonitor {
  QString name;
  QRectF geometry;
  QImage image;
  // One pixel per mosaic block; painting it unsmoothed at full size pixelates.
  QImage mosaicImage;
  QScreen *screen = nullptr;
  int id = -1;
  int workspace = -1;
};

class CaptureController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QRectF selection READ selection NOTIFY selectionChanged)
  Q_PROPERTY(QRectF hovered READ hovered NOTIFY hoveredChanged)
  Q_PROPERTY(bool selected READ selected NOTIFY selectedChanged)
  Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
  Q_PROPERTY(QVariantMap toolVariants READ toolVariants NOTIFY toolVariantsChanged)
  Q_PROPERTY(QString annotationColor READ annotationColor WRITE
                 setAnnotationColor NOTIFY annotationColorChanged)
  // Screen pixels per image pixel in the long-image review, so selection
  // hit areas stay a constant size on screen at every zoom level.
  Q_PROPERTY(qreal reviewScale MEMBER m_reviewScale NOTIFY reviewScaleChanged)
  Q_PROPERTY(bool darkToolbar READ darkToolbar WRITE setDarkToolbar NOTIFY
                 darkToolbarChanged)
  Q_PROPERTY(
      QVariantList annotations READ annotations NOTIFY annotationsChanged)
  Q_PROPERTY(QVariantMap draft READ draft NOTIFY draftChanged)
  Q_PROPERTY(int toolbarScreen READ toolbarScreen NOTIFY selectionChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(int scrollState READ scrollState NOTIFY scrollStateChanged)
  Q_PROPERTY(QVariantList scrollCandidates READ scrollCandidates NOTIFY scrollStateChanged)
  Q_PROPERTY(int scrollRevision READ scrollRevision NOTIFY scrollImageChanged)
  Q_PROPERTY(int scrollHeight READ scrollHeight NOTIFY scrollImageChanged)
  Q_PROPERTY(int scrollWidth READ scrollWidth NOTIFY scrollImageChanged)
  Q_PROPERTY(bool scrollAwaitingPane READ scrollAwaitingPane NOTIFY scrollAwaitingPaneChanged)
  Q_PROPERTY(bool scrollPaneSelected READ scrollPaneSelected NOTIFY scrollAwaitingPaneChanged)
  Q_PROPERTY(bool scrollStopping READ scrollStopping NOTIFY scrollStoppingChanged)
  Q_PROPERTY(QRectF scrollRegion READ scrollRegion NOTIFY scrollStateChanged)
  Q_PROPERTY(bool imagesReady READ imagesReady NOTIFY imagesReadyChanged)
  // Bumped per capture so image URLs change and QML never shows a stale one.
  Q_PROPERTY(int captureGeneration READ captureGeneration NOTIFY imagesReadyChanged)
  Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
  Q_PROPERTY(bool ocrVisible READ ocrVisible NOTIFY ocrChanged)
  Q_PROPERTY(bool ocrRunning READ ocrRunning NOTIFY ocrChanged)
  Q_PROPERTY(QString ocrText READ ocrText NOTIFY ocrChanged)
  Q_PROPERTY(QString ocrError READ ocrError NOTIFY ocrChanged)
  Q_PROPERTY(bool ocrCopied READ ocrCopied NOTIFY ocrChanged)

public:
  explicit CaptureController(QObject *parent = nullptr);
  ~CaptureController() override;

  bool initialize(QString *error);
  // Split form of initialize(): the capture runs on a worker thread between
  // the two calls so the caller can load the UI meanwhile.
  bool startCapture(QString *error);
  bool finishCapture(QString *error);
  // Drops the previous capture and its edits so a resident process can
  // start the next one from a clean slate.
  void reset();
  const QVector<CaptureMonitor> &monitors() const { return m_monitors; }
  QRectF selection() const { return m_selection; }
  QRectF hovered() const { return m_hovered; }
  bool selected() const { return m_selected; }
  QString tool() const { return m_tool; }
  QVariantMap toolVariants() const { return m_toolVariants; }
  QString annotationColor() const { return m_annotationColor; }
  bool darkToolbar() const { return m_darkToolbar; }
  QVariantList annotations() const { return m_annotations; }
  QVariantMap draft() const { return m_draft; }
  int toolbarScreen() const;
  QString status() const { return m_status; }
  enum class ScrollState { Idle, Choosing, Capturing, Reviewing };
  int scrollState() const { return int(m_scrollState); }
  QVariantList scrollCandidates() const;
  int scrollRevision() const { return m_scrollRevision; }
  int scrollHeight() const { return m_scrollStitcher.image().height(); }
  int scrollWidth() const { return m_scrollStitcher.image().width(); }
  bool scrollAwaitingPane() const { return m_scrollAwaitingPane; }
  bool scrollPaneSelected() const { return m_scrollPaneSelected; }
  bool scrollStopping() const { return m_scrollPauseRequested; }
  const QImage &scrollImage() const { return m_scrollStitcher.image(); }
  const QImage &scrollMosaicImage() const { return m_scrollMosaicImage; }
  QRectF scrollRegion() const { return m_scrollRegion; }
  bool imagesReady() const { return m_imagesReady; }
  int captureGeneration() const { return m_captureGeneration; }
  bool exporting() const { return m_exporting; }
  bool ocrVisible() const { return m_ocrVisible; }
  bool ocrRunning() const { return m_ocrRunning; }
  QString ocrText() const { return m_ocrText; }
  QString ocrError() const { return m_ocrError; }
  bool ocrCopied() const { return m_ocrCopied; }
  QImage ocrPreview() const { return m_ocrPreviewImage; }
  QImage renderedImage() const;
  void paintScrollPreview(QPainter &painter, const QRectF &source,
                          const QRectF &target) const;
  static QPainterPath freehandPath(const QVariantList &points);
  static QPainterPath mosaicPath(const QVariantMap &item);

  Q_INVOKABLE void pointerMove(int screenIndex, qreal x, qreal y);
  Q_INVOKABLE void pointerPress(int screenIndex, qreal x, qreal y);
  Q_INVOKABLE void pointerRelease(int screenIndex, qreal x, qreal y);
  Q_INVOKABLE void addText(qreal x, qreal y, const QString &text);
  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();
  Q_INVOKABLE void copy();
  Q_INVOKABLE void save();
  Q_INVOKABLE void pin();
  void finishPin(bool success);
  Q_INVOKABLE void startOcr();
  Q_INVOKABLE void closeOcr();
  Q_INVOKABLE void copyOcrText();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void setTool(const QString &tool);
  Q_INVOKABLE void restoreToolBeforeText();
  Q_INVOKABLE void activateToolGroup(const QString &group);
  Q_INVOKABLE void cancelPointerAction();
  Q_INVOKABLE void setAnnotationColor(const QString &color);
  Q_INVOKABLE void setAnnotationColorFromHsv(qreal hue, qreal saturation,
                                             qreal value);
  Q_INVOKABLE void saveAnnotationColor();
  Q_INVOKABLE void setDarkToolbar(bool dark);
  Q_INVOKABLE void adjustSelectionEdge(int key, bool shrink);
  Q_INVOKABLE QVariantList
  smoothedFreehandPoints(const QVariantList &points) const;
  Q_INVOKABLE void startScroll();
  Q_INVOKABLE void chooseScrollWindow(int index);
  Q_INVOKABLE void pauseScroll();
  Q_INVOKABLE void resumeScroll();
  Q_INVOKABLE void cancelScrollChoice();
  Q_INVOKABLE void continueAfterPane();
  Q_INVOKABLE void selectScrollPane(qreal x, qreal y);
  void restoreScrollInputFocus();

signals:
  void selectionChanged();
  void hoveredChanged();
  void selectedChanged();
  void toolChanged();
  void toolVariantsChanged();
  void annotationColorChanged();
  void darkToolbarChanged();
  void reviewScaleChanged();
  void annotationsChanged();
  void draftChanged();
  void statusChanged();
  void imagesReadyChanged();
  void exportingChanged();
  void ocrChanged();
  void done();
  void pinRequested(const QImage &image, const QRectF &rect);
  void scrollStateChanged();
  void scrollImageChanged();
  void scrollFrameAboutToCapture();
  void scrollFrameCaptured();
  void scrollInputAboutToSend();
  void scrollInputSent();
  void scrollAwaitingPaneChanged();
  void scrollStoppingChanged();

private:
  struct Candidate {
    QRectF geometry;
    bool window = false;
    QString address;
    QString title;
    int monitorIndex = -1;
  };
  enum class Drag { None, Select, Move, Resize, Draw };
  struct CaptureResult {
    QList<QImage> images;
    QList<QImage> mosaics;
    QString error;
  };

  static QVariantMap defaultToolVariants();
  QPointF globalPoint(int screenIndex, qreal x, qreal y) const;
  QRectF candidateAt(const QPointF &point) const;
  void setSelection(const QRectF &rect);
  void setHovered(const QRectF &rect);
  void setStatus(const QString &message);
  void paintAnnotation(QPainter &painter, const QVariantMap &item) const;
  void appendAnnotation(const QVariantMap &item);
  void setScrollState(ScrollState state);
  void captureScrollFrame();
  void finishScrollFrame(const QImage &frame, const QString &error);
  void prepareScrollFrame();
  void prepareScrollStep();
  void sendScrollStep();
  void finishScrollCapture();
  bool scrollWindowUnchanged();
  bool focusScrollWindow();
  void prepareScrollMosaic();
  // Runs job off the GUI thread; job returns an error message or nothing.
  void exportInBackground(std::function<QString()> job);
  void finishExport(const QString &error);
  QImage selectionBaseImage() const;
  void finishOcr(int generation, const OcrResult &result);
  void addWindowCandidates(const QByteArray &json);
  static CaptureResult captureMonitors(const QStringList &names,
                                       const QList<bool> &direct);

  QVector<CaptureMonitor> m_monitors;
  QVector<Candidate> m_candidates;
  QRectF m_selection;
  QRectF m_hovered;
  QRectF m_initialSelection;
  QPointF m_press;
  Drag m_drag = Drag::None;
  bool m_selected = false;
  bool m_moved = false;
  int m_resizeEdges = 0;
  QString m_tool = QStringLiteral("select");
  QString m_toolBeforeText = QStringLiteral("select");
  QVariantMap m_toolVariants = defaultToolVariants();
  QString m_annotationColor = QStringLiteral("#ff4b55");
  bool m_darkToolbar = false;
  qreal m_reviewScale = 1;
  QTimer m_colorSaveTimer;
  bool m_colorDirty = false;
  QVariantList m_annotations;
  QVariantList m_redoAnnotations;
  QVariantList m_initialAnnotations;
  QVariantMap m_draft;
  QString m_status;
  ScrollState m_scrollState = ScrollState::Idle;
  ScrollStitcher m_scrollStitcher;
  QImage m_scrollMosaicImage;
  QRectF m_scrollRegion;
  QPointF m_scrollPoint;
  std::unique_ptr<VirtualPointer> m_virtualPointer;
  QString m_scrollWindowAddress;
  QRectF m_scrollWindowGeometry;
  QTimer m_scrollTimer;
  QElapsedTimer m_scrollInputTimer;
  bool m_scrollPauseRequested = false;
  bool m_scrollResumeCheck = false;
  bool m_scrollNeedsPane = false;
  bool m_scrollAwaitingPane = false;
  bool m_scrollPaneSelected = false;
  bool m_scrollInputSent = false;
  bool m_scrollReviewInitialized = false;
  int m_scrollUnchangedFrames = 0;
  int m_scrollNoMatchRetries = 0;
  int m_scrollSteps = 1;
  int m_scrollReviewedHeight = 0;
  int m_scrollRevision = 0;
  int m_scrollGeneration = 0;
  std::future<CaptureResult> m_capture;
  std::future<QByteArray> m_clients;
  bool m_imagesReady = false;
  int m_captureGeneration = 0;
  bool m_exporting = false;
  QPointer<QThread> m_exportThread;
  QPointer<QThread> m_scrollCaptureThread;
  std::unique_ptr<OcrEngine> m_ocr;
  QPointer<QThread> m_ocrThread;
  bool m_ocrVisible = false;
  bool m_ocrRunning = false;
  bool m_ocrCopied = false;
  QImage m_ocrPreviewImage;
  QString m_ocrText;
  QString m_ocrError;
  int m_ocrGeneration = 0;
};
