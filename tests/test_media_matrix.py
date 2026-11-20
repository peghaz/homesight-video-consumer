"""Cover camera-native H.264/H.265 footage in both supported output containers."""

from __future__ import annotations

import time
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from test_resilience import collect_until, recovery_config, stored_ids
from verification_services import RtspReplay, probe_media

import homesight_core as core

if TYPE_CHECKING:
    from conftest import IntegrationSettings

pytestmark = pytest.mark.media


@pytest.mark.parametrize("codec", ["h264", "h265"])
@pytest.mark.parametrize("container", ["mp4", "matroska"])
def test_camera_codec_container_matrix(
    tmp_path: Path,
    integration_settings: IntegrationSettings,
    codec: str,
    container: str,
) -> None:
    """Remux actual camera footage, verify playback/times and preserve unknown results."""
    source: Path | None = (
        integration_settings.h264_segment if codec == "h264" else integration_settings.h265_segment
    )
    assert source is not None, f"Set TEST_{codec.upper()}_SEGMENT to actual camera footage"
    replay = RtspReplay(source, integration_settings.rtsp_python, tmp_path)
    assert replay.codec == ("hevc" if codec == "h265" else "h264")
    assert replay.probe.format.duration >= 12
    config: core.SegmentRecorderConfig = recovery_config(
        tmp_path, replay, core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stored")
    )
    config.container_format = getattr(core.VideoContainerFormat, container)
    recorder = core.SegmentRecorder(config)
    events = list[core.RecorderEvent]()
    started: int = time.time_ns() // 1_000_000
    try:
        replay.start()
        recorder.start_recording()
        collect_until(recorder, events, lambda: len(stored_ids(events)) >= 2)
    finally:
        stop_started: float = time.monotonic()
        recorder.stop_recording(finalize_timeout_ms=3000)
        elapsed: float = time.monotonic() - stop_started
        events.extend(recorder.take_recorder_events())
        replay.stop()
    assert elapsed < 15
    assert recorder.get_recorder_statistics().state == core.RecorderState.stopped
    stored: dict[str, core.CompletedSegment] = {
        event.completed_segment.segment.segment_id: event.completed_segment
        for event in events
        if event.completed_segment.storage_outcome == core.SegmentStorageOutcome.stored
    }
    assert len(stored) >= 2
    for identity, completed in stored.items():
        segment: core.SegmentInfo = completed.segment
        extension = ".mp4" if container == "mp4" else ".mkv"
        path: Path = next((tmp_path / "stored").rglob(identity + extension))
        probe = probe_media(path)
        assert probe.streams[0].codec_name == ("hevc" if codec == "h265" else "h264")
        assert abs(probe.format.duration - segment.duration_seconds) < 0.005
        assert segment.start_epoch_ms >= started
        assert (
            abs(segment.end_epoch_ms - segment.start_epoch_ms - segment.duration_seconds * 1000) < 2
        )
        assert segment.video_codec == codec
        assert segment.file_size_bytes == path.stat().st_size > 0
        assert completed.motion is not None and completed.motion.has_motion is None
        assert core.sample_clip_frames_from_segment(path, 3, 64, 48).shape == (3, 48, 64, 3)


@pytest.mark.parametrize("codec", ["h264", "h265"])
def test_discontinuities_reset_conservatively(
    integration_settings: IntegrationSettings,
    codec: str,
) -> None:
    """Continuity changes, explicit resets and timestamp gaps restore warm-up status."""
    source: Path | None = (
        integration_settings.h264_segment if codec == "h264" else integration_settings.h265_segment
    )
    assert source is not None, f"Set TEST_{codec.upper()}_SEGMENT to actual camera footage"
    duration: float = probe_media(source).format.duration
    analyzer = core.SegmentMotionAnalyzer(core.MotionAnalysisParams(warmup_seconds=1))
    first: core.MotionAnalysisResult = analyzer.analyze_segment_motion(source, "one", 0)
    assert first.status == core.MotionAnalysisStatus.warming_up and first.has_motion is None
    continuous: core.MotionAnalysisResult = analyzer.analyze_segment_motion(source, "one", duration)
    assert continuous.status == core.MotionAnalysisStatus.valid
    gap: core.MotionAnalysisResult = analyzer.analyze_segment_motion(source, "one", duration * 4)
    assert gap.status == core.MotionAnalysisStatus.warming_up and gap.has_motion is None
    changed: core.MotionAnalysisResult = analyzer.analyze_segment_motion(source, "two", 0)
    assert changed.status == core.MotionAnalysisStatus.warming_up and changed.has_motion is None
    analyzer.reset()
    reset: core.MotionAnalysisResult = analyzer.analyze_segment_motion(source, "two", duration)
    assert reset.status == core.MotionAnalysisStatus.warming_up and reset.has_motion is None
