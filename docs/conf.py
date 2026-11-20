"""Sphinx configuration for the installed Python API and Doxygen C++ reference."""

import sys
import tomllib
from pathlib import Path
from typing import cast

DOCS_ROOT: Path = Path(__file__).resolve().parent
PROJECT_ROOT: Path = DOCS_ROOT.parent
metadata: dict[str, object] = tomllib.loads((PROJECT_ROOT / "pyproject.toml").read_text())
# Only documentation extensions are loaded from sources; the native package must be installed.
sys.path.insert(0, str(DOCS_ROOT))
project = "HomeSight Core"
project_metadata: dict[str, object] = cast(dict[str, object], metadata["project"])
release: str = str(project_metadata["version"])
version = release
extensions = [
    "myst_parser",
    "sphinxcontrib.mermaid",
    "sphinx.ext.autodoc",
    "sphinx.ext.napoleon",
    "breathe",
    "homesight_docs",
]
source_suffix = {".rst": "restructuredtext", ".md": "markdown"}
root_doc = "index"
exclude_patterns: list[str] = []
html_theme = "alabaster"
html_title = f"{project} {release}"
html_show_sourcelink = False
html_static_path = ["_static"]
html_css_files = ["diagrams.css"]
myst_heading_anchors = 3
myst_enable_extensions = ["colon_fence"]
myst_fence_as_directive = ["mermaid"]
# Pin the browser renderer; ordinary Mermaid fences also work in Markdown viewers.
mermaid_version = "11.12.1"
mermaid_height = "auto"
mermaid_light_theme = "neutral"
autodoc_default_options = {
    "members": True,
    "undoc-members": True,
    "imported-members": True,
    "member-order": "bysource",
    "special-members": "__init__,__enter__,__exit__",
}
autodoc_docstring_signature = False
# Sphinx 9's dynamic renderer indexes an empty signature list for native callables.
# The supported class-based renderer accepts signatures supplied by our stub hook.
autodoc_use_legacy_class_based = True
autodoc_typehints = "signature"
napoleon_google_docstring = True
napoleon_numpy_docstring = False
breathe_projects = {"HomeSightCore": str(PROJECT_ROOT / "build/docs/doxygen/xml")}
breathe_default_project = "HomeSightCore"
breathe_domain_by_extension = {"hpp": "cpp"}
