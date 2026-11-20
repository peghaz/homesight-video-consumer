"""Public behavior that does not require a camera or an S3 endpoint."""

from pathlib import Path

import pytest


def test_native_package_is_importable() -> None:
    """The installed package exposes its native implementation."""
    import homesight_core as core

    assert core.__version__ == "0.1.0"


def test_reject_invalid_recorder_configuration(tmp_path: Path) -> None:
    """Invalid camera identities cannot escape the storage directory."""
    import homesight_core as core

    config = core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(camera_name="../escape", rtsp_url="rtsp://localhost/live"),
        storage=core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stored"),
        staging_directory=tmp_path / "staging",
    )
    with pytest.raises(core.InvalidConfigError):
        core.SegmentRecorder(config)


def test_camera_state_has_exclusive_ownership(tmp_path: Path) -> None:
    """Two recorder instances cannot concurrently own a camera journal."""
    import homesight_core as core

    config = core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(camera_name="camera", rtsp_url="rtsp://localhost/live"),
        storage=core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stored"),
        staging_directory=tmp_path / "staging",
    )
    recorder = core.SegmentRecorder(config)
    with pytest.raises(core.RecorderError):
        core.SegmentRecorder(config)
    assert recorder.take_recorder_events() == []
    recorder.stop_recording()


def test_decoding_errors_preserve_input(tmp_path: Path) -> None:
    """A failed decode reports an error and never removes the source."""
    import homesight_core as core

    source = tmp_path / "broken.mp4"
    source.write_bytes(b"not a video")
    with pytest.raises(core.SegmentDecodingError):
        core.sample_clip_frames_from_segment(source, 4, 32, 32)
    result = core.SegmentMotionAnalyzer().analyze_segment_motion(source)
    assert result.status == core.MotionAnalysisStatus.failed
    assert result.has_motion is None
    assert source.exists()


def test_local_storage_rejects_symlink_escape(tmp_path: Path) -> None:
    """An existing camera-directory symlink cannot redirect writes outside storage."""
    import homesight_core as core

    root = tmp_path / "stored"
    outside = tmp_path / "outside"
    root.mkdir()
    outside.mkdir()
    (root / "camera").symlink_to(outside, target_is_directory=True)
    source = tmp_path / "segment.mp4"
    source.write_bytes(b"owned file")
    completed = core.CompletedSegment(
        segment=core.SegmentInfo(
            camera_name="camera",
            segment_id="segment",
            staging_file_path=source,
            start_epoch_ms=1700000000000,
            file_size_bytes=10,
        )
    )
    storage = core.create_segment_storage(core.LocalDirectoryStorageConfig(root_directory=root))
    with pytest.raises(core.InvalidConfigError):
        storage.store_segment_file(completed)
    assert source.exists()
    assert list(outside.rglob("*.mp4")) == []


def test_local_storage_keeps_file_when_staging_aliases_destination(tmp_path: Path) -> None:
    """Equivalent source/destination paths must not remove the published recording."""
    import homesight_core as core

    root = tmp_path / "stored"
    destination = root / "camera/20231114/segment.mp4"
    destination.parent.mkdir(parents=True)
    destination.write_bytes(b"owned file")
    alias = tmp_path / "alias"
    alias.symlink_to(root, target_is_directory=True)
    completed = core.CompletedSegment(
        segment=core.SegmentInfo(
            camera_name="camera",
            segment_id="segment",
            staging_file_path=alias / "camera/20231114/segment.mp4",
            start_epoch_ms=1700000000000,
            file_size_bytes=10,
        )
    )
    storage = core.create_segment_storage(core.LocalDirectoryStorageConfig(root_directory=root))
    uri = storage.store_segment_file(completed)
    assert uri.endswith("/stored/camera/20231114/segment.mp4")
    assert destination.read_bytes() == b"owned file"


def test_local_download_preserves_unrelated_sibling(tmp_path: Path) -> None:
    """A download owns only its own exclusive temporary file."""
    import homesight_core as core

    root = tmp_path / "stored"
    root.mkdir()
    source = root / "source.mp4"
    source.write_bytes(b"owned file")
    destination = tmp_path / "download.mp4"
    sibling = tmp_path / "download.mp4.copying"
    sibling.write_bytes(b"unrelated user file")
    storage = core.create_segment_storage(core.LocalDirectoryStorageConfig(root_directory=root))
    storage.download_segment_to_local_file(storage_uri=source.as_uri(), destination=destination)
    assert destination.read_bytes() == b"owned file"
    assert sibling.read_bytes() == b"unrelated user file"


def test_s3_dictionary_selects_storage_alternative() -> None:
    """Nested dictionaries preserve the selected storage implementation."""
    import homesight_core as core

    config = core.SegmentRecorderConfig.from_dict({"storage": {"bucket_name": "camera123"}})
    assert isinstance(config.storage, core.S3StorageConfig)
    assert config.storage.bucket_name == "camera123"


def test_motion_metadata_roundtrips() -> None:
    """The optional motion snapshot survives nested value deserialization."""
    import homesight_core as core

    original = core.CompletedSegment(
        motion=core.MotionAnalysisResult(
            status=core.MotionAnalysisStatus.valid,
            has_motion=True,
            frames_with_motion=3,
        )
    )
    restored = core.CompletedSegment.from_dict(original.to_dict())
    assert restored.motion is not None
    assert restored.motion.has_motion is True
    assert restored.motion.frames_with_motion == 3


def test_local_recording_fault_fails_before_connecting(tmp_path: Path) -> None:
    """A blocked recording directory is a visible local fault, not a reconnect loop."""
    import homesight_core as core

    config = core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(camera_name="camera", rtsp_url="rtsp://127.0.0.1:1/live"),
        storage=core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stored"),
        staging_directory=tmp_path / "stage",
    )
    recorder = core.SegmentRecorder(config)
    (tmp_path / "stage/camera/video").write_bytes(b"not a directory")
    with pytest.raises(core.RecorderError):
        recorder.start_recording()
    assert recorder.get_recorder_statistics().state == core.RecorderState.failed


def test_local_storage_cannot_delete_its_root_directory(tmp_path: Path) -> None:
    """Deleting a segment must never remove the configured archive directory."""
    import homesight_core as core

    root = tmp_path / "stored"
    storage = core.create_segment_storage(core.LocalDirectoryStorageConfig(root_directory=root))
    with pytest.raises(core.InvalidConfigError):
        storage.delete_stored_segment(root.as_uri())
    assert root.is_dir()
