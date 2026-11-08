// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QImage>
#include <QList>
#include <QRectF>
#include <QString>

#include <memory>

struct OcrLine {
  QString text;
  float confidence = 0;
  QRectF box;
};

enum class OcrError {
  None,
  ModelsMissing,
  LoadFailed,
  InferenceFailed,
  EmptyImage,
};

struct OcrResult {
  QList<OcrLine> lines;
  OcrError error = OcrError::None;
  QString detail;
};

// Keeps RapidOCRLite out of the rest of the application. One instance is used
// by one worker at a time; the model stays loaded between captures.
class OcrEngine {
public:
  OcrEngine();
  ~OcrEngine();
  OcrEngine(const OcrEngine &) = delete;
  OcrEngine &operator=(const OcrEngine &) = delete;

  OcrResult recognize(const QImage &image);
  static QString modelDirectory();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
