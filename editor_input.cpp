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
    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
        if ((modifiers & Qt::ControlModifier) &&
            !(modifiers & Qt::AltModifier) &&
            !(modifiers & Qt::MetaModifier)) {
            bool belongsToEditor = false;
            if (auto *widget = qobject_cast<QWidget *>(watched)) {
                belongsToEditor = widget->window() == this;
            }
            if (belongsToEditor) {
                const int key = keyEvent->key();
                if (!shouldBlockGlobalEditorShortcuts() && m_timeline && !keyEvent->isAutoRepeat()) {
                    bool changed = false;
                    if (key == Qt::Key_C) {
                        return m_timeline->copySelectedClips();
                    }
                    if (key == Qt::Key_X) {
                        changed = m_timeline->cutSelectedClips();
                    } else if (key == Qt::Key_V) {
                        changed = m_timeline->pasteClipsAtCurrentFrame();
                    } else if (key == Qt::Key_D) {
                        changed = m_timeline->duplicateSelectedClips();
                    } else if (key == Qt::Key_A) {
                        return m_timeline->selectAllClips();
                    } else if (key == Qt::Key_Y) {
                        redoHistory();
                        return true;
                    }
                    if (changed) {
                        refreshTimelineStructureInspectorViews();
                        return true;
                    }
                }
                if (key == Qt::Key_Equal || key == Qt::Key_Plus) {
                    adjustGlobalFontSize(+1);
                    return true;
                }
                if (key == Qt::Key_Minus || key == Qt::Key_Underscore) {
                    adjustGlobalFontSize(-1);
                    return true;
                }
            }
        }
    }

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
