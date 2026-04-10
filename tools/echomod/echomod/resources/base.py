"""Base classes for Echo VR resource types."""

from __future__ import annotations
from abc import ABC, abstractmethod


class Resource(ABC):
    """Base class for all resource types."""

    type_name: str = ""

    @classmethod
    @abstractmethod
    def from_bytes(cls, data: bytes) -> Resource:
        ...

    @abstractmethod
    def to_bytes(self) -> bytes:
        ...
