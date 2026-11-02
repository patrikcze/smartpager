"""Wakes long-polling sync requests when a new change is committed.

Usage avoids the lost-wakeup race: take `token = notifier.token()` BEFORE querying,
then `await notifier.wait(token, timeout)` if the query returned nothing. A change
committed between the query and the wait has already set that token's event.
"""
from __future__ import annotations

import asyncio


class ChangeNotifier:
    def __init__(self) -> None:
        self._loop: asyncio.AbstractEventLoop | None = None
        self._event = asyncio.Event()

    def bind(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    def notify(self) -> None:
        """Thread-safe: called from sync endpoints running in the threadpool."""
        if self._loop is not None and not self._loop.is_closed():
            self._loop.call_soon_threadsafe(self._fire)

    def _fire(self) -> None:
        self._event.set()
        self._event = asyncio.Event()

    def token(self) -> asyncio.Event:
        return self._event

    async def wait(self, token: asyncio.Event, timeout: float) -> bool:
        try:
            await asyncio.wait_for(token.wait(), timeout)
            return True
        except asyncio.TimeoutError:
            return False
