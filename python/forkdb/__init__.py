"""
forkdb - A branchable, snapshot-based SQLite page store

Python bindings for forkdb, providing a convenient interface to the forkdb CLI.
"""

__version__ = "0.1.0"
__author__ = "Ben McKibben"

from .forkdb import ForkDB

__all__ = ["ForkDB"]