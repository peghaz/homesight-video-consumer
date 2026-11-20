"""Retain dependency-only TSan evidence without treating warnings as a safety pass."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from verification_services import sanitizer_environment

if TYPE_CHECKING:
    from conftest import IntegrationSettings


@pytest.mark.dataset
def test_dependency_only_tsan_diagnostic(
    tmp_path: Path,
    integration_settings: IntegrationSettings,
    pytestconfig: pytest.Config,
) -> None:
    """Reproduce opaque library handoffs without linking or including any HomeSight code."""
    if pytestconfig.getoption("--native-preset") != "tsan":
        pytest.skip("Diagnostic requires --native-preset tsan")
    source: Path | None = integration_settings.segment
    assert source is not None, "Set TEST_SEGMENT to actual camera footage"
    root: Path = Path(__file__).resolve().parents[1]
    (tmp_path / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.22)\nproject(DependencyProbe LANGUAGES CXX)\n"
        "find_package(PkgConfig REQUIRED)\n"
        "pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-app-1.0 gstreamer-video-1.0)\n"
        "find_package(OpenCV REQUIRED COMPONENTS core)\n"
        f'add_executable(probe "{root / "tests/dependency_probe.cpp"}")\n'
        "target_compile_features(probe PRIVATE cxx_std_20)\n"
        "target_include_directories(probe SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})\n"
        "target_link_libraries(probe PRIVATE PkgConfig::GST ${OpenCV_LIBS})\n"
        "target_compile_options(probe PRIVATE -fsanitize=thread -g)\n"
        "target_link_options(probe PRIVATE -fsanitize=thread)\n",
        encoding="utf-8",
    )
    subprocess.run(["cmake", "-S", str(tmp_path), "-B", str(tmp_path / "out")], check=True)
    subprocess.run(["cmake", "--build", str(tmp_path / "out"), "-j", "2"], check=True)
    environment: dict[str, str] = dict(os.environ)
    dependency_path: Path | None = integration_settings.tsan_library_path
    if dependency_path is not None:
        environment = sanitizer_environment(dependency_path, tmp_path)
    result: subprocess.CompletedProcess[str] = subprocess.run(
        [str(tmp_path / "out/probe"), str(source.resolve())],
        capture_output=True,
        text=True,
        timeout=90,
        env=environment,
    )
    suffix: str = "-instrumented" if dependency_path is not None else ""
    destination: Path = root / f"build/verification/tsan-dependency-only{suffix}.log"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode in {0, 66}, destination.read_text()[-4000:]
    print(f"Dependency diagnostic exit={result.returncode}; warnings are retained in {destination}")
