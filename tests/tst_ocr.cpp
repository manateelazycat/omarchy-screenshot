// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

#include "ocr.h"

#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QTest>

class OcrTest : public QObject {
  Q_OBJECT

private slots:
  void recognizesRenderedText() {
    QImage image(720, 220, QImage::Format_RGB32);
    image.fill(Qt::white);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::black);
    QFont font;
    const QStringList families = QFontDatabase::families();
    for (const QString &family :
         {QStringLiteral("Noto Sans CJK SC"),
          QStringLiteral("Noto Sans CJK TC"),
          QStringLiteral("Noto Sans CJK JP")}) {
      if (families.contains(family)) {
        font.setFamily(family);
        break;
      }
    }
    font.setPixelSize(34);
    painter.setFont(font);
    painter.drawText(24, 70, QStringLiteral("Hello RapidOCR 123"));
    painter.drawText(24, 130, QStringLiteral("截图文字识别测试"));
    painter.end();

    OcrEngine engine;
    const OcrResult result = engine.recognize(image);
    QVERIFY2(result.error == OcrError::None, qPrintable(result.detail));

    QString text;
    for (const OcrLine &line : result.lines)
      text += line.text + QLatin1Char('\n');
    QVERIFY2(text.contains(QStringLiteral("Hello")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("123")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("截图")), qPrintable(text));
  }
};

QTEST_MAIN(OcrTest)
#include "tst_ocr.moc"
