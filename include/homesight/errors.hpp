#pragma once
/** @file errors.hpp
 * Exception hierarchy for configuration, recording, decoding, and storage boundaries.
 */
#include <stdexcept>
namespace homesight
{
/// Base runtime error for recorder, journal, media, and storage operations.
class RecorderError : public std::runtime_error
{
  public:
    /// Inherit constructors accepting a diagnostic message.
    using std::runtime_error::runtime_error;
};
/// A supplied configuration or operation argument violates its supported constraints.
class InvalidConfigError : public RecorderError
{
  public:
    /// Inherit constructors accepting a diagnostic message.
    using RecorderError::RecorderError;
};
/// An RTSP transport failure that recorder workers may handle by reconnecting.
class CameraConnectionError : public RecorderError
{
  public:
    /// Inherit constructors accepting a diagnostic message.
    using RecorderError::RecorderError;
};
/// A recording cannot be decoded, sampled, or sought within its configured budget.
class SegmentDecodingError : public RecorderError
{
  public:
    /// Inherit constructors accepting a diagnostic message.
    using RecorderError::RecorderError;
};
/// A local or S3 storage operation could not be completed.
class SegmentStorageError : public RecorderError
{
  public:
    /// Inherit constructors accepting a diagnostic message.
    using RecorderError::RecorderError;
};
/// The separate durable journal budget is exhausted; committed events are preserved.
class JournalFullError : public RecorderError
{
  public:
    /// Inherit constructors accepting a diagnostic message.
    using RecorderError::RecorderError;
};
} // namespace homesight
