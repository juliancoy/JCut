#include "editor.h"

#include <QEvent>
#include <QKeyEvent>
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

bool EditorWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (handleTranscriptTableDelete(watched, event)) return true;
    if (handleVideoKeyframeTableDelete(watched, event)) return true;
    if (handleGradingKeyframeTableDelete(watched, event)) return true;
    return QMainWindow::eventFilter(watched, event);
}
