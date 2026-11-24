"""Deterministic timing, ownership, and compatibility checks for frame sampling."""

from __future__ import annotations

import gc
import shutil
import subprocess
from collections import Counter
from collections.abc import Callable
from pathlib import Path

import numpy as np
import pytest
from numpy.typing import NDArray

import homesight_core as core

type ClipFactory = Callable[[str, int, str | None], Path]


@pytest.fixture(scope="module")
def sampling_clip(tmp_path_factory: pytest.TempPathFactory) -> ClipFactory:
    """Create cached, lossless unit-test clips with explicitly controlled timestamps."""
    executable: str | None = shutil.which("ffmpeg")
    if executable is None:
        pytest.skip("Install FFmpeg to run deterministic frame-sampling tests")
    directory: Path = tmp_path_factory.mktemp("frame-sampling")
    clips = dict[tuple[str, int, str | None], Path]()

    def create(duration: str, rate: int, selection: str | None) -> Path:
        """Generate RGB test patterns without a camera, network, or lossy encoding."""
        key: tuple[str, int, str | None] = (duration, rate, selection)
        if key not in clips:
            destination: Path = directory / f"clip-{len(clips)}.mkv"
            command: list[str] = [
                executable,
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                f"testsrc=size=32x24:rate={rate}:duration={duration}",
            ]
            if selection is not None:
                command.extend(["-vf", f"select='{selection}'", "-fps_mode", "vfr"])
            command.extend(["-c:v", "ffv1", "-pix_fmt", "bgr0", str(destination)])
            subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
            clips[key] = destination
        return clips[key]

    return create


@pytest.mark.parametrize(
    ("duration", "rate", "expected"),
    [
        ("2", 2, [0, 500, 1000, 1500]),
        ("2.3", 2, [0, 500, 1000, 1500, 2000]),
        ("2.3", 3, [0, 400, 700, 1000, 1400, 1700, 2000]),
        ("0.3", 2, [0]),
    ],
)
def test_sampling_rate_boundaries_and_partial_seconds(
    sampling_clip: ClipFactory, duration: str, rate: int, expected: list[int]
) -> None:
    """Samples respect clip-relative intervals and retain occupied final partial seconds."""
    source: Path = sampling_clip(duration, 10, None)
    frames: NDArray[np.uint8]
    timestamps: list[int]
    frames, timestamps = core.sample_clip_frames_from_segment_at_rate(source, rate, 32, 24)
    assert timestamps == expected
    assert frames.shape == (len(expected), 24, 32, 3)
    assert frames.dtype == np.uint8
    assert frames.flags.c_contiguous and frames.base is not None
    assert all(count <= rate for count in Counter(time // 1000 for time in timestamps).values())
    # Independently decode every RGB source frame and check the actual selected content.
    decoded: subprocess.CompletedProcess[bytes] = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(source), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        check=True,
        capture_output=True,
        timeout=30,
    )
    originals: NDArray[np.uint8] = np.frombuffer(decoded.stdout, dtype=np.uint8).reshape(
        -1, 24, 32, 3
    )
    np.testing.assert_array_equal(frames, originals[[time // 100 for time in expected]])


def test_sampling_rate_never_duplicates_sparse_source_frames(sampling_clip: ClipFactory) -> None:
    """A requested rate above the source rate returns each available frame once."""
    source: Path = sampling_clip("2.5", 2, None)
    frames: NDArray[np.uint8]
    timestamps: list[int]
    frames, timestamps = core.sample_clip_frames_from_segment_at_rate(source, 8, 32, 24)
    assert timestamps == [0, 500, 1000, 1500, 2000]
    assert frames.shape[0] == len(set(timestamps)) == 5


def test_sampling_rate_uses_variable_presentation_times(sampling_clip: ClipFactory) -> None:
    """Missing intervals remain empty instead of drawing frames from later intervals."""
    source: Path = sampling_clip(
        "2.3", 10, "eq(n,0)+eq(n,2)+eq(n,7)+eq(n,9)+eq(n,10)+eq(n,17)+eq(n,22)"
    )
    frames: NDArray[np.uint8]
    timestamps: list[int]
    frames, timestamps = core.sample_clip_frames_from_segment_at_rate(source, 2, 16, 12)
    assert timestamps == [0, 700, 1000, 1700, 2200]
    assert frames.shape == (5, 12, 16, 3)


def test_rate_samples_own_pixels_and_preserve_count_api(
    sampling_clip: ClipFactory, tmp_path: Path
) -> None:
    """Both APIs retain usable pixels after decoding and preserve the existing return type."""
    source: Path = tmp_path / "owned.mkv"
    shutil.copyfile(sampling_clip("2", 10, None), source)
    original_bytes: bytes = source.read_bytes()
    frames: NDArray[np.uint8]
    timestamps: list[int]
    frames, timestamps = core.sample_clip_frames_from_segment_at_rate(source, 2, 16, 9)
    saved: NDArray[np.uint8] = frames.copy()
    counted: NDArray[np.uint8] = core.sample_clip_frames_from_segment(source, 7, 16, 9)
    assert isinstance(counted, np.ndarray) and counted.shape == (7, 9, 16, 3)
    assert source.read_bytes() == original_bytes
    source.unlink()
    gc.collect()
    assert timestamps == [0, 500, 1000, 1500]
    np.testing.assert_array_equal(frames, saved)
    assert frames.flags.writeable
    frames[0, 0, 0] = [1, 2, 3]
    np.testing.assert_array_equal(frames[0, 0, 0], [1, 2, 3])


@pytest.mark.parametrize(
    ("rate", "width", "height", "timeout"),
    [
        (0, 32, 24, 10.0),
        (-1, 32, 24, 10.0),
        (2, 0, 24, 10.0),
        (2, 32, -1, 10.0),
        (2, 8193, 24, 10.0),
        (2, 32, 8193, 10.0),
        (2, 32, 24, 0.0),
        (2, 32, 24, float("nan")),
        (2, 32, 24, float("inf")),
        (2, 32, 24, 86401.0),
    ],
)
def test_sampling_rate_rejects_invalid_arguments_before_decode(
    tmp_path: Path, rate: int, width: int, height: int, timeout: float
) -> None:
    """Invalid requests fail before opening or allocating for an input recording."""
    with pytest.raises(core.InvalidConfigError):
        core.sample_clip_frames_from_segment_at_rate(
            tmp_path / "missing.mkv", rate, width, height, decode_timeout_seconds=timeout
        )


def test_sampling_errors_and_shared_allocation_limit(
    sampling_clip: ClipFactory, tmp_path: Path
) -> None:
    """Unreadable media, exhausted time, and oversized RGB requests raise explicit errors."""
    source: Path = tmp_path / "broken.mkv"
    source.write_bytes(b"not a recording")
    with pytest.raises(core.SegmentDecodingError):
        core.sample_clip_frames_from_segment_at_rate(source, 2, 32, 24)
    assert source.read_bytes() == b"not a recording"
    with pytest.raises(core.InvalidConfigError):
        core.sample_clip_frames_from_segment(source, 4096, 8192, 8192)
    with pytest.raises(core.SegmentDecodingError):
        core.sample_clip_frames_from_segment_at_rate(
            sampling_clip("2", 10, None), 2, 32, 24, decode_timeout_seconds=1e-9
        )


@pytest.mark.parametrize("rate", [2.5, "2", None])
def test_sampling_rate_requires_an_integer(tmp_path: Path, rate: object) -> None:
    """The Python binding rejects non-integer rates instead of coercing them."""
    with pytest.raises(TypeError):
        core.sample_clip_frames_from_segment_at_rate(
            tmp_path / "missing.mkv",
            rate,  # type: ignore[arg-type]
            32,
            24,
        )


@pytest.mark.parametrize("count", [4096, 4097])
def test_sampling_rate_enforces_actual_frame_limit(sampling_clip: ClipFactory, count: int) -> None:
    """The maximum output succeeds and additional decoded samples fail without truncation."""
    source: Path = sampling_clip(f"{count / 1000:.3f}", 1000, None)
    if count > 4096:
        with pytest.raises(core.InvalidConfigError, match="4096"):
            core.sample_clip_frames_from_segment_at_rate(
                source, 1000, 1, 1, decode_timeout_seconds=30
            )
    else:
        frames: NDArray[np.uint8]
        timestamps: list[int]
        frames, timestamps = core.sample_clip_frames_from_segment_at_rate(
            source, 1000, 1, 1, decode_timeout_seconds=30
        )
        assert frames.shape == (4096, 1, 1, 3)
        assert timestamps == list(range(4096))


def test_native_rate_sampling(state_driver: Path, sampling_clip: ClipFactory) -> None:
    """An installed C++ consumer exposes the same interval selection and owned output."""
    source: Path = sampling_clip("2.3", 10, None)
    result: subprocess.CompletedProcess[str] = subprocess.run(
        [str(state_driver), "sample_rate", str(source), "2"],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert [int(value) for value in result.stdout.split()] == [0, 500, 1000, 1500, 2000]
