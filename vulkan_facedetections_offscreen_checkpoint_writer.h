#pragma once

#include "vulkan_facedetections_offscreen_artifact_io.h"

#include <QString>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

class AsyncFaceStreamWriter {
public:
  explicit AsyncFaceStreamWriter(int capacity);
  ~AsyncFaceStreamWriter();

  bool open(const QString &path, QString *error);
  bool enqueueMeta(const FaceStreamMetaRecord &record, QString *error);
  bool enqueueFrame(const FaceStreamFrameRecord &record, QString *error);
  bool close(QString *error);

  int backlog() const;
  int maxBacklog() const;
  int recordsQueued() const;
  int recordsWritten() const;
  int backpressureWaits() const;
  double backpressureMs() const;
  double writeMsTotal() const;

private:
  void failLocked(const QString &error);
  void run();
  using Record = std::variant<FaceStreamMetaRecord, FaceStreamFrameRecord>;
  bool enqueueRecord(Record record, QString *error);

  QString m_path;
  const int m_capacity = 1;
  mutable std::mutex m_mutex;
  std::condition_variable m_notEmpty;
  std::condition_variable m_notFull;
  std::deque<Record> m_queue;
  std::thread m_thread;
  bool m_stopping = false;
  bool m_failed = false;
  bool m_closed = false;
  QString m_error;
  int m_maxBacklog = 0;
  int m_recordsQueued = 0;
  int m_recordsWritten = 0;
  int m_backpressureWaits = 0;
  double m_backpressureMs = 0.0;
  double m_writeMsTotal = 0.0;
};
