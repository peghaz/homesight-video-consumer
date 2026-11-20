#include "homesight/clip_frame_sampler.hpp"
#include "homesight/errors.hpp"
#include "homesight/segment_motion_analyzer.hpp"
#include "homesight/segment_recorder.hpp"
#include "journal.hpp"
#include "recording_pipeline.hpp"
#include "timeline_coverage.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        return 2;
    }
    try
    {
        std::filesystem::path directory(argv[2]);
        if (std::string(argv[1]) == "rtsp_url")
        {
            const auto check = [](const std::string &input, const std::string &location,
                                  const std::string &username, const std::string &password)
            {
                const auto parsed = homesight::detail::parse_rtsp_connection_settings(input);
                return parsed.location == location && parsed.username == username &&
                       parsed.password == password;
            };
            if (!check("rtsp://viewer:p@ss@127.0.0.1:8554/live?x=a%2Fb@c",
                       "rtsp://127.0.0.1:8554/live?x=a%2Fb@c", "viewer", "p@ss") ||
                !check("rtsp://view%40er:p%40ss%3A%25@127.0.0.1/cam%2Fstream?token=x%40y",
                       "rtsp://127.0.0.1/cam%2Fstream?token=x%40y", "view@er", "p@ss:%") ||
                !check("rtsp://127.0.0.1/user@cam?x=@", "rtsp://127.0.0.1/user@cam?x=@", "", "") ||
                !check("rtsps://viewer:a:b@[::1]:8554/live", "rtsps://[::1]:8554/live", "viewer",
                       "a:b") ||
                !check("rtsp://viewer@127.0.0.1/live", "rtsp://127.0.0.1/live", "viewer", ""))
            {
                std::cerr << "RTSP credentials or request URI changed during parsing\n";
                return 13;
            }
            try
            {
                (void)homesight::detail::parse_rtsp_connection_settings(
                    "rtsp://viewer:%zz@host/live");
                return 14;
            }
            catch (const homesight::InvalidConfigError &)
            {
            }
            return 0;
        }
        if (std::string(argv[1]) == "media")
        {
            const auto *source = std::getenv("TEST_SEGMENT");
            if (!source)
            {
                return 2;
            }
            auto frames = homesight::sample_clip_frames_from_segment(source, 4, 64, 48, false, 30);
            if (frames.rgb_pixels.size() != 4 * 64 * 48 * 3 ||
                frames.frame_timestamps_ms.size() != 4)
            {
                return 11;
            }
            homesight::MotionAnalysisParams params;
            params.warmup_seconds = 3600;
            params.decode_timeout_seconds = 30;
            homesight::SegmentMotionAnalyzer analyzer(params);
            auto result = analyzer.analyze_segment_motion(source);
            if (result.has_motion || result.status != homesight::MotionAnalysisStatus::warming_up ||
                result.frames_analyzed < 2)
            {
                return 12;
            }
            return 0;
        }
        if (std::string(argv[1]) == "deadline")
        {
            struct Connection
            {
                homesight::SegmentRecorderConfig config;
                std::atomic<bool> stop{false};
                std::atomic<bool> opened{false};
                std::atomic<std::int64_t> deadline{0};
            };
            auto state = std::make_shared<Connection>();
            const auto *url = std::getenv("TEST_RTSP_URL");
            if (!url)
            {
                return 2;
            }
            state->config.camera.camera_name = "camera";
            state->config.camera.rtsp_url = url;
            state->config.staging_directory = directory;
            state->config.segment_duration_seconds = 1;
            std::jthread worker(
                [state]
                {
                    try
                    {
                        homesight::detail::record_session(
                            state->config, state->stop, state->deadline,
                            [state](const homesight::SegmentInfo &)
                            {
                                state->opened = true;
                                std::this_thread::sleep_for(std::chrono::seconds(1));
                            },
                            [state](const homesight::SegmentInfo &)
                            {
                            },
                            []
                            {
                            });
                    }
                    catch (const homesight::RecorderError &)
                    {
                    }
                });
            auto connection_deadline = homesight::detail::monotonic_ms() + 10000;
            while (!state->opened && homesight::detail::monotonic_ms() < connection_deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const auto before = homesight::detail::monotonic_ms();
            state->deadline = before + 100;
            state->stop = true;
            worker.join();
            if (!state->opened || homesight::detail::monotonic_ms() - before > 500)
            {
                return 9;
            }
            while (state.use_count() > 1 && homesight::detail::monotonic_ms() - before < 2500)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (state.use_count() != 1)
            {
                return 10;
            }
            return 0;
        }
        if (std::string(argv[1]) == "coverage")
        {
            homesight::detail::TimelineCoverage complete;
            homesight::detail::TimelineCoverage missing_tail;
            homesight::detail::TimelineCoverage backwards;
            for (int i = 0; i < 100; ++i)
            {
                complete.observe(i * 0.04, 0.04);
                if (i < 2)
                {
                    missing_tail.observe(i * 0.04, 0.04);
                }
                backwards.observe(i < 50 ? i * 0.04 : (i - 50) * 0.04, 0.04);
            }
            if (!complete.is_complete(4) || missing_tail.is_complete(4) || backwards.is_complete(4))
            {
                return 6;
            }
            homesight::detail::TimelineCoverage missing_start;
            missing_start.observe(3.96, 0.04);
            if (missing_start.is_complete(4))
            {
                return 7;
            }
            return 0;
        }
        if (std::string(argv[1]) == "startstop")
        {
            homesight::SegmentRecorderConfig config;
            config.camera.camera_name = "camera";
            config.camera.rtsp_url = "rtsp://127.0.0.1:1/live";
            config.storage = homesight::LocalDirectoryStorageConfig{directory / "stored"};
            config.staging_directory = directory / "stage";
            config.network_timeout_seconds = 0.1;
            homesight::SegmentRecorder recorder(config);
            for (int i = 0; i < 3; ++i)
            {
                recorder.start_recording();
                auto event = recorder.wait_for_next_recorder_event(std::chrono::milliseconds(100));
                if (event)
                {
                    return 4;
                }
                recorder.stop_recording(std::chrono::seconds(3));
                if (recorder.is_recording())
                {
                    return 5;
                }
            }
            return 0;
        }
        if (std::string(argv[1]) == "process_media" && argc == 4)
        {
            homesight::SegmentRecorderConfig config;
            config.camera = {"camera", "rtsp://127.0.0.1:1/unavailable"};
            config.storage = homesight::LocalDirectoryStorageConfig{directory / "stored"};
            config.staging_directory = directory;
            config.network_timeout_seconds = 0.1;
            config.delete_segments_without_motion = true;
            homesight::SegmentRecorder recorder(config);
            recorder.start_recording();
            std::set<std::string> finished;
            const auto deadline = homesight::detail::monotonic_ms() + 90000;
            const auto expected = static_cast<std::size_t>(std::stoul(argv[3]));
            while (finished.size() < expected && homesight::detail::monotonic_ms() < deadline)
            {
                const auto event =
                    recorder.wait_for_next_recorder_event(std::chrono::milliseconds(100));
                if (event && (event->completed_segment.storage_outcome ==
                                  homesight::SegmentStorageOutcome::stored ||
                              event->completed_segment.storage_outcome ==
                                  homesight::SegmentStorageOutcome::deleted_without_motion))
                {
                    finished.insert(event->completed_segment.segment.segment_id);
                }
            }
            recorder.stop_recording(std::chrono::milliseconds(100));
            return finished.size() == expected ? 0 : 15;
        }
        homesight::detail::Journal journal(directory / "camera", 1024 * 1024);
        if (std::string(argv[1]) == "queue_media" && argc == 9)
        {
            homesight::detail::JournalRow row;
            auto &info = row.completed.segment;
            info.segment_id = argv[4];
            info.camera_name = "camera";
            const auto video = directory / "camera/video";
            std::filesystem::create_directories(video);
            info.staging_file_path =
                video / (info.segment_id + std::filesystem::path(argv[3]).extension().string());
            std::filesystem::copy_file(argv[3], info.staging_file_path);
            info.continuity_id = argv[5];
            info.media_start_seconds = std::stod(argv[6]);
            info.duration_seconds = std::stod(argv[7]);
            info.start_epoch_ms = 1700000000000;
            info.end_epoch_ms =
                info.start_epoch_ms + static_cast<std::int64_t>(info.duration_seconds * 1000);
            info.video_codec = argv[8];
            info.file_size_bytes = std::filesystem::file_size(info.staging_file_path);
            row.local_path = info.staging_file_path;
            row.phase = "analysis";
            journal.save(row);
            return 0;
        }
        if (std::string(argv[1]) == "resolve_success" || std::string(argv[1]) == "resolve_failure")
        {
            auto rows = journal.rows();
            if (rows.size() != 1 || rows[0].completed.state_revision != 2)
            {
                return 8;
            }
            auto &row = rows[0];
            row.phase = "done";
            if (std::string(argv[1]) == "resolve_success")
            {
                row.completed.storage_outcome = homesight::SegmentStorageOutcome::stored;
                row.completed.storage_uri = "s3://camera123/segment_0.mp4";
                journal.save(row, homesight::RecorderEventKind::storage_changed);
            }
            else
            {
                journal.save(row);
            }
            return 0;
        }
        if (std::string(argv[1]) == "fill")
        {
            std::size_t saved = 0;
            try
            {
                for (;;)
                {
                    homesight::detail::JournalRow row;
                    row.completed.segment.segment_id = "event_" + std::to_string(saved);
                    row.completed.segment.camera_name = "camera";
                    row.completed.segment.staging_file_path = "segment.mp4";
                    row.completed.storage_error = std::string(2048, 'e');
                    row.phase = "done";
                    journal.save(row, homesight::RecorderEventKind::segment_completed);
                    ++saved;
                }
            }
            catch (const homesight::JournalFullError &)
            {
                auto events = journal.events(0, 100000);
                if (saved == 0 || events.size() != saved)
                {
                    return 3;
                }
                std::cout << saved << '\n';
                return 0;
            }
        }
        if (argc != 6)
        {
            return 2;
        }
        std::string phase(argv[3]);
        int count = std::stoi(argv[4]);
        int bytes = std::stoi(argv[5]);
        auto video = directory / "camera" / "video";
        std::filesystem::create_directories(video);
        for (int i = 0; i < count; ++i)
        {
            homesight::detail::JournalRow row;
            auto &info = row.completed.segment;
            info.segment_id = "segment_" + std::to_string(i);
            info.camera_name = "camera";
            info.staging_file_path = video / (info.segment_id + ".mp4");
            info.start_epoch_ms = 1700000000000 - i * 10000;
            info.end_epoch_ms = info.start_epoch_ms + 10000;
            info.duration_seconds = 10;
            info.video_codec = "h264";
            info.file_size_bytes = static_cast<std::uint64_t>(bytes);
            info.continuity_id = "camera_session";
            {
                std::ofstream file(info.staging_file_path, std::ios::binary);
                file << std::string(static_cast<std::size_t>(bytes), 'x');
            }
            row.local_path = info.staging_file_path;
            row.phase = phase;
            if (phase == "open")
            {
                info.end_epoch_ms = 0;
                info.duration_seconds = 0;
                info.file_size_bytes = 0;
            }
            if (phase == "done")
            {
                row.completed.storage_outcome = homesight::SegmentStorageOutcome::stored;
                row.completed.storage_uri = "file://" + info.staging_file_path.string();
            }
            if (phase == "open")
            {
                journal.save(row);
            }
            else
            {
                journal.save(row, homesight::RecorderEventKind::segment_completed);
            }
        }
        std::_Exit(23); // Exercise committed WAL recovery without any destructors.
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
