"""
HFT Client Library Setup
"""

from setuptools import setup, find_packages

with open("hft_client/__init__.py", "r") as f:
    for line in f:
        if line.startswith("__version__"):
            version = line.split("=")[1].strip().strip('"').strip("'")
            break
    else:
        version = "0.1.0"

setup(
    name="hft-client",
    version=version,
    description="HFT Engine WebSocket Client Library",
    author="HFT Team",
    packages=find_packages(),
    install_requires=[
        "websockets>=10.0",
    ],
    extras_require={
        "dev": [
            "pytest>=7.0",
            "pytest-asyncio>=0.20",
            "black>=22.0",
            "mypy>=0.991",
        ],
    },
    python_requires=">=3.8",
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
    ],
)
