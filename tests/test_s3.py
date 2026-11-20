"""Real S3 transport, metadata-bearing uploads and isolated object cleanup."""

from __future__ import annotations

import shutil
import subprocess
import time
import uuid
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from rich.console import Console
from rich.text import Text
from segment_report import ObjectState, SegmentReport

import homesight_core as core

if TYPE_CHECKING:
    from conftest import IntegrationSettings


@pytest.fixture
def s3_config(integration_settings: IntegrationSettings) -> core.S3StorageConfig:
    """Require S3 inputs and isolate each test under a unique object prefix."""
    settings = integration_settings
    assert settings.s3_endpoint and settings.s3_bucket, "Set TEST_S3_ENDPOINT and TEST_S3_BUCKET"
    assert settings.s3_access_key and settings.s3_secret_key, (
        "Set TEST_S3_ACCESS_KEY and TEST_S3_SECRET_KEY"
    )
    return core.S3StorageConfig(
        bucket_name=settings.s3_bucket,
        endpoint_url=settings.s3_endpoint,
        key_prefix=f"homesight-test/{uuid.uuid4()}",
        use_path_style_addressing=True,
        credentials=core.S3Credentials(
            access_key_id=settings.s3_access_key.get_secret_value(),
            secret_access_key=settings.s3_secret_key.get_secret_value(),
            session_token=settings.s3_session_token.get_secret_value()
            if settings.s3_session_token
            else "",
        ),
    )


def __finish_test_objects(
    storage: core.SegmentStorage,
    uris: set[str],
    *,
    keep_objects: bool,
    report: SegmentReport | None = None,
) -> None:
    """Retain objects on request, or attempt every deletion and report failures."""
    failures = list[core.SegmentStorageError]()
    for uri in sorted(uris):
        state: ObjectState = "Kept" if keep_objects else "Deleted"
        if not keep_objects:
            try:
                storage.delete_stored_segment(uri)
            except core.SegmentStorageError as error:
                failures.append(error)
                state = "Cleanup failed"
        if report is not None:
            report.set_object_state(uri, state)
        else:
            Console().print(Text(f"S3 object [{state}]: {uri}"), soft_wrap=True)
    if failures:
        raise ExceptionGroup("Could not delete all S3 test objects", failures)


@pytest.mark.s3
def test_real_s3_roundtrip(
    tmp_path: Path,
    integration_settings: IntegrationSettings,
    s3_config: core.S3StorageConfig,
    pytestconfig: pytest.Config,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Upload and retrieve actual camera footage using an isolated S3 object key."""
    assert integration_settings.segment is not None, "Set TEST_SEGMENT to actual camera footage"
    source: Path = tmp_path / "footage.mp4"
    shutil.copyfile(integration_settings.segment, source)
    completed = core.CompletedSegment(
        segment=core.SegmentInfo(
            segment_id=str(uuid.uuid4()),
            camera_name="camera",
            staging_file_path=source,
            start_epoch_ms=1700000000000,
            end_epoch_ms=1700000010000,
            duration_seconds=10,
            video_codec="h264",
            file_size_bytes=source.stat().st_size,
        ),
        motion=core.MotionAnalysisResult(status=core.MotionAnalysisStatus.warming_up),
    )
    storage: core.SegmentStorage = core.create_segment_storage(s3_config)
    uri: str = storage.store_segment_file(completed)
    try:
        assert source.exists(), "Recorder commits upload state before releasing staging files"
        downloaded: Path = tmp_path / "downloaded.mp4"
        storage.download_segment_to_local_file(uri, downloaded)
        assert downloaded.read_bytes() == source.read_bytes()
        assert storage.store_segment_file(completed) == uri
    finally:
        with capsys.disabled():
            Console().print(
                Text(f"S3 destination: s3://{s3_config.bucket_name}/{s3_config.key_prefix}/"),
                soft_wrap=True,
            )
            __finish_test_objects(
                storage, {uri}, keep_objects=pytestconfig.getoption("--keep-s3-objects")
            )


@pytest.mark.live
@pytest.mark.s3
@pytest.mark.real_rtsp_s3
def test_real_rtsp_with_s3(
    tmp_path: Path,
    integration_settings: IntegrationSettings,
    s3_config: core.S3StorageConfig,
    pytestconfig: pytest.Config,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Real RTSP Test with S3: record, upload, download and play camera segments."""
    assert integration_settings.rtsp_url is not None, "Set TEST_RTSP_URL to an actual camera"
    config = core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(
            camera_name="camera",
            rtsp_url=integration_settings.rtsp_url.get_secret_value(),
            rtsp_transport=core.RtspTransport.tcp,
        ),
        storage=s3_config,
        staging_directory=tmp_path / "stage",
        container_format=core.VideoContainerFormat.mp4,
        delete_segments_without_motion=False,
    )
    storage: core.SegmentStorage = core.create_segment_storage(s3_config)
    recorder = core.SegmentRecorder(config)
    events = list[core.RecorderEvent]()
    keep_objects: bool = pytestconfig.getoption("--keep-s3-objects")
    with (
        capsys.disabled(),
        SegmentReport(
            f"s3://{s3_config.bucket_name}/{s3_config.key_prefix}/",
            keep_objects=keep_objects,
            params=config.motion_analysis,
        ) as report,
    ):
        started: int = time.time_ns() // 1_000_000
        try:
            try:
                recorder.start_recording()
                deadline: float = time.monotonic() + integration_settings.camera_test_seconds
                while time.monotonic() < deadline:
                    event: core.RecorderEvent | None = recorder.wait_for_next_recorder_event(
                        timeout_ms=500
                    )
                    if event is not None:
                        events.append(event)
                        report.record(event.completed_segment)
                    statistics: core.RecorderStatistics = recorder.get_recorder_statistics()
                    report.refresh(statistics)
                    assert statistics.state != core.RecorderState.failed, statistics.last_error
            finally:
                try:
                    report.refresh(phase="Finalizing")
                    recorder.stop_recording(finalize_timeout_ms=10000)
                finally:
                    for final_event in recorder.take_recorder_events():
                        events.append(final_event)
                        report.record(final_event.completed_segment)
                    report.refresh(recorder.get_recorder_statistics(), phase="Verifying")

            assert not recorder.is_recording()
            assert recorder.get_recorder_statistics().state == core.RecorderState.stopped, (
                recorder.get_recorder_statistics().last_error
            )
            stored: dict[str, core.CompletedSegment] = {
                event.completed_segment.segment.segment_id: event.completed_segment
                for event in events
                if event.completed_segment.storage_outcome == core.SegmentStorageOutcome.stored
            }
            statistics = recorder.get_recorder_statistics()
            assert stored, (
                f"No uploaded segments received: written={statistics.segments_written}, "
                f"uploaded={statistics.segments_stored}, "
                f"pending={statistics.segments_waiting_for_upload}, "
                f"storage_failures={statistics.storage_failures}, "
                f"reconnects={statistics.reconnect_attempts}, last_error={statistics.last_error}"
            )
            for segment_id, completed in stored.items():
                segment: core.SegmentInfo = completed.segment
                uri: str | None = completed.storage_uri
                assert uri is not None
                assert uri.startswith(f"s3://{s3_config.bucket_name}/{s3_config.key_prefix}/")
                assert uri.endswith(f"/{segment_id}.mp4")
                assert completed.storage_error is None
                assert segment.video_codec in {"h264", "h265"}
                assert segment.start_epoch_ms >= started
                assert segment.duration_seconds > 0
                assert (
                    abs(
                        segment.end_epoch_ms
                        - segment.start_epoch_ms
                        - segment.duration_seconds * 1000
                    )
                    < 2
                )
                assert not segment.staging_file_path.exists()
                downloaded: Path = tmp_path / "downloaded" / f"{segment_id}.mp4"
                storage.download_segment_to_local_file(uri, downloaded)
                assert downloaded.stat().st_size == segment.file_size_bytes > 0
                frames = core.sample_clip_frames_from_segment(downloaded, 2, 32, 32)
                assert frames.shape == (2, 32, 32, 3)
                probe: subprocess.CompletedProcess[str] = subprocess.run(
                    [
                        "ffprobe",
                        "-v",
                        "error",
                        "-show_entries",
                        "format=duration",
                        "-of",
                        "default=noprint_wrappers=1:nokey=1",
                        str(downloaded),
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                assert abs(segment.duration_seconds - float(probe.stdout)) < 0.005
                report.set_object_state(uri, "Verified")
        finally:
            __finish_test_objects(
                storage,
                {
                    event.completed_segment.storage_uri
                    for event in events
                    if event.completed_segment.storage_uri is not None
                },
                keep_objects=keep_objects,
                report=report,
            )
