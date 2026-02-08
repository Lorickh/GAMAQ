import asyncio
from typing import Any, AsyncIterator, Dict

from sse_starlette.sse import EventSourceResponse


class EventBus:
    def __init__(self) -> None:
        self._queues: Dict[str, asyncio.Queue] = {}

    def get_queue(self, task_id: str) -> asyncio.Queue:
        if task_id not in self._queues:
            self._queues[task_id] = asyncio.Queue()
        return self._queues[task_id]

    async def publish(self, task_id: str, event_type: str, payload: Dict[str, Any]) -> None:
        queue = self.get_queue(task_id)
        await queue.put({"event": event_type, "data": payload})

    async def stream(self, task_id: str) -> AsyncIterator[Dict[str, Any]]:
        queue = self.get_queue(task_id)
        while True:
            message = await queue.get()
            yield message


bus = EventBus()


def sse_events(task_id: str) -> EventSourceResponse:
    async def event_generator() -> AsyncIterator[Dict[str, Any]]:
        async for message in bus.stream(task_id):
            yield message

    return EventSourceResponse(event_generator())
