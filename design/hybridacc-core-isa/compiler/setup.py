from setuptools import setup, find_packages

setup(
    name="core-isa-compiler",
    version="0.1.0",
    description="Core-ISA Compiler for RISC-V RV32I Assembly",
    author="HybridAcc Team",
    author_email="hybridacc@example.com",
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    python_requires=">=3.8",
    install_requires=[
        "typing-extensions>=4.0.0",
        "dataclasses>=0.6",
    ],
    extras_require={
        "dev": [
            "pytest>=6.0",
            "black>=21.0",
            "flake8>=3.8",
            "mypy>=0.910",
        ],
        "test": [
            "pytest>=6.0",
            "pytest-cov>=2.10",
        ],
    },
    entry_points={
        "console_scripts": [
            "core-isa-compile=compiler.main:main",
        ],
    },
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