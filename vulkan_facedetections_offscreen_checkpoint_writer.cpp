#include "vulkan_facedetections_offscreen_checkpoint_writer.h"

#include "vulkan_facedetections_offscreen_artifact_io.h"

#include <QElapsedTimer>
#include <QFile>

#include <algorithm>

AsyncFaceStreamWriter::AsyncFaceStreamWriter(int capacity)
    : m_capacity(std::max(1, capacity)) {}

AsyncFaceStreamWriter::~AsyncFaceStreamWriter() {
  QString ignored;
  close(&ignored);
}

bool AsyncFaceStreamWriter::open(const QString &path, QString *error) {
  if (path.isEmpty()) {
    if (error)
      *error = QStringLiteral("Async checkpoint writer path is empty.");
    return false;
  }
  QFile probe(path);
  if (!probe.open(QIODevice::WriteOnly | QIODevice::Append)) {
    if (error) {
      *error = QStringLiteral(
                   "Failed to open streaming facedetections checkpoint: %1")
                   .arg(path);
    }
    return false;
  }
  probe.close();
  m_path = path;
  m_thread = std::thread([this]() { run(); });
  return true;
}

bool AsyncFaceStreamWriter::enqueueMeta(const FaceStreamMetaRecord &record,
                                        QString *error) {
  return enqueueRecord(record, error);
}

bool AsyncFaceStreamWriter::enqueueFrame(const FaceStreamFrameRecord &record,
                                         QString *error) {
  return enqueueRecord(record, error);
}

bool AsyncFaceStreamWriter::enqueueRecord(Record record, QString *error) {
  std::unique_lock<std::mutex> lock(m_mutex);
  if (m_stopping || m_failed) {
    if (error) {
      *error = m_error.isEmpty()
                   ? QStringLiteral(
                         "Async checkpoint writer is not accepting records.")
                   : m_error;
    }
    return false;
  }
  QElapsedTimer waitTimer;
  bool waited = false;
  while (!m_failed && !m_stopping &&
         static_cast<int>(m_queue.size()) >= m_capacity) {
    if (!waited) {
      waited = true;
      waitTimer.start();
      ++m_backpressureWaits;
    }
    m_notFull.wait(lock);
  }
  if (waited) {
    m_backpressureMs +=
        static_cast<double>(waitTimer.nsecsElapsed()) / 1'000'000.0;
  }
  if (m_stopping || m_failed) {
    if (error) {
      *error = m_error.isEmpty()
                   ? QStringLiteral("Async checkpoint writer stopped before "
                                    "enqueue completed.")
                   : m_error;
    }
    return false;
  }
  m_queue.push_back(std::move(record));
  ++m_recordsQueued;
  m_maxBacklog = std::max(m_maxBacklog, static_cast<int>(m_queue.size()));
  m_notEmpty.notify_one();
  return true;
}

bool AsyncFaceStreamWriter::close(QString *error) {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed) {
      if (error && !m_error.isEmpty()) {
        *error = m_error;
      }
      return !m_failed;
    }
    m_stopping = true;
  }
  m_notEmpty.notify_all();
  m_notFull.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_closed = true;
    if (error && !m_error.isEmpty()) {
      *error = m_error;
    }
    return !m_failed;
  }
}

int AsyncFaceStreamWriter::backlog() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_queue.size());
}

int AsyncFaceStreamWriter::maxBacklog() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_maxBacklog;
}

int AsyncFaceStreamWriter::recordsQueued() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_recordsQueued;
}

int AsyncFaceStreamWriter::recordsWritten() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_recordsWritten;
}

int AsyncFaceStreamWriter::backpressureWaits() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_backpressureWaits;
}

double AsyncFaceStreamWriter::backpressureMs() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_backpressureMs;
}

double AsyncFaceStreamWriter::writeMsTotal() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_writeMsTotal;
}

void AsyncFaceStreamWriter::failLocked(const QString &error) {
  m_failed = true;
  if (m_error.isEmpty()) {
    m_error = error;
  }
}

void AsyncFaceStreamWriter::run() {
  QFile file(m_path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      failLocked(QStringLiteral("Async checkpoint writer failed to open %1")
                     .arg(m_path));
    }
    m_notFull.notify_all();
    return;
  }
  while (true) {
    Record record;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_notEmpty.wait(
          lock, [&]() { return m_stopping || m_failed || !m_queue.empty(); });
      if ((m_stopping || m_failed) && m_queue.empty()) {
        break;
      }
      record = std::move(m_queue.front());
      m_queue.pop_front();
      m_notFull.notify_one();
    }
    QElapsedTimer timer;
    timer.start();
    const bool ok =
        std::holds_alternative<FaceStreamMetaRecord>(record)
            ? appendFaceStreamMetaRecord(
                  &file, std::get<FaceStreamMetaRecord>(record))
            : appendFaceStreamFrameRecord(
                  &file, std::get<FaceStreamFrameRecord>(record));
    const double writeMs =
        static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_writeMsTotal += writeMs;
      if (ok) {
        ++m_recordsWritten;
      } else {
        failLocked(
            QStringLiteral("Async checkpoint writer failed while appending %1")
                .arg(m_path));
      }
    }
    if (!ok) {
      m_notFull.notify_all();
      break;
    }
  }
  file.flush();
}
