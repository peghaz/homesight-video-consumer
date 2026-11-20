"""Committed journal recovery, ownership, event replay, and local quota behavior."""

import subprocess
from pathlib import Path

import pytest

import homesight_core as core


def recorder_config(tmp_path: Path, allowance_bytes: int = 2**31) -> core.SegmentRecorderConfig:
    """Construct a recorder that does not need a live camera for recovery checks."""
    return core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(camera_name="camera", rtsp_url="rtsp://127.0.0.1:1/live"),
        storage=core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stored"),
        staging_directory=tmp_path / "stage",
        analyze_motion_after_segment_closes=False,
        network_timeout_seconds=0.1,
        local_video_allowance_bytes=allowance_bytes,
    )


def seed_state(driver: Path, tmp_path: Path, phase: str, count: int, size: int) -> None:
    """Leave durable work behind by exiting a real native process abruptly."""
    result: subprocess.CompletedProcess[str] = subprocess.run(
        [str(driver), "seed", str(tmp_path / "stage"), phase, str(count), str(size)],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 23, result.stderr


def test_events_replay_until_acknowledged(state_driver: Path, tmp_path: Path) -> None:
    """Reading cannot erase events, and acknowledgement survives a fresh owner."""
    seed_state(state_driver, tmp_path, "done", 3, 4)
    config: core.SegmentRecorderConfig = recorder_config(tmp_path)
    recorder = core.SegmentRecorder(config)
    events: list[core.RecorderEvent] = recorder.take_recorder_events()
    assert len(events) == 3
    assert [event.sequence for event in events] == [1, 2, 3]
    identities: list[str] = [event.event_id for event in events]
    assert len(set(identities)) == 3
    assert recorder.take_recorder_events() == []
    del recorder
    recorder = core.SegmentRecorder(config)
    assert [event.event_id for event in recorder.take_recorder_events()] == identities
    recorder.acknowledge_recorder_events([identities[0], identities[2]])
    recorder.acknowledge_recorder_events([identities[0]])
    del recorder
    recorder = core.SegmentRecorder(config)
    assert [event.event_id for event in recorder.take_recorder_events()] == [identities[1]]


@pytest.mark.parametrize("phase", ["done", "storage"])
def test_quota_evicts_oldest_completed_including_pending(
    state_driver: Path,
    tmp_path: Path,
    phase: str,
) -> None:
    """Completion order wins over wall-clock regressions and pending upload status."""
    seed_state(state_driver, tmp_path, phase, 3, 4)
    recorder = core.SegmentRecorder(recorder_config(tmp_path, 8))
    recorder.start_recording()
    recorder.stop_recording(finalize_timeout_ms=100)
    events: list[core.RecorderEvent] = recorder.take_recorder_events()
    deleted: list[core.RecorderEvent] = [
        event for event in events if event.kind == core.RecorderEventKind.deleted_by_quota
    ]
    assert len(deleted) == 1
    assert deleted[0].completed_segment.segment.segment_id == "segment_0"
    assert (
        deleted[0].completed_segment.storage_outcome == core.SegmentStorageOutcome.deleted_by_quota
    )
    assert deleted[0].completed_segment.state_revision == 2
    files: list[Path] = [*tmp_path.rglob("*.mp4")]
    assert sum(path.stat().st_size for path in files) == 8
    assert not any(path.name == "segment_0.mp4" for path in files)


@pytest.mark.parametrize("phase", ["delete_quota", "delete_motion", "cleanup"])
def test_interrupted_deletion_intents_are_reconciled(
    state_driver: Path,
    tmp_path: Path,
    phase: str,
) -> None:
    """Restart completes an interrupted file operation without losing its state."""
    seed_state(state_driver, tmp_path, phase, 1, 4)
    recorder = core.SegmentRecorder(recorder_config(tmp_path))
    recorder.start_recording()
    recorder.stop_recording(finalize_timeout_ms=100)
    assert list((tmp_path / "stage").rglob("*.mp4")) == []
    events: list[core.RecorderEvent] = recorder.take_recorder_events()
    assert len(events) == (1 if phase == "cleanup" else 2)


def test_journal_exhaustion_preserves_every_committed_event(
    state_driver: Path, tmp_path: Path
) -> None:
    """Journal capacity is an explicit error and never a lossy queue."""
    result: subprocess.CompletedProcess[str] = subprocess.run(
        [str(state_driver), "fill", str(tmp_path / "stage")],
        check=True,
        capture_output=True,
        text=True,
    )
    assert int(result.stdout) > 1
    recorder = core.SegmentRecorder(recorder_config(tmp_path))
    assert len(recorder.take_recorder_events(max_count=100000)) == int(result.stdout)


def test_native_worker_lifetimes(state_driver: Path, tmp_path: Path) -> None:
    """A standalone C++ consumer repeatedly starts and joins real native workers."""
    subprocess.run([str(state_driver), "startstop", str(tmp_path)], check=True, timeout=30)


def test_rtsp_credentials_are_separated_from_connection_uri(
    state_driver: Path, tmp_path: Path
) -> None:
    """Preserve credentials containing @, escaped bytes and encoded stream paths."""
    subprocess.run([str(state_driver), "rtsp_url", str(tmp_path)], check=True)


def test_incomplete_timeline_cannot_be_valid(state_driver: Path, tmp_path: Path) -> None:
    """Missing media boundaries and backwards timestamps cannot authorize deletion."""
    subprocess.run([str(state_driver), "coverage", str(tmp_path)], check=True)


@pytest.mark.parametrize("outcome", ["success", "failure"])
def test_acknowledgement_keeps_inflight_eviction_revision(
    state_driver: Path,
    tmp_path: Path,
    outcome: str,
) -> None:
    """A committed eviction survives acknowledgement until upload uncertainty resolves."""
    seed_state(state_driver, tmp_path, "uploading", 1, 4)
    config: core.SegmentRecorderConfig = recorder_config(tmp_path, 1)
    config.storage = core.S3StorageConfig(
        bucket_name="camera123",
        endpoint_url="http://127.0.0.1:1",
        use_path_style_addressing=True,
        request_timeout_seconds=0.1,
        credentials=core.S3Credentials(access_key_id="test-key", secret_access_key="test-secret"),
    )
    recorder = core.SegmentRecorder(config)
    recorder.start_recording()
    recorder.stop_recording(finalize_timeout_ms=100)
    events: list[core.RecorderEvent] = recorder.take_recorder_events()
    assert events[-1].kind == core.RecorderEventKind.deleted_by_quota
    assert events[-1].completed_segment.state_revision == 2
    recorder.acknowledge_recorder_events([event.event_id for event in events])
    del recorder
    subprocess.run(
        [str(state_driver), f"resolve_{outcome}", str(tmp_path / "stage")],
        check=True,
    )
    recorder = core.SegmentRecorder(config)
    resolved: list[core.RecorderEvent] = recorder.take_recorder_events()
    if outcome == "success":
        assert len(resolved) == 1
        assert resolved[0].completed_segment.state_revision == 3
        assert resolved[0].completed_segment.storage_outcome == core.SegmentStorageOutcome.stored
    else:
        assert resolved == []


def test_interrupted_local_publication_is_reconciled_before_quota(
    state_driver: Path,
    tmp_path: Path,
) -> None:
    """A published archive with an uncommitted journal update remains tracked by quota."""
    seed_state(state_driver, tmp_path, "storage", 1, 4)
    source = tmp_path / "stage/camera/video/segment_0.mp4"
    destination = tmp_path / "stored/camera/20231114/segment_0.mp4"
    destination.parent.mkdir(parents=True)
    destination.write_bytes(source.read_bytes())
    source.unlink()
    partial = destination.parent / ".homesight-publish-segment_0.mp4.copying"
    partial.write_bytes(b"incomplete publication")
    recorder = core.SegmentRecorder(recorder_config(tmp_path, 1))
    recorder.start_recording()
    recorder.stop_recording(finalize_timeout_ms=100)
    assert not destination.exists()
    assert not partial.exists()
    events: list[core.RecorderEvent] = recorder.take_recorder_events()
    assert events[-1].kind == core.RecorderEventKind.deleted_by_quota
    assert events[-1].completed_segment.state_revision == 3


def test_interrupted_recording_preserves_file_and_valid_time_bounds(
    state_driver: Path,
    tmp_path: Path,
) -> None:
    """Unreadable crash footage is retained without an end time preceding its start."""
    seed_state(state_driver, tmp_path, "open", 1, 4)
    recorder = core.SegmentRecorder(recorder_config(tmp_path))
    recorder.start_recording()
    recorder.stop_recording(finalize_timeout_ms=100)
    event: core.RecorderEvent | None = recorder.wait_for_next_recorder_event(timeout_ms=0)
    assert event is not None
    completed: core.CompletedSegment = event.completed_segment
    assert completed.storage_outcome == core.SegmentStorageOutcome.interrupted
    assert completed.segment.staging_file_path.exists()
    assert completed.segment.end_epoch_ms >= completed.segment.start_epoch_ms
