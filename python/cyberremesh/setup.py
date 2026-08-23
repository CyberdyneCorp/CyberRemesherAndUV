"""Shim that marks the wheel as platform-specific.

Everything else lives in pyproject.toml. The package ships a prebuilt C ABI
shared library (staged by packaging/publish/stage_native_lib.py) rather than an
Extension setuptools compiles, so setuptools would otherwise tag the wheel
`py3-none-any`: a pure-Python tag that hides the library from auditwheel and
delocate and makes cibuildwheel reject the build. Declaring ext modules flips
the tag to the running platform's without adding a build step.
"""

from setuptools import setup
from setuptools.dist import Distribution


class BinaryDistribution(Distribution):
    """A distribution that is platform-specific despite compiling nothing."""

    def has_ext_modules(self) -> bool:  # noqa: D102 - setuptools hook
        return True

    def is_pure(self) -> bool:  # noqa: D102 - setuptools hook
        return False


setup(distclass=BinaryDistribution)
