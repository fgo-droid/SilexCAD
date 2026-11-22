#pragma once
// Autosave files and crash recovery (pure C++, no Qt: unit-testable).
//
// An autosave is a regular .silex document wrapped with recovery metadata:
//   { "silex_autosave": 1, "saved_at": "2026-11-22T10:42:00Z", "saved_at_epoch": 1795...,
//     "source_path": "C:/parts/bracket.silex" | "", "display_name": "bracket", "pid": 1234,
//     "document": { ...docToJson()... } }
// It lives in <user data>/recovery/<name>_<hash>.autosave.json and is deleted as soon as the user
// saves or closes the project cleanly. A file still present at start-up is an orphan: the
// session that wrote it ended unexpectedly.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "model/Document.h"

namespace silex::recovery {

inline constexpr const char* kSuffix = ".autosave.json";

struct AutosaveInfo
{
    std::filesystem::path file;
    std::string sourcePath;   ///< saved document path ("" = never saved)
    std::string displayName;  ///< project name shown to the user
    std::string savedAt;      ///< ISO 8601 UTC
    int64_t savedAtEpoch = 0; ///< seconds since 1970 (UTC)
    int64_t pid = 0;          ///< process that wrote it
};

/// "<name>_<8 hex>.autosave.json". The hash comes from the source path, or from `sessionKey`
/// for a document that was never saved (so two unsaved sessions never share a file).
std::string autosaveFileName(const std::string& sourcePath, const std::string& sessionKey);
/// Project name for the dialog: file stem of the source, or "Untitled".
std::string displayNameFor(const std::string& sourcePath);

/// Writes atomically (temporary file + rename). `meta.file` is ignored; saved_at is set to now
/// when `meta.savedAtEpoch` is 0. Throws std::runtime_error on I/O failure.
void writeAutosave(const std::filesystem::path& file, const DocState& doc, AutosaveInfo meta);
/// Autosave files of a folder, newest first. Unreadable files are skipped.
std::vector<AutosaveInfo> scan(const std::filesystem::path& dir);
/// The document stored in an autosave file. Throws on a corrupt file.
DocState readAutosave(const std::filesystem::path& file, AutosaveInfo* meta = nullptr);

}  // namespace silex::recovery
