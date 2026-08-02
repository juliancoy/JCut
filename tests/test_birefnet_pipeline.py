#!/usr/bin/env python3

import threading
import time
import unittest

from birefnet_pipeline import BoundedOrderedExecutor, OptionalLatestExecutor


class BiRefNetPipelineTest(unittest.TestCase):
    def test_bounded_executor_preserves_order_and_capacity(self) -> None:
        release = threading.Event()

        def work(value: int) -> int:
            if value == 1:
                release.wait(timeout=2.0)
            return value * 10

        executor = BoundedOrderedExecutor[int](2, "ordered-test")
        try:
            self.assertEqual(executor.submit(1, work, 1), [])
            self.assertEqual(executor.submit(2, work, 2), [])
            self.assertEqual(executor.pending_count, 2)
            release.set()
            completed = executor.submit(3, work, 3)
            completed.extend(executor.drain())
            self.assertEqual(
                [(item.sequence, item.value) for item in completed],
                [(1, 10), (2, 20), (3, 30)],
            )
        finally:
            release.set()
            executor.shutdown()

    def test_optional_monitor_drops_while_busy(self) -> None:
        release = threading.Event()

        def preview(value: int) -> int:
            release.wait(timeout=2.0)
            return value

        monitor = OptionalLatestExecutor[int]("monitor-test")
        try:
            self.assertEqual(monitor.submit(preview, 1), [])
            self.assertEqual(monitor.submit(preview, 2), [])
            self.assertEqual(monitor.submitted, 1)
            self.assertEqual(monitor.dropped, 1)
            release.set()
            self.assertEqual(monitor.drain(), [1])
        finally:
            release.set()
            monitor.shutdown()

    def test_worker_failure_propagates_at_ordered_boundary(self) -> None:
        def fail() -> int:
            raise RuntimeError("publication failed")

        executor = BoundedOrderedExecutor[int](1, "failure-test")
        try:
            with self.assertRaisesRegex(RuntimeError, "publication failed"):
                executor.submit(1, fail)
                executor.drain()
        finally:
            executor.shutdown()


if __name__ == "__main__":
    unittest.main()
