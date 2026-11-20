"""Motion reporting semantics, revision handling and pytest output visibility."""

from __future__ import annotations

import os
import subprocess
import sys
from contextlib import nullcontext
from io import StringIO
from pathlib import Path

import pytest
from rich.console import Console
from segment_report import ObjectState, SegmentReport

import homesight_core as core


def __segment(
    *,
    revision: int = 1,
    status: core.MotionAnalysisStatus | None = core.MotionAnalysisStatus.valid,
    detected: bool | None = True,
    frames: int = 20,
    filename: str = "observed.mp4",
    uploaded: bool = False,
) -> core.CompletedSegment:
    """Build native metadata values without generating video or opening connections."""
    motion: core.MotionAnalysisResult | None = None
    if status is not None:
        motion = core.MotionAnalysisResult(
            status=status,
            has_motion=detected,
            peak_changed_area=0.125,
            mean_changed_area=0.03,
            frames_with_motion=3 if frames else 0,
            frames_analyzed=frames,
            error="Decoder could not cover the segment"
            if status == core.MotionAnalysisStatus.failed
            else None,
        )
    return core.CompletedSegment(
        segment=core.SegmentInfo(
            segment_id="observed",
            camera_name="camera",
            staging_file_path=Path("/stage") / filename,
            duration_seconds=10,
        ),
        motion=motion,
        state_revision=revision,
        storage_outcome=core.SegmentStorageOutcome.stored
        if uploaded
        else core.SegmentStorageOutcome.pending,
        storage_uri="s3://bucket/run/observed.mp4" if uploaded else None,
    )


def __render(report: SegmentReport, width: int = 180) -> str:
    """Render through Rich's real non-terminal console at a fixed test width."""
    output = StringIO()
    Console(file=output, width=width, color_system=None, force_terminal=False).print(
        report.render()
    )
    return output.getvalue()


@pytest.mark.parametrize(
    ("status", "detected", "label", "classification", "metrics"),
    [
        (core.MotionAnalysisStatus.valid, True, "valid", "Yes", True),
        (core.MotionAnalysisStatus.valid, False, "valid", "No", True),
        (core.MotionAnalysisStatus.warming_up, None, "warming_up", "Unknown", True),
        (core.MotionAnalysisStatus.warming_up, True, "warming_up", "Unknown", True),
        (core.MotionAnalysisStatus.insufficient_data, None, "insufficient_data", "Unknown", False),
        (core.MotionAnalysisStatus.failed, None, "failed", "Unknown", False),
        (None, None, "Unavailable", "Unknown", False),
    ],
)
def test_motion_results_are_explicit(
    status: core.MotionAnalysisStatus | None,
    detected: bool | None,
    label: str,
    classification: str,
    metrics: bool,
) -> None:
    """Unknown statuses never become no-motion, and incomplete area values stay hidden."""
    report = SegmentReport("s3://bucket/run/", console=Console(file=StringIO()))
    report.record(__segment(status=status, detected=detected))
    text: str = __render(report)
    assert "observed.mp4" in text
    assert "10.00s" in text
    assert label in text
    assert classification in text.split("Warm-up")[0]
    assert ("12.50%" in text) is metrics
    assert ("3.00%" in text) is metrics
    if status is not None:
        assert "3/20" in text
    if status == core.MotionAnalysisStatus.failed:
        assert "Decoder could not cover the segment" in text


def test_no_analyzed_frames_does_not_show_zero_percent() -> None:
    """A result with no observed frames cannot imply a measured zero changed area."""
    report = SegmentReport("s3://bucket/run/")
    report.record(__segment(status=core.MotionAnalysisStatus.warming_up, detected=None, frames=0))
    text: str = __render(report)
    assert "0/0" in text
    assert "12.50%" not in text
    assert "3.00%" not in text


def test_old_and_duplicate_revisions_cannot_regress_rows() -> None:
    """An old motion event cannot overwrite an uploaded revision or trigger a notice."""
    output = StringIO()
    report = SegmentReport("s3://bucket/run/", console=Console(file=output))
    newer: core.CompletedSegment = __segment(revision=2, detected=False, uploaded=True)
    report.record(newer)
    report.record(__segment(revision=1, detected=True))
    report.record(newer)
    text: str = __render(report)
    assert text.count("observed.mp4") == 1
    assert "Uploaded" in text
    assert "No" in text.split("Warm-up")[0]
    assert "Motion detected:" not in output.getvalue()


def test_motion_notice_is_emitted_once_per_segment() -> None:
    """Storage revisions do not repeat a confirmed motion notification."""
    output = StringIO()
    report = SegmentReport("s3://bucket/run/", console=Console(file=output))
    report.record(__segment())
    report.record(__segment(revision=2, uploaded=True))
    report.record(__segment(revision=2, uploaded=True))
    assert output.getvalue().count("Motion detected: observed.mp4") == 1
    assert __render(report).count("observed.mp4") == 1


@pytest.mark.parametrize("state", ["Verified", "Kept", "Deleted", "Cleanup failed"])
def test_object_disposition_is_visible(state: ObjectState) -> None:
    """Rows show actual verification and retention outcomes rather than just uploaded."""
    report = SegmentReport("s3://bucket/run/", console=Console(file=StringIO()))
    report.record(__segment(uploaded=True))
    report.set_object_state("s3://bucket/run/observed.mp4", state)
    assert state in __render(report)


def test_narrow_output_wraps_filenames_without_terminal_codes() -> None:
    """Redirected output remains readable at a narrow width and preserves filename ends."""
    report = SegmentReport("s3://bucket/run/", console=Console(file=StringIO()))
    filename = "camera_" + "long_segment_name_" * 4 + ".mp4"
    report.record(__segment(filename=filename))
    text: str = __render(report, width=80)
    assert "camera_" in text
    assert filename in __render(report, width=250)
    assert "…" not in text
    assert "\x1b" not in text
    assert all(len(line) <= 80 for line in text.splitlines())


def test_error_details_are_literal_and_redact_camera_urls() -> None:
    """Rich markup in metadata is literal, and errors cannot reveal credential-bearing URLs."""
    report = SegmentReport("s3://bucket/run/")
    completed: core.CompletedSegment = __segment(
        status=core.MotionAnalysisStatus.failed, detected=None, filename="[camera].mp4"
    )
    completed.motion_analysis_error = "Cannot read rtsp://private:secret@camera/live [details]"
    report.record(completed)
    text: str = __render(report)
    assert "[camera].mp4" in text
    assert "[details]" in text
    assert "rtsp://<redacted>" in text
    assert "private:secret" not in text


@pytest.mark.parametrize("fail", [False, True])
def test_interactive_display_restores_cursor(monkeypatch: pytest.MonkeyPatch, fail: bool) -> None:
    """Rich refreshes an interactive table and restores the cursor even on failure."""
    monkeypatch.setenv("TERM", "xterm-256color")
    output = StringIO()
    console = Console(file=output, force_terminal=True, width=160, color_system=None)
    report = SegmentReport("s3://bucket/run/", console=console)
    with pytest.raises(ValueError, match="verification failed") if fail else nullcontext(), report:
        report.record(__segment())
        report.record(__segment(revision=2, uploaded=True))
        if fail:
            raise ValueError("verification failed")
    text: str = output.getvalue()
    assert "\x1b[?25l" in text
    assert "\x1b[?25h" in text
    assert "observed.mp4" in text
    assert text.count("Motion detected: observed.mp4") == 1
    assert ("Failed" if fail else "Complete") in text


@pytest.mark.parametrize("fail", [False, True])
def test_final_table_is_visible_under_pytest_capture(tmp_path: Path, fail: bool) -> None:
    """The ordinary pytest command shows the final table on both success and failure."""
    probe: Path = tmp_path / "test_visible.py"
    probe.write_text(
        "from pathlib import Path\n"
        "import pytest\n"
        "import homesight_core as core\n"
        "from segment_report import SegmentReport\n"
        "def test_report(capsys: pytest.CaptureFixture[str]) -> None:\n"
        "    with capsys.disabled(), SegmentReport(\n"
        '        "s3://bucket/inspection/", keep_objects=True\n'
        "    ) as report:\n"
        "        report.record(core.CompletedSegment(segment=core.SegmentInfo(\n"
        '            segment_id="visible", staging_file_path=Path("visible.mp4"),\n'
        "            duration_seconds=10),\n"
        '            state_revision=1, storage_uri="s3://bucket/inspection/visible.mp4"))\n'
        '        report.set_object_state("s3://bucket/inspection/visible.mp4", "Kept")\n'
        f'        assert {not fail!r}, "deliberate verification failure"\n',
        encoding="utf-8",
    )
    environment = dict(os.environ)
    environment["PYTHONPATH"] = str(Path(__file__).resolve().parent)
    environment["COLUMNS"] = "180"
    result: subprocess.CompletedProcess[str] = subprocess.run(
        [sys.executable, "-m", "pytest", "-v", str(probe), "--confcutdir", str(tmp_path)],
        cwd=tmp_path,
        env=environment,
        capture_output=True,
        text=True,
        timeout=15,
    )
    assert result.returncode == (1 if fail else 0), result.stdout + result.stderr
    assert "Camera segments" in result.stdout
    assert "visible.mp4" in result.stdout
    assert "S3 object [Kept]: s3://bucket/inspection/visible.mp4" in result.stdout
    assert ("Failed" if fail else "Complete") in result.stdout
    assert "\x1b" not in result.stdout
