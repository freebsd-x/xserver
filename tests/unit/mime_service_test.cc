// Tests for MimeService — extension→MIME mapping and handler registry.
// Tests mimeFromExtension() (pure table lookup) and empty-registry behavior
// of defaultHandler() / allHandlers().  No IpcSocket or filesystem I/O.

#include "xserver/mime_service.h"
#include <gtest/gtest.h>

namespace fbx::xserver::testing {

// ── mimeFromExtension ─────────────────────────────────────────────────────────

TEST(MimeServiceTest, TextExtensions) {
    EXPECT_EQ(MimeService::mimeFromExtension(".txt"),  "text/plain");
    EXPECT_EQ(MimeService::mimeFromExtension(".md"),   "text/markdown");
    EXPECT_EQ(MimeService::mimeFromExtension(".html"), "text/html");
    EXPECT_EQ(MimeService::mimeFromExtension(".htm"),  "text/html");
    EXPECT_EQ(MimeService::mimeFromExtension(".css"),  "text/css");
    EXPECT_EQ(MimeService::mimeFromExtension(".js"),   "text/javascript");
}

TEST(MimeServiceTest, ImageExtensions) {
    EXPECT_EQ(MimeService::mimeFromExtension(".png"),  "image/png");
    EXPECT_EQ(MimeService::mimeFromExtension(".jpg"),  "image/jpeg");
    EXPECT_EQ(MimeService::mimeFromExtension(".jpeg"), "image/jpeg");
    EXPECT_EQ(MimeService::mimeFromExtension(".gif"),  "image/gif");
    EXPECT_EQ(MimeService::mimeFromExtension(".webp"), "image/webp");
    EXPECT_EQ(MimeService::mimeFromExtension(".svg"),  "image/svg+xml");
}

TEST(MimeServiceTest, VideoExtensions) {
    EXPECT_EQ(MimeService::mimeFromExtension(".mp4"),  "video/mp4");
    EXPECT_EQ(MimeService::mimeFromExtension(".mkv"),  "video/x-matroska");
    EXPECT_EQ(MimeService::mimeFromExtension(".webm"), "video/webm");
}

TEST(MimeServiceTest, AudioExtensions) {
    EXPECT_EQ(MimeService::mimeFromExtension(".mp3"),  "audio/mpeg");
    EXPECT_EQ(MimeService::mimeFromExtension(".ogg"),  "audio/ogg");
    EXPECT_EQ(MimeService::mimeFromExtension(".wav"),  "audio/wav");
    EXPECT_EQ(MimeService::mimeFromExtension(".flac"), "audio/flac");
}

TEST(MimeServiceTest, ArchiveExtensions) {
    EXPECT_EQ(MimeService::mimeFromExtension(".zip"),  "application/zip");
    EXPECT_EQ(MimeService::mimeFromExtension(".tar"),  "application/x-tar");
    EXPECT_EQ(MimeService::mimeFromExtension(".gz"),   "application/gzip");
    EXPECT_EQ(MimeService::mimeFromExtension(".bz2"),  "application/x-bzip2");
    EXPECT_EQ(MimeService::mimeFromExtension(".xz"),   "application/x-xz");
}

TEST(MimeServiceTest, CodeExtensions) {
    EXPECT_EQ(MimeService::mimeFromExtension(".cc"),   "text/x-c++");
    EXPECT_EQ(MimeService::mimeFromExtension(".cpp"),  "text/x-c++");
    EXPECT_EQ(MimeService::mimeFromExtension(".hpp"),  "text/x-c++");
    EXPECT_EQ(MimeService::mimeFromExtension(".c"),    "text/x-c");
    EXPECT_EQ(MimeService::mimeFromExtension(".h"),    "text/x-c");
    EXPECT_EQ(MimeService::mimeFromExtension(".py"),   "text/x-python");
    EXPECT_EQ(MimeService::mimeFromExtension(".rs"),   "text/x-rust");
}

TEST(MimeServiceTest, PdfExtension) {
    EXPECT_EQ(MimeService::mimeFromExtension(".pdf"), "application/pdf");
}

TEST(MimeServiceTest, JsonExtension) {
    EXPECT_EQ(MimeService::mimeFromExtension(".json"), "application/json");
}

TEST(MimeServiceTest, UnknownExtensionFallback) {
    EXPECT_EQ(MimeService::mimeFromExtension(".xyz"),     "application/octet-stream");
    EXPECT_EQ(MimeService::mimeFromExtension(".unknown"), "application/octet-stream");
    EXPECT_EQ(MimeService::mimeFromExtension(""),         "application/octet-stream");
}

TEST(MimeServiceTest, ExtensionIsCaseInsensitive) {
    EXPECT_EQ(MimeService::mimeFromExtension(".PNG"),  "image/png");
    EXPECT_EQ(MimeService::mimeFromExtension(".JPG"),  "image/jpeg");
    EXPECT_EQ(MimeService::mimeFromExtension(".TXT"),  "text/plain");
    EXPECT_EQ(MimeService::mimeFromExtension(".MP3"),  "audio/mpeg");
}

TEST(MimeServiceTest, MixedCaseExtension) {
    EXPECT_EQ(MimeService::mimeFromExtension(".Png"),  "image/png");
    EXPECT_EQ(MimeService::mimeFromExtension(".mD"),   "text/markdown");
}

// ── defaultHandler / allHandlers (empty registry) ────────────────────────────

TEST(MimeServiceTest, DefaultHandlerEmptyRegistryReturnsEmpty) {
    // Before loadFromAppsDir() is called, the registry is empty.
    EXPECT_EQ(MimeService::defaultHandler("image/png"), "");
    EXPECT_EQ(MimeService::defaultHandler("text/plain"), "");
    EXPECT_EQ(MimeService::defaultHandler("unknown/type"), "");
}

TEST(MimeServiceTest, AllHandlersEmptyRegistryReturnsEmpty) {
    EXPECT_TRUE(MimeService::allHandlers("image/png").empty());
    EXPECT_TRUE(MimeService::allHandlers("video/mp4").empty());
}

// ── loadFromAppsDir with non-existent directory ───────────────────────────────

TEST(MimeServiceTest, LoadFromNonExistentDirDoesNotCrash) {
    // Should silently no-op rather than throw.
    EXPECT_NO_FATAL_FAILURE(
        MimeService::loadFromAppsDir("/nonexistent/path/to/apps")
    );
}

} // namespace fbx::xserver::testing
