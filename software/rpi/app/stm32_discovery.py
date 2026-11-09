"""Probe only supported ST-Link VCPs, using disabled JSON HELLO, never ASCII."""
from uuid import uuid4

from serial.tools import list_ports

from .stm32_config import BoardConfig
from .stm32_protocol import LinkError, SerialPeer

# ST-Link V2-1 VCP as observed on the project's two Nucleo boards.
USB_IDS = {(0x0483, 0x374B)}
MAX_CANDIDATES = 8


def candidates():
    return sorted({p.device for p in list_ports.comports()
                   if (p.vid, p.pid) in USB_IDS and "://" not in p.device})


def discover(config, opener, clock, closing, ports):
    paths = ports()
    if len(paths) > MAX_CANDIDATES:
        raise LinkError("discovery_too_many_candidates")
    found = {}
    errors = []
    ambiguous = False
    for path in paths:
        if closing.is_set():
            raise LinkError("shutdown")
        if "://" in path:
            raise LinkError("discovery_local_ports_only")
        peer = None
        try:
            board = BoardConfig(port=path, device_id="discovering")
            peer = SerialPeer(opener(board, config, "stm32"), None, None, uuid4().hex, clock)
            identity = peer.hello()
            if identity.role in found or any(b.device_id == identity.device_id for b in found.values()):
                ambiguous = True
            else:
                found[identity.role] = BoardConfig(port=path, device_id=identity.device_id)
        except Exception as exc:
            errors.append(f"{path}: {exc}")
        finally:
            if peer:
                peer.stop_best_effort()
                peer.close()
    if ambiguous:
        raise LinkError("discovery_ambiguous_roles_or_ids")
    # A silent candidate could be another ARM/base: do not guess an assignment.
    if errors:
        raise LinkError("discovery_probe_failed: " + "; ".join(errors))
    if set(found) != {"arm", "base"}:
        missing = ",".join(sorted({"arm", "base"} - set(found)))
        raise LinkError(f"discovery_waiting_for_{missing}")
    return found
