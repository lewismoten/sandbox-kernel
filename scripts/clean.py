# SPDX-License-Identifier: MIT
"""Remove only this project's generated build directory."""
import pathlib, shutil
root=pathlib.Path(__file__).resolve().parents[1]
shutil.rmtree(root/'build',ignore_errors=True)
