// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

#include "cursorcaptureguard.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
QByteArray hyprctl(const QStringList &arguments) {
  QProcess process;
  process.start(QStringLiteral("hyprctl"), arguments);
  if (!process.waitForStarted(1000) || !process.waitForFinished(3000) ||
      process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {};
  return process.readAllStandardOutput().trimmed();
}
} // namespace

CursorCaptureGuard::CursorCaptureGuard(QString *error) {
  const auto fail = [error](const QString &detail) {
    if (error)
      *error = tr("Cannot exclude the cursor: %1").arg(detail);
  };
  const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (runtime.isEmpty()) {
    fail(tr("XDG_RUNTIME_DIR is not set"));
    return;
  }
  // A daemon and a standalone invocation must not restore the cursor while
  // the other is still copying pixels. The kernel releases this lock on exit.
  const QByteArray instance =
      QCryptographicHash::hash(qgetenv("HYPRLAND_INSTANCE_SIGNATURE"),
                               QCryptographicHash::Sha256)
          .toHex();
  const QByteArray path = QFile::encodeName(
      QDir(runtime).filePath(QStringLiteral("omarchy-screenshot-cursor-%1.lock")
                                 .arg(QString::fromLatin1(instance.left(16)))));
  m_lock =
      open(path.constData(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (m_lock < 0) {
    fail(tr("cannot open the capture lock"));
    return;
  }
  int locked;
  do {
    locked = flock(m_lock, LOCK_EX);
  } while (locked < 0 && errno == EINTR);
  if (locked < 0) {
    fail(tr("cannot lock the cursor state"));
    return;
  }

  const QJsonObject option =
      QJsonDocument::fromJson(
          hyprctl({QStringLiteral("-j"), QStringLiteral("getoption"),
                   QStringLiteral("cursor:invisible")}))
          .object();
  const auto boolean = option.value(QStringLiteral("bool"));
  const auto integer = option.value(QStringLiteral("int"));
  if (!boolean.isBool() && !integer.isDouble()) {
    fail(tr("cannot read cursor:invisible"));
    return;
  }
  const bool invisible =
      boolean.isBool() ? boolean.toBool() : integer.toInt() != 0;
  if (!invisible) {
    const QString lua =
        QStringLiteral("hl.config({ cursor = { invisible = %1 } })");
    const QStringList enableLua{QStringLiteral("eval"),
                                lua.arg(QStringLiteral("true"))};
    // Start restoration before changing the compositor. The child watches stdin
    // EOF, so even SIGKILL of the screenshot process restores the old value.
    // It has its own session to survive Ctrl+C sent to the parent's group.
    m_restore.setChildProcessModifier([lock = m_lock] {
      setsid();
      // Also keep the capture lock if the parent dies before restoration.
      fcntl(lock, F_SETFD, 0);
    });
    m_restore.start(
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"),
         QStringLiteral("while IFS= read -r line; do :; done; "
                        "result=$(hyprctl eval 'hl.config({ cursor = { "
                        "invisible = false } })'); "
                        "case \"$result\" in ok) exit 0;; esac; "
                        "result=$(hyprctl keyword cursor:invisible 0); "
                        "[ \"$result\" = ok ]"),
         QStringLiteral("cursor-restore")});
    if (!m_restore.waitForStarted(1000)) {
      fail(tr("cannot start cursor restoration"));
      return;
    }
    m_restoreArmed = true;
    if (hyprctl(enableLua) != "ok" &&
        hyprctl({QStringLiteral("keyword"), QStringLiteral("cursor:invisible"),
                 QStringLiteral("1")}) != "ok") {
      fail(tr("cannot suppress compositor cursor rendering"));
      return;
    }
  }
  // Hyprland applies cursor:invisible on its 500 ms cursor timer. An IPC
  // reply confirms the setting, not the rendered cursor state. Wait a full
  // timer interval before requesting a fresh frame; shape updates during the
  // capture can then no longer paint a software cursor into the framebuffer.
  QThread::msleep(550);
  m_ready = true;
}

CursorCaptureGuard::~CursorCaptureGuard() {
  if (m_restoreArmed) {
    if (m_restore.state() != QProcess::NotRunning) {
      m_restore.closeWriteChannel();
      if (!m_restore.waitForFinished(3000)) {
        m_restore.kill();
        m_restore.waitForFinished(1000);
      }
    }
    bool restored = m_restore.exitStatus() == QProcess::NormalExit &&
                    m_restore.exitCode() == 0;
    for (int retry = 0; !restored && retry < 2; ++retry)
      restored =
          hyprctl({QStringLiteral("eval"),
                   QStringLiteral(
                       "hl.config({ cursor = { invisible = false } })")}) ==
              "ok" ||
          hyprctl({QStringLiteral("keyword"),
                   QStringLiteral("cursor:invisible"), QStringLiteral("0")}) ==
              "ok";
    if (!restored)
      qWarning() << "Cannot restore cursor:invisible";
  }
  if (m_lock >= 0)
    close(m_lock);
}
