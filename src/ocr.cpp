// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

#include "ocr.h"

#include "rapidocrlite/rapidocrlite.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QtGlobal>

#include <cstring>
#include <exception>
#include <vector>

namespace {
constexpr auto detModel = "PP-OCRv6_det_tiny.onnx";
constexpr auto recModel = "PP-OCRv6_rec_tiny.onnx";
constexpr auto dictionary = "ppocrv6_tiny_dict.txt";

bool hasModels(const QString &directory) {
  const QDir dir(directory);
  return QFileInfo::exists(dir.filePath(detModel)) &&
         QFileInfo::exists(dir.filePath(recModel)) &&
         QFileInfo::exists(dir.filePath(dictionary));
}
} // namespace

struct OcrEngine::Impl {
  std::unique_ptr<rpocr::OCR> ocr;
};

OcrEngine::OcrEngine() : m_impl(std::make_unique<Impl>()) {}

OcrEngine::~OcrEngine() = default;

QString OcrEngine::modelDirectory() {
  QStringList candidates;
  const QString fromEnvironment =
      qEnvironmentVariable("OMARCHY_SCREENSHOT_OCR_MODELS");
  if (!fromEnvironment.isEmpty())
    candidates.append(fromEnvironment);

  const QString located = QStandardPaths::locate(
      QStandardPaths::GenericDataLocation,
      QStringLiteral("omarchy-screenshot/ocr/") + detModel);
  if (!located.isEmpty())
    candidates.append(QFileInfo(located).absolutePath());

  candidates.append(QCoreApplication::applicationDirPath() +
                    QStringLiteral("/../share/omarchy-screenshot/ocr"));
#ifdef OMARCHY_SCREENSHOT_OCR_MODEL_DIR
  candidates.append(QStringLiteral(OMARCHY_SCREENSHOT_OCR_MODEL_DIR));
#endif

  for (const QString &candidate : candidates) {
    if (hasModels(candidate))
      return QDir(candidate).absolutePath();
  }
  return {};
}

OcrResult OcrEngine::recognize(const QImage &image) {
  OcrResult result;
  if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
    result.error = OcrError::EmptyImage;
    result.detail = QStringLiteral("empty image");
    return result;
  }

  if (!m_impl->ocr) {
    const QString directory = modelDirectory();
    if (directory.isEmpty()) {
      result.error = OcrError::ModelsMissing;
      result.detail = QStringLiteral(
          "PP-OCRv6 Tiny models are missing; set OMARCHY_SCREENSHOT_OCR_MODELS");
      return result;
    }
    try {
      rpocr::Options options;
      options.backend = rpocr::Backend::cpu_only;
      options.use_cls = 0;
      options.rec_parallelism =
          qBound(1, QThread::idealThreadCount() / 2, 8);
      const QDir models(directory);
      m_impl->ocr = std::make_unique<rpocr::OCR>(
          models.filePath(QString::fromLatin1(detModel)).toStdString(),
          models.filePath(QString::fromLatin1(recModel)).toStdString(),
          models.filePath(QString::fromLatin1(dictionary)).toStdString(),
          options);
    } catch (const std::exception &error) {
      result.error = OcrError::LoadFailed;
      result.detail = QString::fromUtf8(error.what());
      return result;
    }
  }

  const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
  if (rgb.isNull()) {
    result.error = OcrError::EmptyImage;
    result.detail = QStringLiteral("cannot convert the image to RGB");
    return result;
  }
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(rgb.width()) * rgb.height() * 3);
  for (int y = 0; y < rgb.height(); ++y) {
    std::memcpy(pixels.data() + static_cast<std::size_t>(y) * rgb.width() * 3,
                rgb.constScanLine(y),
                static_cast<std::size_t>(rgb.width()) * 3);
  }

  try {
    rpocr::Image input{rgb.width(), rgb.height(), std::move(pixels)};
    const std::vector<rpocr::Result> lines = m_impl->ocr->Recognize(input);
    result.lines.reserve(static_cast<int>(lines.size()));
    for (const rpocr::Result &line : lines) {
      OcrLine item;
      item.text = QString::fromStdString(line.text);
      item.confidence = line.confidence;
      item.box = QRectF(line.bbox[0], line.bbox[1], line.bbox[2],
                        line.bbox[3]);
      if (!item.text.isEmpty())
        result.lines.append(item);
    }
  } catch (const std::exception &error) {
    result.error = OcrError::InferenceFailed;
    result.detail = QString::fromUtf8(error.what());
  }
  return result;
}
