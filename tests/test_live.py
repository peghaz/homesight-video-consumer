"""Opt-in integration against an actual H.264/H.265 RTSP camera."""

from __future__ import annotations

import os
import subprocess
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

import homesight_core as core

if TYPE_CHECKING:
    from conftest import IntegrationSettings


@pytest.mark.live
@pytest.mark.parametrize(
    "container", [core.VideoContainerFormat.mp4, core.VideoContainerFormat.matroska]
)
def test_live_recording_sampling_shutdown_and_replay(
    tmp_path: Path,
    integration_settings: IntegrationSettings,
    container: core.VideoContainerFormat,
) -> None:
    """Record playable segments with host times, durable notifications and a bounded stop."""
    assert integration_settings.rtsp_url is not None, "Set TEST_RTSP_URL to an actual camera"
    config = core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(
            camera_name="camera", rtsp_url=integration_settings.rtsp_url.get_secret_value()
        ),
        storage=core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stored"),
        staging_directory=tmp_path / "stage",
        container_format=container,
        motion_analysis=core.MotionAnalysisParams(warmup_seconds=3600),
        delete_segments_without_motion=True,
    )
    recorder = core.SegmentRecorder(config)
    started: int = time.time_ns() // 1_000_000
    recorder.start_recording()
    deadline: float = time.monotonic() + integration_settings.camera_test_seconds
    events = list[core.RecorderEvent]()
    while time.monotonic() < deadline:
        event: core.RecorderEvent | None = recorder.wait_for_next_recorder_event(timeout_ms=500)
        if event is not None:
            events.append(event)
    stop_started: float = time.monotonic()
    recorder.stop_recording(finalize_timeout_ms=10000)
    assert time.monotonic() - stop_started < 22
    assert not recorder.is_recording()
    stored: list[core.CompletedSegment] = [
        event.completed_segment
        for event in events
        if event.completed_segment.storage_outcome == core.SegmentStorageOutcome.stored
    ]
    assert len(stored) >= 1, recorder.get_recorder_statistics().last_error
    assert all(value.segment.video_codec in {"h264", "h265"} for value in stored)
    assert all(value.segment.start_epoch_ms >= started for value in stored)
    assert len({value.segment.segment_id for value in stored}) == len(stored)
    assert all(value.motion is not None and value.motion.has_motion is None for value in stored)
    for value in stored:
        assert value.segment.duration_seconds > 0
        assert (
            abs(
                value.segment.end_epoch_ms
                - value.segment.start_epoch_ms
                - value.segment.duration_seconds * 1000
            )
            < 2
        )
    files: list[Path] = [*tmp_path.rglob("*.mp4"), *tmp_path.rglob("*.mkv")]
    assert len(files) >= len(stored)
    for path in files:
        frames = core.sample_clip_frames_from_segment(path, 2, 32, 32)
        assert frames.shape == (2, 32, 32, 3)
        metadata = next(
            (value.segment for value in stored if value.segment.segment_id == path.stem), None
        )
        if metadata is not None:
            probe: subprocess.CompletedProcess[str] = subprocess.run(
                [
                    "ffprobe",
                    "-v",
                    "error",
                    "-show_entries",
                    "format=duration",
                    "-of",
                    "default=noprint_wrappers=1:nokey=1",
                    str(path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            assert abs(metadata.duration_seconds - float(probe.stdout)) < 0.005
    del recorder
    recorder = core.SegmentRecorder(config)
    replay: list[core.RecorderEvent] = recorder.take_recorder_events()
    assert {value.event_id for value in events} <= {value.event_id for value in replay}
    recorder.acknowledge_recorder_events([value.event_id for value in replay])
    del recorder
    recorder = core.SegmentRecorder(config)
    assert recorder.take_recorder_events() == []


def test_blocking_wait_releases_gil(tmp_path: Path) -> None:
    """Another Python thread runs while the native event wait is blocked."""
    config = core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(camera_name="camera", rtsp_url="rtsp://127.0.0.1:1/live"),
        storage=core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stored"),
        staging_directory=tmp_path / "stage",
    )
    recorder = core.SegmentRecorder(config)
    progressed = list[float]()

    def record_progress() -> None:
        """Record when the Python worker acquires the GIL during the native wait."""
        time.sleep(0.03)
        progressed.append(time.monotonic())

    started: float = time.monotonic()
    worker = threading.Thread(target=record_progress)
    worker.start()
    assert recorder.wait_for_next_recorder_event(timeout_ms=150) is None
    worker.join()
    assert progressed and progressed[0] - started < 0.12


@pytest.mark.live
def test_stalled_streaming_callback_respects_deadline(
    state_driver: Path,
    tmp_path: Path,
    integration_settings: IntegrationSettings,
) -> None:
    """EOS and teardown cannot block the thread enforcing the finalization deadline."""
    assert integration_settings.rtsp_url is not None, "Set TEST_RTSP_URL to an actual camera"
    environment = dict(os.environ)
    environment["TEST_RTSP_URL"] = integration_settings.rtsp_url.get_secret_value()
    subprocess.run(
        [str(state_driver), "deadline", str(tmp_path)],
        check=True,
        timeout=15,
        env=environment,
    )
