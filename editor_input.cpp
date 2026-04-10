#include "editor.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTableWidget>

using namespace editor;

bool EditorWindow::handleTranscriptTableDelete(QObject *watched, QEvent *event)
{
    if (!m_transcriptTable ||
        (watched != m_transcriptTable && watched != m_transcriptTable->viewport()) ||
        event->type() != QEvent::KeyPress) {
        return false;
    }

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->key() == Qt::Key_Delete && !(keyEvent->modifiers() & Qt::KeyboardModifierMask)) {
        m_transcriptTab->deleteSelectedRows();
        return true;
    }
    return false;
}

bool EditorWindow::handleVideoKeyframeTableDelete(QObject *watched, QEvent *event)
{
    if (!m_videoKeyframeTable ||
        (watched != m_videoKeyframeTable && watched != m_videoKeyframeTable->viewport()) ||
        event->type() != QEvent::KeyPress) {
        return false;
    }

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->key() == Qt::Key_Delete && !(keyEvent->modifiers() & Qt::KeyboardModifierMask)) {
        m_videoKeyframeTab->removeSelectedKeyframes();
        return true;
    }
    return false;
}

bool EditorWindow::handleGradingKeyframeTableDelete(QObject *watched, QEvent *event)
{
    if (!m_gradingKeyframeTable ||
        (watched != m_gradingKeyframeTable && watched != m_gradingKeyframeTable->viewport()) ||
        event->type() != QEvent::KeyPress) {
        return false;
    }

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->key() == Qt::Key_Delete && !(keyEvent->modifiers() & Qt::KeyboardModifierMask)) {
        m_gradingTab->removeSelectedKeyframes();
        return true;
    }
    return false;
}

bool EditorWindow::handleOpacityKeyframeTableDelete(QObject *watched, QEvent *event)
{
    if (!m_opacityKeyframeTable ||
        (watched != m_opacityKeyframeTable && watched != m_opacityKeyframeTable->viewport()) ||
        event->type() != QEvent::KeyPress) {
        return false;
    }

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->key() == Qt::Key_Delete && !(keyEvent->modifiers() & Qt::KeyboardModifierMask)) {
        m_opacityTab->removeSelectedKeyframes();
        return true;
    }
    return false;
}

bool EditorWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_timecodeLabel && watched == m_timecodeLabel && event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            const int64_t currentFrame = m_timeline ? m_timeline->currentFrame() : 0;
            QApplication::clipboard()->setText(QString::number(currentFrame));
            return true;
        }
    }
    if (handleTranscriptTableDelete(watched, event)) return true;
    if (handleVideoKeyframeTableDelete(watched, event)) return true;
    if (handleGradingKeyframeTableDelete(watched, event)) return true;
    if (handleOpacityKeyframeTableDelete(watched, event)) return true;
    return QMainWindow::eventFilter(watched, event);
}
