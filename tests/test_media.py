"""Sampling and conservative MOG2 behavior on actual configured camera footage."""

from __future__ import annotations

import hashlib
import os
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, Literal, Self

import numpy as np
import pytest
from pydantic import AwareDatetime, BaseModel, ConfigDict, Field, model_validator
from verification_services import probe_media

import homesight_core as core

if TYPE_CHECKING:
    from conftest import IntegrationSettings


class RecordingLabel(BaseModel):
    """One chronological real-camera recording and its independently assigned label."""

    model_config = ConfigDict(extra="forbid")
    path: Path
    continuity_id: str
    start_media_seconds: float = Field(ge=0)
    expected_status: Literal["valid", "warming_up", "insufficient_data", "failed"]
    has_motion: bool | None
    sha256: str = Field(pattern=r"^[0-9a-f]{64}$")

    @model_validator(mode="after")
    def validate_label(self) -> Self:
        """Only independently labeled valid results can assert motion or stillness."""
        if (self.expected_status == "valid") != (self.has_motion is not None):
            raise ValueError("Only valid results have a boolean motion label")
        return self


class LabeledRecordings(BaseModel):
    """A chronological sequence covering warm-up, motion, and stillness."""

    model_config = ConfigDict(extra="forbid")
    reviewed_by: str = Field(min_length=1)
    reviewed_at: AwareDatetime
    recordings: list[RecordingLabel] = Field(min_length=3)

    @model_validator(mode="after")
    def validate_session_order(self) -> Self:
        """Reject interleaved sessions and backwards media times before any deletion."""
        seen = set[str]()
        current = ""
        previous_start = -1.0
        for recording in self.recordings:
            if recording.continuity_id != current:
                if recording.continuity_id in seen:
                    raise ValueError("Recording sessions must remain contiguous")
                current = recording.continuity_id
                seen.add(current)
                previous_start = -1.0
            if recording.start_media_seconds <= previous_start:
                raise ValueError("Media start times must increase within a session")
            previous_start = recording.start_media_seconds
        return self


def reviewed_dataset(settings: IntegrationSettings) -> tuple[Path, LabeledRecordings]:
    """Require review provenance and reject changed or missing confirmed footage."""
    manifest: Path | None = settings.dataset_manifest
    assert manifest is not None, "Set TEST_DATASET_MANIFEST after human clip review"
    dataset: LabeledRecordings = LabeledRecordings.model_validate_json(manifest.read_text())
    assert {entry.has_motion for entry in dataset.recordings} == {None, False, True}, (
        "Reviewed dataset must contain warm-up, motion and stillness"
    )
    for entry in dataset.recordings:
        source: Path = manifest.parent / entry.path
        assert hashlib.sha256(source.read_bytes()).hexdigest() == entry.sha256, str(source)
    return manifest, dataset


@pytest.mark.dataset
def test_chronological_motion_labels(integration_settings: IntegrationSettings) -> None:
    """The background model survives segment boundaries and matches camera labels."""
    manifest, dataset = reviewed_dataset(integration_settings)
    analyzer = core.SegmentMotionAnalyzer()
    observed_labels = set[bool | None]()
    for recording in dataset.recordings:
        source: Path = manifest.parent / recording.path
        result: core.MotionAnalysisResult = analyzer.analyze_segment_motion(
            source, recording.continuity_id, recording.start_media_seconds
        )
        assert result.status.name == recording.expected_status, str(source)
        assert result.has_motion == recording.has_motion, str(source)
        observed_labels.add(result.has_motion)
    assert observed_labels == {None, False, True}, (
        "Dataset must cover warm-up, motion and stillness"
    )


@pytest.mark.dataset
def test_reviewed_media_deleted_only_after_valid_negative(
    integration_settings: IntegrationSettings,
    state_driver: Path,
    tmp_path: Path,
) -> None:
    """The real analyzer authorizes deletion only for reviewed valid stillness clips."""
    manifest, dataset = reviewed_dataset(integration_settings)
    for index, recording in enumerate(dataset.recordings):
        source: Path = (manifest.parent / recording.path).resolve()
        probe = probe_media(source)
        subprocess.run(
            [
                str(state_driver),
                "queue_media",
                str(tmp_path / "stage"),
                str(source),
                f"clip-{index}",
                recording.continuity_id,
                str(recording.start_media_seconds),
                str(probe.format.duration),
                probe.streams[0].codec_name,
            ],
            check=True,
            timeout=30,
        )
    subprocess.run(
        [
            str(state_driver),
            "process_media",
            str(tmp_path / "stage"),
            str(len(dataset.recordings)),
        ],
        check=True,
        timeout=120,
    )
    config = core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(
            camera_name="camera", rtsp_url="rtsp://127.0.0.1:1/unavailable"
        ),
        storage=core.LocalDirectoryStorageConfig(root_directory=tmp_path / "stage/stored"),
        staging_directory=tmp_path / "stage",
    )
    recorder = core.SegmentRecorder(config)
    latest: dict[str, core.CompletedSegment] = {
        event.completed_segment.segment.segment_id: event.completed_segment
        for event in recorder.take_recorder_events()
    }
    assert len(latest) == len(dataset.recordings)
    for index, recording in enumerate(dataset.recordings):
        result: core.CompletedSegment = latest[f"clip-{index}"]
        assert result.motion is not None
        assert result.motion.status.name == recording.expected_status
        assert result.motion.has_motion == recording.has_motion
        negative: bool = recording.expected_status == "valid" and recording.has_motion is False
        assert (
            result.storage_outcome == core.SegmentStorageOutcome.deleted_without_motion
        ) == negative
        surviving: list[Path] = list((tmp_path / "stage").rglob(f"clip-{index}.*"))
        assert bool(surviving) != negative


@pytest.mark.dataset
def test_sampling_owns_pixels_and_reset_preserves_unknown(
    integration_settings: IntegrationSettings,
) -> None:
    """Actual camera frames remain valid after C++ temporaries die; reset is conservative."""
    source: Path | None = integration_settings.segment
    assert source is not None, "Set TEST_SEGMENT to actual camera footage"
    frames: np.ndarray[tuple[int, ...], np.dtype[np.uint8]] = core.sample_clip_frames_from_segment(
        source, frame_count=4, frame_width=64, frame_height=48
    )
    assert frames.shape == (4, 48, 64, 3)
    assert frames.dtype == np.uint8
    assert frames.base is not None
    assert frames.flags.c_contiguous
    assert int(frames.max()) > int(frames.min())
    params = core.MotionAnalysisParams(warmup_seconds=3600, decode_timeout_seconds=30)
    analyzer = core.SegmentMotionAnalyzer(params)
    result: core.MotionAnalysisResult = analyzer.analyze_segment_motion(source, "connection_a")
    assert result.status == core.MotionAnalysisStatus.warming_up
    assert result.has_motion is None
    analyzer.reset()
    assert analyzer.analyze_segment_motion(source, "connection_b").has_motion is None


@pytest.mark.gpu
def test_gpu_sampling(integration_settings: IntegrationSettings) -> None:
    """Exercise hardware-enabled decoder selection on actual camera media."""
    source: Path | None = integration_settings.segment
    assert source is not None, "Set TEST_SEGMENT to actual camera footage"
    frames: np.ndarray[tuple[int, ...], np.dtype[np.uint8]] = core.sample_clip_frames_from_segment(
        source, 4, 64, 48, use_gpu_decoding=True
    )
    assert frames.shape == (4, 48, 64, 3)


@pytest.mark.dataset
def test_native_media_memory(
    state_driver: Path,
    tmp_path: Path,
    integration_settings: IntegrationSettings,
) -> None:
    """Exercise native decoding and buffer lifetimes with the selected sanitizer preset."""
    assert integration_settings.segment is not None, "Set TEST_SEGMENT to actual camera footage"
    environment = dict(os.environ)
    environment["TEST_SEGMENT"] = str(integration_settings.segment)
    subprocess.run(
        [str(state_driver), "media", str(tmp_path)],
        check=True,
        timeout=60,
        env=environment,
    )
