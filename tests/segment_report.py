"""Live terminal reporting for native segment motion and storage events."""

from __future__ import annotations

import math
import re
import time
from types import TracebackType
from typing import Literal

from rich import box
from rich.console import Console, Group
from rich.live import Live
from rich.table import Table
from rich.text import Text

import homesight_core as core

type ObjectState = Literal["Verified", "Deleted", "Kept", "Cleanup failed"]


class SegmentReport:
    """Keep one current row per segment and leave its final result visible."""

    def __init__(
        self,
        prefix: str,
        *,
        keep_objects: bool = False,
        params: core.MotionAnalysisParams | None = None,
        console: Console | None = None,
    ) -> None:
        """Configure a display without opening a camera or storage connection."""
        self.__console = console if console is not None else Console()
        self.__prefix = prefix
        self.__keep_objects = keep_objects
        self.__params = params if params is not None else core.MotionAnalysisParams()
        self.__segments = dict[str, core.CompletedSegment]()
        self.__notified = set[str]()
        self.__uris = set[str]()
        self.__object_states = dict[str, ObjectState]()
        self.__statistics: core.RecorderStatistics | None = None
        self.__phase = "Waiting for segments"
        self.__started: float = time.monotonic()
        self.__live: Live | None = None

    def __enter__(self) -> SegmentReport:
        """Start an interactive display, or defer a static table for redirected output."""
        self.__console.print()
        self.__console.print(Text(f"S3 destination: {self.__prefix}"), soft_wrap=True)
        policy = "kept for inspection" if self.__keep_objects else "deleted after verification"
        self.__console.print(Text(f"Uploaded test objects will be {policy}."))
        self.__live = Live(
            self.render(), console=self.__console, auto_refresh=False, transient=False
        )
        self.__live.start()
        return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        """Restore the terminal and preserve the table even when verification fails."""
        self.__phase = "Failed" if exception_type is not None else "Complete"
        if self.__live is not None:
            try:
                self.refresh()
            finally:
                self.__live.stop()
                self.__live = None
        self.__console.print()
        for uri in sorted(self.__uris):
            self.__console.print(
                Text(f"S3 object [{self.__object_states.get(uri, 'Uploaded')}]: {uri}"),
                soft_wrap=True,
            )

    def record(self, completed: core.CompletedSegment) -> None:
        """Apply newer revisions and announce confirmed motion once per segment."""
        if completed.storage_uri is not None:
            self.__uris.add(completed.storage_uri)
        identity: str = completed.segment.segment_id
        previous: core.CompletedSegment | None = self.__segments.get(identity)
        if previous is not None and previous.state_revision >= completed.state_revision:
            return
        self.__segments[identity] = completed
        motion: core.MotionAnalysisResult | None = completed.motion
        if (
            motion is not None
            and motion.status == core.MotionAnalysisStatus.valid
            and motion.has_motion is True
            and identity not in self.__notified
        ):
            self.__notified.add(identity)
            self.__console.print(
                Text(
                    f"Motion detected: {self.__filename(completed)} "
                    f"({motion.frames_with_motion}/{motion.frames_analyzed} analyzed frames)",
                    style="bold red",
                )
            )
        self.refresh()

    def set_object_state(self, uri: str, state: ObjectState) -> None:
        """Report verified downloads or the actual retention/cleanup outcome."""
        self.__uris.add(uri)
        self.__object_states[uri] = state
        self.refresh()

    def refresh(
        self, statistics: core.RecorderStatistics | None = None, *, phase: str | None = None
    ) -> None:
        """Refresh elapsed time and counters without a background rendering thread."""
        if statistics is not None:
            self.__statistics = statistics
            if phase is None:
                self.__phase = statistics.state.name.capitalize()
        if phase is not None:
            self.__phase = phase
        if self.__live is not None:
            self.__live.update(self.render(), refresh=True)

    def render(self) -> Group:
        """Build a table with explicit unknown states and trustworthy area metrics."""
        table = Table(title="Camera segments · motion and S3", box=box.SIMPLE_HEAVY, expand=True)
        table.add_column("Segment", overflow="fold", ratio=3)
        table.add_column("Duration", justify="right")
        table.add_column("Analysis")
        table.add_column("Motion")
        table.add_column("Peak", justify="right")
        table.add_column("Mean", justify="right")
        table.add_column("Frames", justify="right")
        table.add_column("S3")
        details = list[Text]()
        if not self.__segments:
            table.add_row(
                "Waiting for first completed segment…",
                "—",
                "Waiting",
                "Unknown",
                "—",
                "—",
                "—",
                "—",
            )
        for completed in self.__segments.values():
            motion: core.MotionAnalysisResult | None = completed.motion
            status = Text("Unavailable", style="dim")
            detected = Text("Unknown", style="yellow")
            peak = mean = frames = "—"
            if motion is not None:
                status = Text(
                    motion.status.name,
                    style="green" if motion.status == core.MotionAnalysisStatus.valid else "yellow",
                )
                frames = f"{motion.frames_with_motion}/{motion.frames_analyzed}"
                if motion.status == core.MotionAnalysisStatus.failed:
                    status.stylize("red")
                if (
                    motion.status == core.MotionAnalysisStatus.valid
                    and motion.has_motion is not None
                ):
                    detected = Text(
                        "Yes" if motion.has_motion else "No",
                        style="bold red" if motion.has_motion else "green",
                    )
                if (
                    motion.status
                    in {core.MotionAnalysisStatus.valid, core.MotionAnalysisStatus.warming_up}
                    and motion.frames_analyzed > 0
                ):
                    peak = self.__percentage(motion.peak_changed_area)
                    mean = self.__percentage(motion.mean_changed_area)
            uri: str | None = completed.storage_uri
            storage = completed.storage_outcome.name.replace("_", " ")
            if uri is not None and uri in self.__object_states:
                storage = self.__object_states[uri]
            elif completed.storage_outcome == core.SegmentStorageOutcome.stored:
                storage = "Uploaded"
            table.add_row(
                Text(self.__filename(completed)),
                f"{completed.segment.duration_seconds:.2f}s",
                status,
                detected,
                peak,
                mean,
                frames,
                Text(storage),
            )
            errors: set[str] = {
                error
                for error in (
                    completed.motion_analysis_error,
                    motion.error if motion is not None else None,
                    completed.storage_error,
                )
                if error
            }
            for error in sorted(errors):
                redacted: str = re.sub(r"rtsps?://[^\s<>]+", "rtsp://<redacted>", error)
                details.append(Text(f"{self.__filename(completed)}: {redacted}", style="red"))
        elapsed: float = time.monotonic() - self.__started
        counters = ""
        if self.__statistics is not None:
            stats = self.__statistics
            if stats.last_error:
                error = re.sub(r"rtsps?://[^\s<>]+", "rtsp://<redacted>", stats.last_error)
                details.append(Text(f"Recorder: {error}", style="red"))
            counters = (
                f" · recorder={stats.state.name} · written={stats.segments_written}"
                f" · analyzed={stats.segments_analyzed} · uploaded={stats.segments_stored}"
                f" · pending={stats.segments_waiting_for_upload}"
                f" · upload failures={stats.storage_failures}"
                f" · reconnects={stats.reconnect_attempts}"
            )
        table.caption = Text(
            f"{self.__phase} · elapsed={elapsed:.1f}s{counters}\n"
            f"Warm-up {self.__params.warmup_seconds:g}s · "
            f"sampling {self.__params.sample_rate_fps:g} fps · "
            f"changed-area threshold {self.__params.changed_area_threshold:.1%} · "
            f"minimum {self.__params.minimum_frames_with_motion} changed frames"
        )
        return Group(table, *details)

    @staticmethod
    def __filename(completed: core.CompletedSegment) -> str:
        """Use the original filename even after its staging file has been removed."""
        return completed.segment.staging_file_path.name or completed.segment.segment_id

    @staticmethod
    def __percentage(value: float) -> str:
        """Do not turn missing or invalid analyzer metrics into reassuring zeros."""
        return f"{value:.2%}" if math.isfinite(value) and 0 <= value <= 1 else "—"
