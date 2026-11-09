"""Local service password configuration; no built-in password or persistent unlock."""
import asyncio
import getpass
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import time


def password_path():
    return Path(os.getenv("OBOTX_SERVICE_PASSWORD_FILE", str(Path.home() / ".config/obotx/service-password.json")))


def digest(password, salt):
    return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), bytes.fromhex(salt), 600000).hex()


class ServiceAccess:
    def __init__(self, path=None, clock=time.monotonic):
        self.clock = clock
        self.next_attempt = 0.0
        self.failures = 0
        self.record = None
        try:
            data = json.loads((path or password_path()).read_text())
            if data.get("algorithm") != "pbkdf2-sha256-600000":
                raise ValueError()
            if len(bytes.fromhex(data["salt"])) != 16 or len(bytes.fromhex(data["digest"])) != 32:
                raise ValueError()
            self.record = data
        except (OSError, ValueError, KeyError, TypeError):
            pass

    async def authorize(self, password):
        now = self.clock()
        if not self.record or now < self.next_attempt:
            return False
        # Reserve before yielding, shared across all sockets; no parallel guessing.
        self.next_attempt = now + 2
        candidate = await asyncio.to_thread(digest, password, self.record["salt"])
        ok = hmac.compare_digest(candidate, self.record["digest"])
        self.failures = 0 if ok else self.failures + 1
        if self.failures >= 5:
            self.next_attempt = self.clock() + 30
        return ok


def main():
    password = getpass.getpass("New service password (12-256 characters): ")
    if not 12 <= len(password) <= 256 or password != getpass.getpass("Repeat password: "):
        raise SystemExit("Password too short/long or confirmation differs. Nothing changed.")
    salt = secrets.token_hex(16)
    data = {"algorithm": "pbkdf2-sha256-600000", "salt": salt, "digest": digest(password, salt)}
    path = password_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + secrets.token_hex(8))
    try:
        with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as file:
            json.dump(data, file)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    print("Password hash saved. Restart obotx-rpi-control. No service mode or motion was enabled.")


if __name__ == "__main__":
    main()
