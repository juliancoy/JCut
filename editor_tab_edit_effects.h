#pragma once

#include <functional>

struct TabEditEffects {
    bool updatePreview = true;
    bool refreshInspector = true;
    bool scheduleSave = true;
    bool pushHistory = true;
};

struct TabEditCallbacks {
    std::function<void()> updatePreview;
    std::function<void()> refreshInspector;
    std::function<void()> scheduleSave;
    std::function<void()> pushHistory;
};

inline void applyTabEditEffects(const TabEditCallbacks& callbacks,
                                const TabEditEffects& effects = TabEditEffects()) {
    if (effects.updatePreview && callbacks.updatePreview) {
        callbacks.updatePreview();
    }
    if (effects.refreshInspector && callbacks.refreshInspector) {
        callbacks.refreshInspector();
    }
    if (effects.scheduleSave && callbacks.scheduleSave) {
        callbacks.scheduleSave();
    }
    if (effects.pushHistory && callbacks.pushHistory) {
        callbacks.pushHistory();
    }
}
