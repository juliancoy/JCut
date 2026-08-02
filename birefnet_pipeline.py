"""Bounded ordered worker primitives for the BiRefNet frame pipeline."""

from __future__ import annotations

from collections import deque
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from typing import Callable, Generic, TypeVar


T = TypeVar("T")


@dataclass(frozen=True)
class OrderedResult(Generic[T]):
    sequence: int
    value: T


class BoundedOrderedExecutor(Generic[T]):
    """Single-consumer ordered worker with explicit bounded backpressure."""

    def __init__(self, capacity: int, thread_name: str) -> None:
        if capacity < 1:
            raise ValueError("capacity must be positive")
        self.capacity = capacity
        self._executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix=thread_name
        )
        self._pending: deque[tuple[int, Future[T]]] = deque()

    @property
    def pending_count(self) -> int:
        return len(self._pending)

    def submit(
        self, sequence: int, function: Callable[..., T], *args, **kwargs
    ) -> list[OrderedResult[T]]:
        completed: list[OrderedResult[T]] = []
        if len(self._pending) >= self.capacity:
            completed.append(self.finish_oldest())
        self._pending.append(
            (sequence, self._executor.submit(function, *args, **kwargs))
        )
        completed.extend(self.collect_ready())
        return completed

    def collect_ready(self) -> list[OrderedResult[T]]:
        completed: list[OrderedResult[T]] = []
        while self._pending and self._pending[0][1].done():
            completed.append(self.finish_oldest())
        return completed

    def finish_oldest(self) -> OrderedResult[T]:
        if not self._pending:
            raise RuntimeError("no pending work")
        sequence, future = self._pending.popleft()
        return OrderedResult(sequence, future.result())

    def drain(self) -> list[OrderedResult[T]]:
        completed: list[OrderedResult[T]] = []
        while self._pending:
            completed.append(self.finish_oldest())
        return completed

    def shutdown(self) -> None:
        self._executor.shutdown(wait=True, cancel_futures=False)


class OptionalLatestExecutor(Generic[T]):
    """One-slot optional monitor worker; busy submissions are dropped."""

    def __init__(self, thread_name: str) -> None:
        self._executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix=thread_name
        )
        self._pending: Future[T] | None = None
        self.submitted = 0
        self.dropped = 0

    @property
    def busy(self) -> bool:
        return self._pending is not None and not self._pending.done()

    def collect_ready(self) -> list[T]:
        if self._pending is None or not self._pending.done():
            return []
        result = self._pending.result()
        self._pending = None
        return [result]

    def submit(self, function: Callable[..., T], *args, **kwargs) -> list[T]:
        completed = self.collect_ready()
        if self._pending is not None:
            self.dropped += 1
            return completed
        self._pending = self._executor.submit(function, *args, **kwargs)
        self.submitted += 1
        return completed

    def drain(self) -> list[T]:
        if self._pending is None:
            return []
        result = self._pending.result()
        self._pending = None
        return [result]

    def shutdown(self) -> None:
        self._executor.shutdown(wait=True, cancel_futures=False)
