// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QCoreApplication>
#include <QProcess>
#include <QString>

// Keep compositor-rendered cursors out of the source framebuffer as well as
// requesting a cursor-free capture. Owns the temporary setting for all outputs.
class CursorCaptureGuard {
  Q_DECLARE_TR_FUNCTIONS(CursorCaptureGuard)

public:
  explicit CursorCaptureGuard(QString *error);
  ~CursorCaptureGuard();
  CursorCaptureGuard(const CursorCaptureGuard &) = delete;
  CursorCaptureGuard &operator=(const CursorCaptureGuard &) = delete;

  bool ready() const { return m_ready; }

private:
  int m_lock = -1;
  QProcess m_restore;
  bool m_restoreArmed = false;
  bool m_ready = false;
};
