"""Reconnect grants; service selection outlives the short motion recovery window."""
from dataclasses import dataclass, field
from secrets import token_urlsafe
import time


@dataclass
class ControlSession:
    client_id: str
    token: str = field(default_factory=lambda: token_urlsafe(32))
    lease: str = field(default_factory=lambda: token_urlsafe(24))
    wanted: bool = False
    paused: bool = False
    service: bool = False
    connected: bool = True
    expires: float = 0
    generation: int = 0

    def pause(self):
        if not self.paused:
            self.lease = token_urlsafe(24)
            self.paused = True


class ControlSessions:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.items = {}

    def add(self, session):
        self.items = {k: s for k, s in self.items.items()
                      if s.connected or s.service or s.expires > self.clock()}
        self.items[session.token] = session

    def resume(self, token, generation):
        session = self.items.get(token)
        if not session or session.connected:
            return None
        stale = session.expires <= self.clock() or session.generation != generation
        if stale and not session.service:
            return None
        if stale:
            session.wanted = False
        session.generation = generation
        del self.items[token]
        session.token = token_urlsafe(32)
        session.connected = True
        session.pause()
        self.add(session)
        return session

    def detach(self, session, generation):
        session.pause()
        session.connected = False
        session.generation = generation
        session.expires = self.clock() + 30
