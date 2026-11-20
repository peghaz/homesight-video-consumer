#include <filesystem>
#include <homesight/clip_frame_sampler.hpp>
#include <homesight/errors.hpp>
#include <homesight/recorder_config.hpp>
#include <homesight/segment_info.hpp>
#include <homesight/segment_motion_analyzer.hpp>
#include <homesight/segment_recorder.hpp>
#include <homesight/segment_storage.hpp>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    homesight::MotionAnalysisParams params;
    homesight::SegmentMotionAnalyzer analyzer(params);
    const auto unknown = analyzer.analyze_segment_motion(directory / "missing.mp4");
    if (unknown.has_motion || unknown.status != homesight::MotionAnalysisStatus::failed)
    {
        return 3;
    }
    homesight::SegmentRecorderConfig config;
    config.camera = {"external_consumer", "rtsp://127.0.0.1:1/unavailable"};
    config.storage = homesight::LocalDirectoryStorageConfig{directory / "stored"};
    config.staging_directory = directory / "stage";
    homesight::SegmentRecorder recorder(config);
    if (recorder.is_recording() || !recorder.take_recorder_events().empty())
    {
        return 4;
    }
    std::cout << "Installed public C++ API passed\n";
    return 0;
}
