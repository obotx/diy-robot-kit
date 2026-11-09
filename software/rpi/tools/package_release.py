"""Build a code-only Raspberry Pi update archive; never include the local venv."""

from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT.parents[1] / "output" / "obotx-rpi-panel-0.9.29.zip"
FOLDERS = ("app", "web", "systemd", "examples", "docs")
FILES = ("requirements.txt", "README.md")


def package():
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    paths = [ROOT / name for name in FILES]
    for folder in FOLDERS:
        paths.extend(path for path in (ROOT / folder).rglob("*")
                     if path.is_file() and "__pycache__" not in path.parts
                     and path.suffix not in {".pyc", ".log", ".tmp"})
    with ZipFile(OUTPUT, "w", compression=ZIP_DEFLATED) as archive:
        for path in sorted(paths):
            name = path.relative_to(ROOT).as_posix()
            if path.suffix in {".sh", ".env", ".service"}:
                archive.writestr(name, path.read_bytes().replace(b"\r\n", b"\n"))
            else:
                archive.write(path, name)
    with ZipFile(OUTPUT) as archive:
        assert archive.testzip() is None
        names = archive.namelist()
        assert all(".venv" not in name and ".." not in name.split("/") for name in names)
    print(f"{OUTPUT}\n{len(paths)} files")


if __name__ == "__main__":
    package()
