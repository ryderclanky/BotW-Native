#pragma once

#include <filesystem>
#include <string>

// Extract a zip (store or deflate, no encryption, no zip64) into dest.
// Rejects paths that escape dest, and refuses archives that expand too far.
bool unzip_to(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string& err);
