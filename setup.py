import os
import pathlib
import shutil
import subprocess
import sys
import sysconfig

from setuptools import Extension, find_packages, setup
from setuptools.command.build_py import build_py as _build_py
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


ROOT = pathlib.Path(__file__).resolve().parent
PACKAGE_DIR = ROOT / "hft_backtest"
PACKAGE_WORK_DIR = PACKAGE_DIR / "dist" / ".tmp_build"
SETUPTOOLS_BUILD_DIR = PACKAGE_WORK_DIR / "setuptools"
CMAKE_BUILD_DIR = PACKAGE_WORK_DIR / "cmake"
DIST_DIR = PACKAGE_DIR / "dist"


def _python_library():
    libdir = sysconfig.get_config_var("LIBDIR") or ""
    candidates = [
        sysconfig.get_config_var("LDLIBRARY") or "",
        sysconfig.get_config_var("LIBRARY") or "",
        f"libpython{sys.version_info.major}.{sys.version_info.minor}.so",
        f"libpython{sys.version_info.major}.{sys.version_info.minor}.dylib",
        f"libpython{sys.version_info.major}.{sys.version_info.minor}.a",
    ]
    if libdir:
        for libname in candidates:
            if not libname:
                continue
            path = os.path.join(libdir, libname)
            if os.path.exists(path):
                return path
    return ""


def _clean_package_artifacts():
    for path in PACKAGE_DIR.glob("_core*.so"):
        path.unlink(missing_ok=True)

    lib_dir = PACKAGE_DIR / "lib"
    if lib_dir.exists():
        for path in lib_dir.glob("*.so*"):
            path.unlink(missing_ok=True)
        if not any(lib_dir.iterdir()):
            lib_dir.rmdir()


def _clean_packaging_workdirs():
    for path in (
        CMAKE_BUILD_DIR,
        SETUPTOOLS_BUILD_DIR,
        PACKAGE_WORK_DIR / "egg-info",
        ROOT / "hft_backtest.egg-info",
    ):
        if path.exists():
            shutil.rmtree(path)
    if PACKAGE_WORK_DIR.exists():
        shutil.rmtree(PACKAGE_WORK_DIR, ignore_errors=True)


def _stage_package_artifacts(build_lib):
    build_package_dir = pathlib.Path(build_lib) / "hft_backtest"
    build_package_dir.mkdir(parents=True, exist_ok=True)

    for path in PACKAGE_DIR.glob("_core*.so"):
        shutil.copy2(path, build_package_dir / path.name)

    source_lib_dir = PACKAGE_DIR / "lib"
    if source_lib_dir.exists():
        target_lib_dir = build_package_dir / "lib"
        target_lib_dir.mkdir(parents=True, exist_ok=True)
        for path in source_lib_dir.glob("*.so*"):
            if path.is_symlink():
                link_target = os.readlink(path)
                target_path = target_lib_dir / path.name
                target_path.unlink(missing_ok=True)
                os.symlink(link_target, target_path)
            else:
                shutil.copy2(path, target_lib_dir / path.name)


class build_py(_build_py):
    def run(self):
        if not os.environ.get("HFT_SKIP_CMAKE"):
            self._build_native()
        super().run()
        _stage_package_artifacts(self.build_lib)

    def _build_native(self):
        import pybind11

        if CMAKE_BUILD_DIR.exists():
            shutil.rmtree(CMAKE_BUILD_DIR)
        if SETUPTOOLS_BUILD_DIR.exists():
            shutil.rmtree(SETUPTOOLS_BUILD_DIR)
        PACKAGE_WORK_DIR.mkdir(parents=True, exist_ok=True)
        CMAKE_BUILD_DIR.mkdir(parents=True, exist_ok=True)
        self._clean_stale_artifacts()

        cmake_args = [
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(CMAKE_BUILD_DIR),
            f"-DPython3_EXECUTABLE={sys.executable}",
            f"-DPython3_ROOT_DIR={sys.prefix}",
            f"-DPython3_INCLUDE_DIR={sysconfig.get_paths().get('include', '')}",
            f"-DPython3_LIBRARY={_python_library()}",
            f"-Dpybind11_DIR={pybind11.get_cmake_dir()}",
        ]
        subprocess.check_call(cmake_args, cwd=ROOT)
        subprocess.check_call(
            [
                "cmake",
                "--build",
                str(CMAKE_BUILD_DIR),
                "--parallel", "2",
                "--target",
                "_core",
                "hft_core",
                "hft_engine_lib",
                "mod_replay",
                "mod_py_strategy",
                "mod_risk",
                "mod_sim_trade",
                "mod_backtest_recorder",
            ],
            cwd=ROOT,
        )

    def _clean_stale_artifacts(self):
        _clean_package_artifacts()


class bdist_wheel(_bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False
        DIST_DIR.mkdir(parents=True, exist_ok=True)
        self.dist_dir = str(DIST_DIR)

    def run(self):
        try:
            super().run()
        finally:
            _clean_package_artifacts()
            _clean_packaging_workdirs()


version_ns = {}
with open(PACKAGE_DIR / "__init__.py", "r", encoding="utf-8") as f:
    exec(f.read().split("from .engine import", 1)[0], version_ns)


setup(
    name="hft-backtest",
    version=version_ns["__version__"],
    description="Python backtest package powered by hft_eb engine libraries",
    author="HFT Team",
    ext_modules=[Extension("hft_backtest._core", [])],
    packages=find_packages(include=["hft_backtest", "hft_backtest.*"]),
    include_package_data=True,
    package_data={
        "hft_backtest": ["*.so", "*.so.*", "lib/*.so", "lib/*.so.*"],
    },
    entry_points={
        "console_scripts": [
            "hft-backtest-demo=hft_backtest.scripts.run_demo:main",
            "hft-backtest-run-config=hft_backtest.scripts.run_config:main",
        ]
    },
    python_requires=">=3.10",
    cmdclass={"build_py": build_py, "bdist_wheel": bdist_wheel},
    options={"build": {"build_base": str(SETUPTOOLS_BUILD_DIR)}},
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
    ],
)
