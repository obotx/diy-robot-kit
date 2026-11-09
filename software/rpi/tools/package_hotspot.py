"""Package the application and fresh-install instructions, without credentials."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
from package_release import ROOT

OUTPUT = ROOT / "packages" / "ObotX-RPi-hotspot.zip"


def package():
    paths = [ROOT / name for name in ("requirements.txt", "INSTALL.md")]
    for folder in ("app", "web", "systemd"):
        paths.extend(p for p in (ROOT / folder).rglob("*") if p.is_file()
                     and "__pycache__" not in p.parts and p.suffix not in {".pyc", ".log", ".tmp"})
    paths.extend(ROOT / "docs" / name for name in ("hotspot-installation.md", "smartshunt.md", "cameras.md"))
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(OUTPUT, "w", ZIP_DEFLATED) as archive:
        for path in sorted(paths):
            archive.writestr(path.relative_to(ROOT).as_posix(), path.read_bytes().replace(b"\r\n", b"\n")
                             if path.suffix in {".sh", ".py", ".env", ".service", ".md"} else path.read_bytes())
    with ZipFile(OUTPUT) as archive:
        assert archive.testzip() is None
        assert "systemd/prepare-hotspot.py" in archive.namelist()
        assert not any(".venv" in n or n.endswith(".nmconnection") for n in archive.namelist())
    print(OUTPUT)


if __name__ == "__main__":
    package()
