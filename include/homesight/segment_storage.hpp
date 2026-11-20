#pragma once
/** @file segment_storage.hpp
 * Synchronous local and S3-compatible segment storage interfaces.
 */
#include "homesight/segment_info.hpp"
#include <memory>
namespace homesight
{
/** Publish segments with idempotent keys derived from stable segment identity.
 * Calls are synchronous and bounded by configured network timeouts. Recorder
 * workers add durable intent, acknowledgement, cleanup, and retry handling.
 * Standalone callers must coordinate file mutations and journal their own intents;
 * no cross-call transaction or caller-file lifetime synchronization is supplied.
 */
class SegmentStorage
{
  public:
    /// Destroy the selected backend through its public interface.
    virtual ~SegmentStorage() = default;
    /** Publish a recording and its identifying/motion metadata.
     * Standalone S3 publication does not remove staging: the recorder commits the
     * durable storage event before releasing it. Local publication can move staging.
     * @param completed Full segment snapshot with a readable staging file.
     * @return File or S3 URI for the stable destination key.
     * @throws InvalidConfigError If segment identity or extension is invalid.
     * @throws SegmentStorageError If publication fails; no standalone retry loop is performed.
     */
    [[nodiscard]] virtual std::string store_segment_file(const CompletedSegment &completed) = 0;
    /** Probe the stable destination after uncertain publication or process restart.
     * @param completed Segment identity used to derive the archive/object key.
     * @return URI if present, or an empty optional for definite absence.
     * @throws SegmentStorageError If presence cannot be established; failure is not absence.
     */
    [[nodiscard]] virtual std::optional<std::string>
    find_stored_segment(const CompletedSegment &completed) = 0;
    /** Remove a backend-owned recording.
     * @param storage_uri URI within the configured archive root or bucket/prefix.
     * @throws SegmentStorageError If deletion fails or the URI escapes this backend.
     */
    virtual void delete_stored_segment(const std::string &storage_uri) = 0;
    /** Atomically publish a downloaded/copied recording using an exclusive temporary file.
     * @param storage_uri Backend-owned source URI.
     * @param destination Local destination; unrelated sibling files are preserved.
     * @throws SegmentStorageError If URI validation, transfer, or durable publication fails.
     */
    virtual void download_segment_to_local_file(const std::string &storage_uri,
                                                const std::filesystem::path &destination) = 0;
};
/** Create a backend without opening an RTSP camera.
 * @param config Local-directory or S3-compatible settings; S3 may use AWS environment credentials.
 * @return Exclusively owned synchronous storage implementation.
 * @throws InvalidConfigError If backend settings or credentials are invalid.
 * @throws SegmentStorageError If S3 is disabled or backend initialization fails.
 */
[[nodiscard]] std::unique_ptr<SegmentStorage>
create_segment_storage(const SegmentStorageConfig &config);
} // namespace homesight
