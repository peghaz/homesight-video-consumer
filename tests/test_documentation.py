"""Regression checks for native API signature extraction and docs prerequisites."""

from __future__ import annotations

import importlib
import runpy
import shutil
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import cast

import pytest


@dataclass(frozen=True)
class DocumentationTools:
    """Typed facade for the optional Sphinx extension's standard-library helpers."""

    signatures: Callable[[str], dict[str, tuple[str, str | None]]]
    fields: Callable[[str], dict[str, str]]
    prepare: Callable[[object], None]


@pytest.fixture
def documentation_tools() -> DocumentationTools:
    """Load helpers without requiring Sphinx in the ordinary development environment."""
    root: Path = Path(__file__).resolve().parents[1]
    namespace: dict[str, object] = runpy.run_path(str(root / "docs/homesight_docs.py"))
    return DocumentationTools(
        signatures=cast(
            Callable[[str], dict[str, tuple[str, str | None]]], namespace["stub_signatures"]
        ),
        fields=cast(Callable[[str], dict[str, str]], namespace["stub_field_types"]),
        prepare=cast(Callable[[object], None], namespace["__build_cpp"]),
    )


def test_stub_signatures_preserve_parameter_kinds_without_evaluation(
    documentation_tools: DocumentationTools,
) -> None:
    """Native signatures retain positional/keyword constraints and unevaluated types."""
    source = """
class API:
    def __init__(self, *, name: str = ...) -> None: ...
    def read(self, count: int, /, *, timeout: float = 1.5) -> bytes | None: ...
    @classmethod
    def create(cls, source: UnknownType) -> API: ...
def transform(value: MissingType, *args: int, **kwargs: str) -> list[int]: ...
"""
    signatures: dict[str, tuple[str, str | None]] = documentation_tools.signatures(source)
    assert signatures["API"] == ("(*, name: str=...)", None)
    assert signatures["API.read"] == ("(count: int, /, *, timeout: float=1.5)", "bytes | None")
    assert signatures["API.create"] == ("(source: UnknownType)", "API")
    assert signatures["transform"] == (
        "(value: MissingType, *args: int, **kwargs: str)",
        "list[int]",
    )


def test_public_stub_configuration_and_property_types(
    documentation_tools: DocumentationTools,
) -> None:
    """The checked-in kwargs constructors and read-only fields stay documentable."""
    root: Path = Path(__file__).resolve().parents[1]
    source: str = (root / "src/homesight_core/_native.pyi").read_text()
    signatures: dict[str, tuple[str, str | None]] = documentation_tools.signatures(source)
    config: str = signatures["SegmentRecorderConfig"][0]
    assert config.startswith("(*,") and "staging_directory: FilePath" in config
    assert "kwargs" not in config and "self" not in config
    assert signatures["S3StorageConfig.to_dict"] == ("()", "dict[str, object]")
    assert signatures["S3StorageConfig.from_dict"][0] == "(values: dict[str, object], /)"
    assert signatures["sample_clip_frames_from_segment"][1] == "NDArray[np.uint8]"
    fields: dict[str, str] = documentation_tools.fields(source)
    assert fields["RecorderStatistics.state"] == "RecorderState"
    assert fields["MotionAnalysisResult.has_motion"] == "bool | None"
    assert fields["SegmentRecorderConfig.local_video_allowance_bytes"] == "int"


@pytest.mark.parametrize("missing", ["native", "doxygen"])
def test_documentation_reports_missing_prerequisites(
    documentation_tools: DocumentationTools,
    monkeypatch: pytest.MonkeyPatch,
    missing: str,
) -> None:
    """Missing native imports or Doxygen fail before generation with actionable messages."""
    with monkeypatch.context() as patch:
        if missing == "native":
            original: Callable[..., object] = importlib.import_module

            def missing_native(name: str, package: str | None = None) -> object:
                """Simulate an unavailable extension while preserving unrelated imports."""
                if name == "homesight_core":
                    raise ImportError("native library missing")
                return original(name, package)

            patch.setattr(importlib, "import_module", missing_native)
            message = "uv sync --group docs"
        else:
            patch.setattr(shutil, "which", lambda command: None)
            message = "Install Doxygen"
        with pytest.raises(RuntimeError, match=message):
            documentation_tools.prepare(None)
