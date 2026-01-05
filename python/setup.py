from setuptools import setup, find_packages

setup(
    name="hybridacc_verify",
    version="0.1.0",
    description="Verification tools for HybridAcc",
    packages=find_packages(),
    install_requires=[
        "numpy",
        "torch",
        "pyyaml",
    ],
    include_package_data=True,
)
