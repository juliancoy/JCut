#pragma once

#include "transcript_document_core.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jcut {

struct TranscriptSourceSpec {
    std::string sourcePath;
    std::string audioSourcePath;
    std::string audioSourceMode = "embedded";
    std::string audioSourceStatus = "unknown";
    int audioStreamIndex = -1;
    std::string sourceRootPath;
};

struct TranscriptSourceIdentity {
    std::string sourcePath;
    std::string canonicalKey;
    std::string fileStem;
    int audioStreamIndex = -1;

    bool valid() const noexcept { return !sourcePath.empty(); }
};

enum class TranscriptCutKind {
    Original,
    Editable,
    Version,
    Custom,
};

struct TranscriptCutEntry {
    std::string path;
    std::string label;
    TranscriptCutKind kind = TranscriptCutKind::Original;
    int version = 0;

    bool mutableCut() const noexcept { return kind != TranscriptCutKind::Original; }
};

struct TranscriptCutCatalog {
    TranscriptSourceIdentity source;
    std::string originalPath;
    std::string editablePath;
    std::string workingPath;
    std::vector<TranscriptCutEntry> cuts;
    std::string warning;
};

struct TranscriptCutSessionOptions {
    std::string requestedActivePath;
    TranscriptTiming timing;
    bool includeOutsideActiveCut = false;
    bool ensureEditable = true;
};

struct TranscriptFileStamp {
    bool exists = false;
    std::uintmax_t size = 0;
    std::int64_t writeTimeTicks = 0;

    bool operator==(const TranscriptFileStamp& other) const noexcept
    {
        return exists == other.exists && size == other.size &&
            writeTimeTicks == other.writeTimeTicks;
    }
    bool operator!=(const TranscriptFileStamp& other) const noexcept
    {
        return !(*this == other);
    }
};

struct TranscriptCutSession {
    TranscriptCutCatalog catalog;
    std::string activePath;
    bool activeCutMutable = false;
    std::optional<TranscriptDocumentCore> activeDocument;
    std::optional<TranscriptDocumentCore> originalDocument;
    std::vector<TranscriptRow> rows;
    TranscriptFileStamp activeStamp;
    TranscriptFileStamp originalStamp;
    std::vector<TranscriptFileStamp> cutStamps;
    std::int64_t catalogDirectoryWriteTimeTicks = 0;
    std::string warning;
    std::string error;

    bool ok() const noexcept { return activeDocument.has_value() && error.empty(); }
};

TranscriptSourceIdentity resolveTranscriptSourceIdentity(
    const TranscriptSourceSpec& source);
TranscriptCutCatalog discoverTranscriptCutCatalog(
    const TranscriptSourceSpec& source,
    bool ensureEditable = false);
TranscriptFileStamp inspectTranscriptFile(const std::string& path);
std::int64_t transcriptCatalogDirectoryWriteTime(
    const TranscriptSourceIdentity& source);
TranscriptCutSession loadTranscriptCutSession(
    const TranscriptSourceSpec& source,
    const TranscriptCutSessionOptions& options = {});
std::optional<std::string> createTranscriptCutVersion(
    const TranscriptCutSession& session,
    std::string* errorOut = nullptr);
bool renameTranscriptCut(const TranscriptCutSession& session,
                         const std::string& label,
                         std::string* errorOut = nullptr);
bool deleteTranscriptCut(const TranscriptCutSession& session,
                         std::string* fallbackPathOut = nullptr,
                         std::string* errorOut = nullptr);

} // namespace jcut
