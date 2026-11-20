"""Build public API references from installed bindings, typing stubs, and Doxygen."""

from __future__ import annotations

import ast
import importlib
import os
import re
import shutil
import subprocess
from pathlib import Path
from types import ModuleType
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from sphinx.application import Sphinx

PROJECT_ROOT: Path = Path(__file__).resolve().parents[1]


def __track_python_inputs(app: Sphinx) -> None:
    """Invalidate cached API pages when stubs, bindings, or the installed extension change."""
    for relative in (
        "src/homesight_core/_native.pyi",
        "src/homesight_core/__init__.py",
        "python/bindings.cpp",
        "docs/homesight_docs.py",
    ):
        app.env.note_dependency(PROJECT_ROOT / relative)
    native: ModuleType = importlib.import_module("homesight_core._native")
    if native.__file__ is not None:
        app.env.note_dependency(native.__file__)


def stub_signatures(source: str) -> dict[str, tuple[str, str | None]]:
    """Read declared signatures without importing or evaluating stub expressions.

    Constructors expose keyword fields instead of the native generic kwargs.
    Ellipsis defaults stay explicit because stubs do not encode their values;
    documented field descriptions provide the actual native defaults.
    """
    module: ast.Module = ast.parse(source)
    signatures = dict[str, tuple[str, str | None]]()

    def __visit(function: ast.FunctionDef, owner: str = "") -> None:
        """Index one declaration, omitting bound self/cls parameters."""
        arguments: ast.arguments = function.args
        bound: list[ast.arg] = arguments.posonlyargs + arguments.args
        if owner and bound and bound[0].arg in {"self", "cls"}:
            if arguments.posonlyargs:
                arguments.posonlyargs.pop(0)
            else:
                arguments.args.pop(0)
        if owner == "_Value" and function.name == "from_dict":
            # This shared pybind static helper has no named py::arg declaration.
            arguments.posonlyargs.extend(arguments.args)
            arguments.args.clear()
        signature: str = "(" + ast.unparse(arguments) + ")"
        result: str | None = ast.unparse(function.returns) if function.returns else None
        name: str = f"{owner}.{function.name}" if owner else function.name
        signatures[name] = (signature, result)
        if function.name == "__init__":
            signatures[owner] = (signature, None)

    for node in module.body:
        if isinstance(node, ast.FunctionDef):
            __visit(node)
        elif isinstance(node, ast.ClassDef):
            for member in node.body:
                if isinstance(member, ast.FunctionDef):
                    __visit(member, node.name)
    # Value classes share conversion methods at runtime; their stubs inherit _Value.
    for node in module.body:
        if isinstance(node, ast.ClassDef) and any(
            isinstance(base, ast.Name) and base.id == "_Value" for base in node.bases
        ):
            for name in ("to_dict", "from_dict"):
                signatures[f"{node.name}.{name}"] = signatures[f"_Value.{name}"]
    return signatures


def stub_field_types(source: str) -> dict[str, str]:
    """Read writable fields and read-only properties from the public stub classes."""
    result = dict[str, str]()
    for node in ast.parse(source).body:
        if not isinstance(node, ast.ClassDef):
            continue
        for member in node.body:
            if isinstance(member, ast.AnnAssign) and isinstance(member.target, ast.Name):
                result[f"{node.name}.{member.target.id}"] = ast.unparse(member.annotation)
            elif (
                isinstance(member, ast.FunctionDef)
                and member.returns is not None
                and any(
                    isinstance(item, ast.Name) and item.id == "property"
                    for item in member.decorator_list
                )
            ):
                result[f"{node.name}.{member.name}"] = ast.unparse(member.returns)
    return result


def __process_signature(
    app: Sphinx,
    what: str,
    name: str,
    obj: object,
    options: object,
    signature: str | None,
    return_annotation: str | None,
) -> tuple[str, str | None] | None:
    """Use the checked-in public typing surface for native Python signatures."""
    del obj, options, signature, return_annotation
    if what not in {"class", "function", "method"} or not name.startswith("homesight_core."):
        return None
    __track_python_inputs(app)
    relative: str = name.removeprefix("homesight_core.").removeprefix("_native.")
    return stub_signatures((PROJECT_ROOT / "src/homesight_core/_native.pyi").read_text()).get(
        relative
    )


def __process_docstring(
    app: Sphinx, what: str, name: str, obj: object, options: object, lines: list[str]
) -> None:
    """Strip pybind's runtime signature line before Napoleon processes the prose."""
    del obj, options
    if not name.startswith("homesight_core.") or not lines:
        return
    __track_python_inputs(app)
    if re.match(r"^\w+\(.*\)\s*->", lines[0]):
        del lines[0]
        if lines and not lines[0].strip():
            del lines[0]
    if what in {"attribute", "property"}:
        relative: str = name.removeprefix("homesight_core.").removeprefix("_native.")
        annotation: str | None = stub_field_types(
            (PROJECT_ROOT / "src/homesight_core/_native.pyi").read_text()
        ).get(relative)
        if annotation is not None:
            lines.extend(["", f":type: ``{annotation}``", ""])


def __build_cpp(app: Sphinx) -> None:
    """Require real prerequisites and refresh Doxygen XML before Sphinx reads pages."""
    try:
        importlib.import_module("homesight_core")
    except ImportError as error:
        raise RuntimeError(
            "Install homesight-core with its native dependencies before building documentation: "
            "uv sync --group docs"
        ) from error
    executable: str | None = shutil.which("doxygen")
    if executable is None:
        raise RuntimeError(
            "Install Doxygen before building documentation (see docs/development.md)."
        )
    output: Path = PROJECT_ROOT / "build/docs"
    output.mkdir(parents=True, exist_ok=True)
    environment: dict[str, str] = dict(os.environ)
    environment["HOMESIGHT_DOCS_VERSION"] = str(app.config.release)
    subprocess.run(
        [executable, str(PROJECT_ROOT / "docs/Doxyfile")],
        cwd=PROJECT_ROOT,
        env=environment,
        check=True,
        timeout=120,
    )


def setup(app: Sphinx) -> dict[str, object]:
    """Register Sphinx build and autodoc hooks without modifying the public package."""
    app.connect("builder-inited", __build_cpp)
    app.connect("autodoc-process-signature", __process_signature)
    app.connect("autodoc-process-docstring", __process_docstring, priority=100)
    return {"version": "1", "parallel_read_safe": True, "parallel_write_safe": True}
