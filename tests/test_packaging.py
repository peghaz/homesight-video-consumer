"""Verify distributions and consumers without source-tree include/import paths."""

import os
import shutil
import subprocess
import tarfile
import zipfile
from pathlib import Path, PurePosixPath

import pytest


def run_build(command: list[str], directory: Path) -> None:
    """Run a build with useful failure output and bounded lifetime."""
    subprocess.run(command, cwd=directory, check=True, timeout=600)


def assert_archive_clean(names: list[str]) -> None:
    """Reject secret files, private environment state and generated build products."""
    forbidden = {
        ".git",
        ".aws",
        ".codex",
        ".agents",
        ".venv",
        ".vscode",
        "__pycache__",
        ".pytest_cache",
        ".mypy_cache",
        ".ruff_cache",
        "build",
        "dist",
        "wheels",
    }
    for name in names:
        path = PurePosixPath(name)
        assert not (set(path.parts) & forbidden), name
        assert not (
            path.name == ".env" or (path.name.startswith(".env.") and path.name != ".env.example")
        ), name
        assert path.suffix not in {".pyc", ".mp4", ".mkv", ".sqlite3", ".rpm"}, name


def assert_installed_wheel(wheel: Path, directory: Path) -> None:
    """Install and exercise a wheel in a fresh interpreter outside the checkout."""
    environment: Path = directory / "clean"
    run_build(["uv", "venv", "--python", "3.12", str(environment)], directory)
    python: Path = environment / "bin/python"
    run_build(["uv", "pip", "install", "--python", str(python), str(wheel)], directory)
    smoke = (
        "import pathlib, homesight_core as core; "
        "package=pathlib.Path(core.__file__).parent; "
        "assert 'site-packages' in str(package); "
        "assert (package/'py.typed').is_file(); "
        "assert (package/'_native.pyi').is_file(); "
        "assert list((package/'lib').glob('libhomesight_core.so*')); "
        "result=core.SegmentMotionAnalyzer().analyze_segment_motion('absent.mp4'); "
        "assert result.has_motion is None; "
        "assert result.status == core.MotionAnalysisStatus.failed; "
        "assert core.__version__ == '0.1.0'"
    )
    subprocess.run([str(python), "-I", "-c", smoke], cwd=directory, check=True, timeout=30)


@pytest.mark.packaging
def test_clean_wheel_and_sdist_install(tmp_path: Path) -> None:
    """Rebuild the sdist outside the checkout and import both resulting wheels."""
    root: Path = Path(__file__).resolve().parents[1]
    artifacts: Path = tmp_path / "artifacts"
    run_build(["uv", "build", "--out-dir", str(artifacts)], root)
    wheels: list[Path] = list(artifacts.glob("*.whl"))
    sources: list[Path] = list(artifacts.glob("*.tar.gz"))
    assert len(wheels) == len(sources) == 1
    with zipfile.ZipFile(wheels[0]) as wheel:
        assert_archive_clean(wheel.namelist())
        assert any(name.endswith("homesight_core/py.typed") for name in wheel.namelist())
        assert any(name.endswith("homesight_core/_native.pyi") for name in wheel.namelist())
        assert any("homesight_core/_native." in name for name in wheel.namelist())
    with tarfile.open(sources[0]) as source:
        assert_archive_clean(source.getnames())
        assert not any(name.endswith((".so", ".a", ".o")) for name in source.getnames())
        source.extractall(tmp_path / "source", filter="data")
    assert_installed_wheel(wheels[0], tmp_path)
    extracted: Path = next((tmp_path / "source").iterdir())
    run_build(["uv", "build", "--wheel", "--out-dir", str(tmp_path / "rebuilt")], extracted)
    rebuilt: Path = next((tmp_path / "rebuilt").glob("*.whl"))
    second: Path = tmp_path / "second"
    second.mkdir()
    assert_installed_wheel(rebuilt, second)


@pytest.mark.packaging
@pytest.mark.parametrize("variant", ["debug", "release", "no-s3"])
def test_installed_public_cpp_consumer(tmp_path: Path, variant: str) -> None:
    """Build using only installed public headers and the exported target."""
    root: Path = Path(__file__).resolve().parents[1]
    native: Path = tmp_path / "native"
    prefix: Path = tmp_path / "prefix"
    run_build(
        [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(native),
            "-G",
            "Ninja",
            f"-DCMAKE_BUILD_TYPE={'Debug' if variant == 'debug' else 'Release'}",
            "-DHOMESIGHT_BUILD_PYTHON_BINDINGS=OFF",
            f"-DHOMESIGHT_ENABLE_S3_STORAGE={'OFF' if variant == 'no-s3' else 'ON'}",
        ],
        tmp_path,
    )
    run_build(["cmake", "--build", str(native), "-j", "4"], tmp_path)
    run_build(["cmake", "--install", str(native), "--prefix", str(prefix)], tmp_path)
    consumer: Path = tmp_path / "consumer"
    consumer.mkdir()
    shutil.copyfile(root / "tests/public_consumer.cpp", consumer / "main.cpp")
    lines = [
        "cmake_minimum_required(VERSION 3.22)",
        "project(PublicConsumer LANGUAGES CXX)",
        "find_package(HomesightCore CONFIG REQUIRED)",
        "add_executable(consumer main.cpp)",
        "target_link_libraries(consumer PRIVATE HomesightCore::homesight_core)",
    ]
    for header in sorted((prefix / "include/homesight").glob("*.hpp")):
        (consumer / f"{header.stem}.cpp").write_text(
            f"#include <homesight/{header.name}>\n", encoding="utf-8"
        )
        lines.extend(
            [
                f"add_library(header_{header.stem} OBJECT {header.stem}.cpp)",
                f"target_link_libraries(header_{header.stem} PRIVATE "
                "HomesightCore::homesight_core)",
            ]
        )
    (consumer / "CMakeLists.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    run_build(
        [
            "cmake",
            "-S",
            str(consumer),
            "-B",
            str(consumer / "out"),
            f"-DCMAKE_PREFIX_PATH={prefix}",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ],
        tmp_path,
    )
    run_build(["cmake", "--build", str(consumer / "out"), "-j", "2"], tmp_path)
    commands: str = (consumer / "out/compile_commands.json").read_text()
    assert str(root / "src") not in commands
    assert str(root / "include") not in commands
    environment: dict[str, str] = dict(os.environ)
    environment.pop("PYTHONPATH", None)
    subprocess.run(
        [str(consumer / "out/consumer"), str(tmp_path / "state")],
        cwd=consumer,
        env=environment,
        check=True,
        timeout=30,
    )
