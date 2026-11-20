#include "homesight/segment_storage.hpp"
#include "homesight/errors.hpp"
#include "media.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <glib.h>
#include <mutex>
#include <regex>
#include <unistd.h>
#ifdef HOMESIGHT_ENABLE_S3_STORAGE
#include <curl/curl.h>
#endif

namespace homesight
{
namespace
{
std::filesystem::path relative_segment_path(const SegmentInfo &segment)
{
    if (!std::regex_match(segment.camera_name, std::regex("[a-z0-9_]{1,128}")) ||
        !std::regex_match(segment.segment_id, std::regex("[a-zA-Z0-9_-]{1,128}")))
    {
        throw InvalidConfigError("Invalid segment or camera identity");
    }
    auto *date = g_date_time_new_from_unix_utc(segment.start_epoch_ms / 1000);
    if (!date)
    {
        throw InvalidConfigError("Invalid segment start time");
    }
    auto *formatted = g_date_time_format(date, "%Y%m%d");
    std::string day(formatted);
    g_free(formatted);
    g_date_time_unref(date);
    const auto extension = segment.staging_file_path.extension().string();
    if (extension != ".mp4" && extension != ".mkv")
    {
        throw InvalidConfigError("Segment must use MP4 or Matroska");
    }
    return std::filesystem::path(segment.camera_name) / day / (segment.segment_id + extension);
}
std::string file_uri(const std::filesystem::path &path)
{
    auto *encoded = g_filename_to_uri(std::filesystem::absolute(path).c_str(), nullptr, nullptr);
    if (!encoded)
    {
        throw SegmentStorageError("Cannot encode local storage URI");
    }
    std::string uri(encoded);
    g_free(encoded);
    return uri;
}
class TemporaryFile
{
  public:
    std::filesystem::path path;
    int descriptor = -1;
    explicit TemporaryFile(const std::filesystem::path &destination)
    {
        detail::create_directories_durable(
            destination.parent_path().empty() ? "." : destination.parent_path());
        auto pattern = (destination.parent_path() / ".homesight-download-XXXXXX").string();
        descriptor = g_mkstemp_full(pattern.data(), O_RDWR | O_CLOEXEC, 0600);
        if (descriptor < 0)
        {
            throw SegmentStorageError("Cannot create exclusive storage temporary");
        }
        path = pattern;
    }
    ~TemporaryFile()
    {
        if (descriptor >= 0)
        {
            close(descriptor);
        }
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;
    void publish(const std::filesystem::path &destination)
    {
        detail::sync_file(path);
        std::filesystem::rename(path, destination);
        detail::sync_directory(destination.parent_path());
    }
};
void publish_copy(const std::filesystem::path &source, const std::filesystem::path &destination)
{
    TemporaryFile temporary(destination);
    std::filesystem::copy_file(source, temporary.path,
                               std::filesystem::copy_options::overwrite_existing);
    temporary.publish(destination);
}
std::filesystem::path publication_temporary(const std::filesystem::path &destination)
{
    return destination.parent_path() /
           (".homesight-publish-" + destination.filename().string() + ".copying");
}
void publish_journaled_copy(const std::filesystem::path &source,
                            const std::filesystem::path &destination)
{
    detail::create_directories_durable(destination.parent_path());
    const auto temporary = publication_temporary(destination);
    detail::remove_file_durable(temporary);
    std::filesystem::copy_file(source, temporary);
    detail::sync_file(temporary);
    std::filesystem::rename(temporary, destination);
    detail::sync_directory(destination.parent_path());
}
class LocalStorage final : public SegmentStorage
{
  public:
    explicit LocalStorage(const LocalDirectoryStorageConfig &config)
    {
        if (config.root_directory.empty())
        {
            throw InvalidConfigError("Local storage root is required");
        }
        detail::create_directories_durable(config.root_directory);
        root_ = std::filesystem::canonical(config.root_directory);
    }
    std::string store_segment_file(const CompletedSegment &completed) override
    {
        std::lock_guard lock(mutex_);
        auto destination = resolve(file_uri(root_ / relative_segment_path(completed.segment)));
        auto source = std::filesystem::weakly_canonical(completed.segment.staging_file_path);
        if (std::filesystem::exists(source) &&
            std::filesystem::file_size(source) != completed.segment.file_size_bytes)
        {
            throw SegmentStorageError("Staged segment size does not match journal metadata");
        }
        if (source != destination)
        {
            if (!std::filesystem::exists(destination))
            {
                publish_journaled_copy(source, destination);
            }
            if (std::filesystem::file_size(destination) != completed.segment.file_size_bytes)
            {
                throw SegmentStorageError("Stored segment size does not match journal metadata");
            }
            detail::remove_file_durable(source);
        }
        else
        {
            detail::sync_file(destination);
        }
        return file_uri(destination);
    }
    void delete_stored_segment(const std::string &uri) override
    {
        std::lock_guard lock(mutex_);
        auto path = resolve(uri);
        if (std::filesystem::exists(path) && !std::filesystem::is_regular_file(path))
        {
            throw InvalidConfigError("A segment URI must identify a regular file");
        }
        detail::remove_file_durable(path);
    }
    std::optional<std::string> find_stored_segment(const CompletedSegment &completed) override
    {
        std::lock_guard lock(mutex_);
        auto destination = resolve(file_uri(root_ / relative_segment_path(completed.segment)));
        if (std::filesystem::exists(destination.parent_path()))
        {
            detail::remove_file_durable(publication_temporary(destination));
        }
        if (!std::filesystem::exists(destination))
        {
            return std::nullopt;
        }
        if (std::filesystem::file_size(destination) != completed.segment.file_size_bytes)
        {
            throw SegmentStorageError("Published segment size does not match journal metadata");
        }
        return file_uri(destination);
    }
    void download_segment_to_local_file(const std::string &uri,
                                        const std::filesystem::path &destination) override
    {
        std::lock_guard lock(mutex_);
        auto source = resolve(uri);
        if (std::filesystem::absolute(destination) != source)
        {
            publish_copy(source, std::filesystem::absolute(destination));
        }
    }

  private:
    std::filesystem::path root_;
    std::mutex mutex_;
    std::filesystem::path resolve(const std::string &uri)
    {
        char *host = nullptr;
        auto *decoded = g_filename_from_uri(uri.c_str(), &host, nullptr);
        const bool valid_host = !host || *host == '\0' || std::string(host) == "localhost";
        g_free(host);
        if (!decoded || !valid_host)
        {
            g_free(decoded);
            throw InvalidConfigError("Invalid local storage URI");
        }
        auto path = std::filesystem::weakly_canonical(decoded);
        g_free(decoded);
        auto relative = path.lexically_relative(root_);
        if (relative.empty() || relative == "." || *relative.begin() == "..")
        {
            throw InvalidConfigError("Storage URI is outside the configured root");
        }
        return path;
    }
};
#ifdef HOMESIGHT_ENABLE_S3_STORAGE
std::string environment(const char *name)
{
    auto *value = std::getenv(name);
    return value ? value : "";
}
class S3Storage final : public SegmentStorage
{
  public:
    explicit S3Storage(S3StorageConfig config) : config_(std::move(config))
    {
        static std::once_flag initialized;
        std::call_once(initialized,
                       []
                       {
                           if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
                           {
                               throw SegmentStorageError("Cannot initialize S3 transport");
                           }
                       });
        if (!std::regex_match(config_.bucket_name,
                              std::regex("[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]")) ||
            config_.region.empty() || !std::isfinite(config_.request_timeout_seconds) ||
            config_.request_timeout_seconds <= 0 || config_.request_timeout_seconds > 86400 ||
            !std::isfinite(config_.retry_initial_delay_seconds) ||
            config_.retry_initial_delay_seconds <= 0 ||
            !std::isfinite(config_.retry_max_delay_seconds) ||
            config_.retry_max_delay_seconds > 86400 ||
            config_.retry_max_delay_seconds < config_.retry_initial_delay_seconds ||
            config_.key_prefix.find("..") != std::string::npos ||
            config_.key_prefix.find_first_of("\r\n?#") != std::string::npos)
        {
            throw InvalidConfigError("Invalid S3 configuration");
        }
        auto &credentials = config_.credentials;
        if (credentials.access_key_id.empty())
        {
            credentials.access_key_id = environment("AWS_ACCESS_KEY_ID");
            credentials.secret_access_key = environment("AWS_SECRET_ACCESS_KEY");
            credentials.session_token = environment("AWS_SESSION_TOKEN");
        }
        if (credentials.access_key_id.empty() || credentials.secret_access_key.empty())
        {
            throw InvalidConfigError(
                "S3 requires explicit credentials or AWS environment credentials");
        }
        for (const auto *value : {&credentials.access_key_id, &credentials.secret_access_key,
                                  &credentials.session_token})
        {
            if (value->find_first_of("\r\n") != std::string::npos)
            {
                throw InvalidConfigError("Invalid S3 credentials");
            }
        }
        endpoint_ = config_.endpoint_url.empty() ? "https://s3." + config_.region + ".amazonaws.com"
                                                 : config_.endpoint_url;
        while (endpoint_.ends_with('/'))
        {
            endpoint_.pop_back();
        }
        auto scheme_end = endpoint_.find("://");
        if (!(endpoint_.starts_with("https://") || endpoint_.starts_with("http://")) ||
            endpoint_.find_first_of("@?#\r\n") != std::string::npos ||
            endpoint_.find('/', scheme_end + 3) != std::string::npos)
        {
            throw InvalidConfigError("S3 endpoint must be an HTTP(S) origin without credentials");
        }
        if (config_.use_path_style_addressing)
        {
            endpoint_ += '/' + config_.bucket_name;
        }
        else
        {
            endpoint_.insert(scheme_end + 3, config_.bucket_name + '.');
        }
    }
    std::string store_segment_file(const CompletedSegment &completed) override
    {
        const auto key = object_key(completed);
        request("PUT", key, completed.segment.staging_file_path, &completed);
        return "s3://" + config_.bucket_name + '/' + key;
    }
    std::optional<std::string> find_stored_segment(const CompletedSegment &completed) override
    {
        const auto key = object_key(completed);
        if (request("HEAD", key, {}, nullptr) == 404)
        {
            return std::nullopt;
        }
        return "s3://" + config_.bucket_name + '/' + key;
    }
    std::string object_key(const CompletedSegment &completed)
    {
        auto key = relative_segment_path(completed.segment).generic_string();
        if (!config_.key_prefix.empty())
        {
            auto prefix = config_.key_prefix;
            while (prefix.starts_with('/'))
            {
                prefix.erase(0, 1);
            }
            while (prefix.ends_with('/'))
            {
                prefix.pop_back();
            }
            if (!prefix.empty())
            {
                key = prefix + '/' + key;
            }
        }
        return key;
    }
    void delete_stored_segment(const std::string &uri) override
    {
        request("DELETE", parse_uri(uri), {}, nullptr);
    }
    void download_segment_to_local_file(const std::string &uri,
                                        const std::filesystem::path &destination) override
    {
        request("GET", parse_uri(uri), destination, nullptr);
    }

  private:
    S3StorageConfig config_;
    std::string endpoint_;
    std::string parse_uri(const std::string &uri)
    {
        auto prefix = "s3://" + config_.bucket_name + '/';
        if (!uri.starts_with(prefix))
        {
            throw InvalidConfigError("S3 URI does not belong to the configured bucket");
        }
        return uri.substr(prefix.size());
    }
    static std::size_t discard(char *, std::size_t size, std::size_t count, void *)
    {
        return size * count;
    }
    static std::size_t read(char *data, std::size_t size, std::size_t count, void *file)
    {
        return fread(data, size, count, static_cast<FILE *>(file)) * size;
    }
    static std::size_t write(char *data, std::size_t size, std::size_t count, void *file)
    {
        return fwrite(data, size, count, static_cast<FILE *>(file)) * size;
    }
    long request(const std::string &method, const std::string &key,
                 const std::filesystem::path &file, const CompletedSegment *completed)
    {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 curl_easy_cleanup);
        if (!curl)
        {
            throw SegmentStorageError("Cannot initialize S3 request");
        }
        auto *escaped = g_uri_escape_string(key.c_str(), "/-_.~", false);
        std::string url = endpoint_ + '/' + escaped;
        g_free(escaped);
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(
            curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
            std::max(1L, static_cast<long>(std::ceil(config_.request_timeout_seconds * 1000))));
        curl_easy_setopt(
            curl.get(), CURLOPT_TIMEOUT_MS,
            std::max(1L, static_cast<long>(std::ceil(config_.request_timeout_seconds * 1000))));
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER,
                         config_.verify_tls_certificates ? 1L : 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST,
                         config_.verify_tls_certificates ? 2L : 0L);
        auto auth = "aws:amz:" + config_.region + ":s3";
        curl_easy_setopt(curl.get(), CURLOPT_AWS_SIGV4, auth.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_USERNAME, config_.credentials.access_key_id.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PASSWORD,
                         config_.credentials.secret_access_key.c_str());
        curl_slist *headers = nullptr;
        auto add_header = [&headers](const std::string &value)
        {
            headers = curl_slist_append(headers, value.c_str());
        };
        add_header("x-amz-content-sha256: UNSIGNED-PAYLOAD");
        if (!config_.credentials.session_token.empty())
        {
            add_header("x-amz-security-token: " + config_.credentials.session_token);
        }
        if (completed)
        {
            const auto &s = completed->segment;
            add_header("Content-Type: " + std::string(s.staging_file_path.extension() == ".mp4"
                                                          ? "video/mp4"
                                                          : "video/x-matroska"));
            add_header("x-amz-meta-segment-id: " + s.segment_id);
            add_header("x-amz-meta-camera-name: " + s.camera_name);
            add_header("x-amz-meta-start-epoch-ms: " + std::to_string(s.start_epoch_ms));
            add_header("x-amz-meta-end-epoch-ms: " + std::to_string(s.end_epoch_ms));
            add_header("x-amz-meta-duration-seconds: " + std::to_string(s.duration_seconds));
            add_header("x-amz-meta-video-codec: " + s.video_codec);
            add_header("x-amz-meta-file-size-bytes: " + std::to_string(s.file_size_bytes));
            add_header("x-amz-meta-state-revision: " + std::to_string(completed->state_revision));
            if (completed->motion)
            {
                const auto &m = *completed->motion;
                const auto &p = m.params_used;
                add_header("x-amz-meta-motion-status: " +
                           std::to_string(static_cast<int>(m.status)));
                add_header(
                    "x-amz-meta-has-motion: " +
                    std::string(m.has_motion ? (*m.has_motion ? "true" : "false") : "unknown"));
                add_header("x-amz-meta-peak-changed-area: " + std::to_string(m.peak_changed_area));
                add_header("x-amz-meta-mean-changed-area: " + std::to_string(m.mean_changed_area));
                add_header("x-amz-meta-frames-with-motion: " +
                           std::to_string(m.frames_with_motion));
                add_header("x-amz-meta-frames-analyzed: " + std::to_string(m.frames_analyzed));
                add_header("x-amz-meta-motion-params: " + std::to_string(p.sample_rate_fps) + "," +
                           std::to_string(p.analysis_frame_width) + "," +
                           std::to_string(p.warmup_seconds) + "," +
                           std::to_string(p.history_frames) + "," +
                           std::to_string(p.variance_threshold) + "," +
                           std::to_string(p.learning_rate) + "," +
                           std::to_string(p.changed_area_threshold) + "," +
                           std::to_string(p.minimum_frames_with_motion) + "," +
                           std::to_string(p.exclude_shadows));
            }
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_owner(
            headers, curl_slist_free_all);
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, discard);
        std::unique_ptr<TemporaryFile> temporary;
        auto close_file = [](FILE *stream)
        {
            fclose(stream);
        };
        std::unique_ptr<FILE, decltype(close_file)> stream(nullptr, close_file);
        if (method == "PUT")
        {
            stream.reset(fopen(file.c_str(), "rb"));
            if (!stream)
            {
                throw SegmentStorageError("Cannot open staged segment for S3 upload");
            }
            curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, read);
            curl_easy_setopt(curl.get(), CURLOPT_READDATA, stream.get());
            curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE,
                             static_cast<curl_off_t>(std::filesystem::file_size(file)));
        }
        else if (method == "GET")
        {
            temporary = std::make_unique<TemporaryFile>(std::filesystem::absolute(file));
            stream.reset(fdopen(temporary->descriptor, "wb"));
            if (!stream)
            {
                throw SegmentStorageError("Cannot create S3 download destination");
            }
            temporary->descriptor = -1;
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, stream.get());
        }
        else
        {
            if (method == "HEAD")
            {
                curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
            }
            curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        auto code = curl_easy_perform(curl.get());
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        if (method == "GET")
        {
            const int flush_result = fflush(stream.get());
            const int close_result = fclose(stream.release());
            if (flush_result != 0 || close_result != 0)
            {
                throw SegmentStorageError("Cannot flush S3 download to local storage");
            }
        }
        else
        {
            stream.reset();
        }
        if (code == CURLE_OK && status == 404 && method == "HEAD")
        {
            return status;
        }
        if (code != CURLE_OK || status < 200 || status >= 300)
        {
            throw SegmentStorageError("S3 request failed (transport " + std::to_string(code) +
                                      ", HTTP " + std::to_string(status) + ")");
        }
        if (method == "GET")
        {
            temporary->publish(std::filesystem::absolute(file));
        }
        return status;
    }
};
#endif
} // namespace
std::unique_ptr<SegmentStorage> create_segment_storage(const SegmentStorageConfig &config)
{
    if (const auto *local = std::get_if<LocalDirectoryStorageConfig>(&config))
    {
        return std::make_unique<LocalStorage>(*local);
    }
#ifdef HOMESIGHT_ENABLE_S3_STORAGE
    return std::make_unique<S3Storage>(std::get<S3StorageConfig>(config));
#else
    throw InvalidConfigError("This build has S3 storage disabled");
#endif
}
} // namespace homesight
