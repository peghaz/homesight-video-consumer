"""Exercise real transport failures using services owned exclusively by pytest."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

import pytest
from verification_services import (
    MinioService,
    RtspReplay,
    probe_media,
    wait_until,
)

import homesight_core as core

pytestmark = pytest.mark.resilience


def recovery_config(
    root: Path, replay: RtspReplay, storage: core.LocalDirectoryStorageConfig | core.S3StorageConfig
) -> core.SegmentRecorderConfig:
    """Use short bounded operations while retaining real encoded camera frames."""
    return core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(camera_name="camera", rtsp_url=replay.url),
        storage=storage,
        staging_directory=root / "stage",
        segment_duration_seconds=2,
        container_format=core.VideoContainerFormat.matroska,
        network_timeout_seconds=1,
        reconnect_initial_delay_seconds=0.2,
        reconnect_max_delay_seconds=1,
        motion_analysis=core.MotionAnalysisParams(warmup_seconds=30),
        delete_segments_without_motion=True,
    )


def collect_until(
    recorder: core.SegmentRecorder,
    events: list[core.RecorderEvent],
    predicate: Callable[[], bool],
    *,
    seconds: float = 30,
) -> None:
    """Collect durable events while a caller-specific condition becomes true."""

    def progressed() -> bool:
        """Record every revision and fail early on a terminal recorder error."""
        events.extend(recorder.take_recorder_events())
        stats: core.RecorderStatistics = recorder.get_recorder_statistics()
        assert stats.state != core.RecorderState.failed, stats.last_error
        return predicate()

    wait_until(progressed, seconds=seconds, description="recorder recovery events")


def stored_ids(events: list[core.RecorderEvent]) -> set[str]:
    """Extract stable segment identities after a committed storage success."""
    return {
        event.completed_segment.segment.segment_id
        for event in events
        if event.completed_segment.storage_outcome == core.SegmentStorageOutcome.stored
    }


def test_stream_interruption_reconnect_resets_motion(
    tmp_path: Path,
    rtsp_replay: RtspReplay,
) -> None:
    """Restart the owned RTSP server and preserve unknown footage across discontinuity."""
    config: core.SegmentRecorderConfig = recovery_config(
        tmp_path, rtsp_replay, core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stored")
    )
    recorder = core.SegmentRecorder(config)
    events = list[core.RecorderEvent]()
    try:
        recorder.start_recording()
        collect_until(recorder, events, lambda: bool(stored_ids(events)))
        before: set[str] = {event.completed_segment.segment.continuity_id for event in events}
        rtsp_replay.stop()
        collect_until(
            recorder,
            events,
            lambda: recorder.get_recorder_statistics().reconnect_attempts >= 1,
            seconds=15,
        )
        rtsp_replay.start()
        collect_until(
            recorder,
            events,
            lambda: any(
                event.completed_segment.segment.continuity_id not in before
                and event.completed_segment.storage_outcome == core.SegmentStorageOutcome.stored
                for event in events
            ),
        )
    finally:
        recorder.stop_recording(finalize_timeout_ms=3000)
        events.extend(recorder.take_recorder_events())
    assert recorder.get_recorder_statistics().state == core.RecorderState.stopped
    recovered: list[core.CompletedSegment] = [
        event.completed_segment
        for event in events
        if event.completed_segment.segment.continuity_id not in before
        and event.completed_segment.storage_outcome == core.SegmentStorageOutcome.stored
    ]
    assert recovered
    for completed in recovered:
        assert completed.motion is not None and completed.motion.has_motion is None
        assert completed.segment.staging_file_path.name in {
            path.name for path in (tmp_path / "stored").rglob("*.mkv")
        }
    assert not any(
        event.completed_segment.storage_outcome == core.SegmentStorageOutcome.deleted_without_motion
        for event in events
    )


def test_native_replay_decoding_and_shutdown(
    state_driver: Path,
    rtsp_replay: RtspReplay,
    tmp_path: Path,
) -> None:
    """Exercise real native decode ownership and stalled callbacks under the chosen sanitizer."""
    environment: dict[str, str] = dict(os.environ)
    environment.update(TEST_RTSP_URL=rtsp_replay.url, TEST_SEGMENT=str(rtsp_replay.source))
    for mode, timeout in (("deadline", 20), ("media", 90)):
        subprocess.run(
            [str(state_driver), mode, str(tmp_path / mode)],
            env=environment,
            check=True,
            timeout=timeout,
        )


def test_upload_outage_retries_without_blocking_recording(
    tmp_path: Path,
    rtsp_replay: RtspReplay,
    minio_service: MinioService,
) -> None:
    """A real S3 outage retains pending videos and resumes bounded idempotent uploads."""
    config: core.SegmentRecorderConfig = recovery_config(
        tmp_path, rtsp_replay, minio_service.config
    )
    recorder = core.SegmentRecorder(config)
    events = list[core.RecorderEvent]()
    minio_service.interrupt()
    try:
        recorder.start_recording()
        collect_until(
            recorder,
            events,
            lambda: (
                recorder.get_recorder_statistics().storage_failures >= 1
                and recorder.get_recorder_statistics().segments_written >= 2
            ),
        )
        pending: set[str] = {
            event.completed_segment.segment.segment_id
            for event in events
            if event.completed_segment.storage_outcome
            in {core.SegmentStorageOutcome.pending, core.SegmentStorageOutcome.storage_failed}
        }
        assert pending
        assert all(
            (tmp_path / "stage/camera/video" / f"{identity}.mkv").is_file() for identity in pending
        )
        minio_service.resume()
        collect_until(recorder, events, lambda: pending <= stored_ids(events))
    finally:
        recorder.stop_recording(finalize_timeout_ms=3000)
        events.extend(recorder.take_recorder_events())
    storage: core.SegmentStorage = core.create_segment_storage(minio_service.config)
    latest: dict[str, core.CompletedSegment] = {
        event.completed_segment.segment.segment_id: event.completed_segment
        for event in events
        if event.completed_segment.storage_outcome == core.SegmentStorageOutcome.stored
    }
    for identity in pending:
        completed: core.CompletedSegment = latest[identity]
        assert completed.storage_uri is not None
        target: Path = tmp_path / f"download-{identity}.mkv"
        storage.download_segment_to_local_file(completed.storage_uri, target)
        assert target.stat().st_size == completed.segment.file_size_bytes
        assert probe_media(target).format.duration > 0
        assert not completed.segment.staging_file_path.exists()


def test_pending_upload_quota_evicts_oldest(
    tmp_path: Path,
    rtsp_replay: RtspReplay,
    minio_service: MinioService,
) -> None:
    """Evict pending real videos by journal order while the isolated endpoint is down."""
    config: core.SegmentRecorderConfig = recovery_config(
        tmp_path, rtsp_replay, minio_service.config
    )
    config.local_video_allowance_bytes = 1
    minio_service.interrupt()
    recorder = core.SegmentRecorder(config)
    events = list[core.RecorderEvent]()
    try:
        recorder.start_recording()
        collect_until(
            recorder,
            events,
            lambda: (
                sum(event.kind == core.RecorderEventKind.deleted_by_quota for event in events) >= 2
            ),
        )
    finally:
        recorder.stop_recording(finalize_timeout_ms=3000)
        events.extend(recorder.take_recorder_events())
    evicted: list[core.SegmentInfo] = [
        event.completed_segment.segment
        for event in events
        if event.kind == core.RecorderEventKind.deleted_by_quota
    ]
    assert len(evicted) >= 2
    assert [segment.media_start_seconds for segment in evicted] == sorted(
        segment.media_start_seconds for segment in evicted
    )
    assert evicted[1].media_start_seconds - evicted[0].media_start_seconds == pytest.approx(
        evicted[0].duration_seconds, abs=0.05
    )
    assert all(not segment.staging_file_path.exists() for segment in evicted)
    # A just-finalized segment can remain in the stopped backlog; active recordings
    # are explicitly outside the completed-video quota until finalization.
    for event in events:
        if event.completed_segment.storage_outcome == core.SegmentStorageOutcome.deleted_by_quota:
            assert not event.completed_segment.segment.staging_file_path.exists()


def test_process_crash_recovers_pending_uploads_and_events(
    tmp_path: Path,
    rtsp_replay: RtspReplay,
    minio_service: MinioService,
) -> None:
    """Kill the recorder process with pending uploads and recover its durable journal."""
    minio_service.interrupt()
    environment: dict[str, str] = dict(os.environ)
    environment.update(
        OWNED_RTSP_URL=rtsp_replay.url,
        OWNED_ROOT=str(tmp_path),
        OWNED_ENDPOINT=minio_service.endpoint,
        OWNED_ACCESS_KEY=minio_service.access_key,
        OWNED_SECRET_KEY=minio_service.secret_key,
    )
    with (tmp_path / "crash-worker.log").open("w") as log:
        worker: subprocess.Popen[str] = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("recording_worker.py")), "crash"],
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:

            def ready() -> bool:
                """Fail if the worker exits before committing pending recordings."""
                assert worker.poll() is None, (tmp_path / "crash-worker.log").read_text()
                return (tmp_path / "pending.json").exists()

            wait_until(ready, seconds=30, description="durable recordings before process kill")
        finally:
            worker.kill()
            worker.wait(timeout=10)
    pending: list[str] = json.loads((tmp_path / "pending.json").read_text())
    minio_service.resume()
    config: core.SegmentRecorderConfig = recovery_config(
        tmp_path, rtsp_replay, minio_service.config
    )
    recorder = core.SegmentRecorder(config)
    replayed: list[core.RecorderEvent] = recorder.take_recorder_events()
    assert set(pending) <= {event.event_id for event in replayed}
    pending_ids: set[str] = {event.completed_segment.segment.segment_id for event in replayed}
    try:
        recorder.start_recording()
        collect_until(recorder, replayed, lambda: pending_ids <= stored_ids(replayed))
    finally:
        recorder.stop_recording(finalize_timeout_ms=3000)
        replayed.extend(recorder.take_recorder_events())
    identities: set[str] = {event.event_id for event in replayed}
    del recorder
    recorder = core.SegmentRecorder(config)
    assert identities <= {event.event_id for event in recorder.take_recorder_events()}
    recorder.acknowledge_recorder_events(list(identities))
    del recorder
    recorder = core.SegmentRecorder(config)
    assert not recorder.take_recorder_events()
