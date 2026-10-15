# Platform wheels that carry one prebuilt library and nothing that depends on the Python version: py3-none-<platform>.
from setuptools import Distribution, setup

try:
    from setuptools.command.bdist_wheel import bdist_wheel
except ImportError:  # setuptools older than 70.1
    from wheel.bdist_wheel import bdist_wheel


class BinaryDistribution(Distribution):
    def has_ext_modules(self):  # it holds a binary: the files go to platlib and the wheel says it is not pure Python
        return True


class PlatformWheel(bdist_wheel):
    def get_tag(self):
        _, _, plat = super().get_tag()
        return "py3", "none", plat


setup(distclass=BinaryDistribution, cmdclass={"bdist_wheel": PlatformWheel})
