#!/usr/bin/env python3
"""Build the complete CBLPRD validation archive for RK3568 evaluation."""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import tarfile
from pathlib import Path, PurePosixPath


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_DATASET_ROOT = SCRIPT_DIR.parent / "datasets" / "CBLPRD-330k"
PACKAGE_ROOT = PurePosixPath("CBLPRD")
MANIFEST_NAMES = (
    "val_basic.txt",
    "val_hard.txt",
    "val_special_使.txt",
    "val_special_学.txt",
    "val_special_港.txt",
    "val_special_澳.txt",
    "val_special_警.txt",
    "val_special_领.txt",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create cblprd_eval.tar.gz containing all eight CBLPRD "
            "validation subsets and their UTF-8 manifests."
        )
    )
    parser.add_argument(
        "--dataset-root",
        type=Path,
        default=DEFAULT_DATASET_ROOT,
        help=f"Directory containing CBLPRD/ and val_*.txt (default: {DEFAULT_DATASET_ROOT})",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Output .tar.gz path (default: <dataset-root>/cblprd_eval.tar.gz)",
    )
    parser.add_argument(
        "--compress-level",
        type=int,
        choices=range(1, 10),
        default=1,
        metavar="1-9",
        help="gzip compression level; JPEG data normally benefits from level 1",
    )
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and report all files without creating an archive",
    )
    return parser.parse_args()


def read_manifest(dataset_root: Path, manifest_name: str) -> list[tuple[str, Path, str]]:
    manifest_path = dataset_root / manifest_name
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Manifest not found: {manifest_path}")

    dataset_root_resolved = dataset_root.resolve()
    records: list[tuple[str, Path, str]] = []
    seen_paths: set[str] = set()
    for line_number, raw_line in enumerate(
        manifest_path.read_text(encoding="utf-8-sig").splitlines(), start=1
    ):
        if not raw_line or raw_line.count("\t") != 1:
            raise ValueError(
                f"{manifest_path}:{line_number} must be path<TAB>label"
            )
        path_text, label = raw_line.split("\t")
        relative_path = PurePosixPath(path_text.replace("\\", "/"))
        if (
            relative_path.is_absolute()
            or not relative_path.parts
            or relative_path.parts[0] != "CBLPRD"
            or ".." in relative_path.parts
            or relative_path.suffix.lower() != ".jpg"
        ):
            raise ValueError(
                f"Unsafe or unexpected image path at {manifest_path}:{line_number}: "
                f"{path_text}"
            )
        if not label or path_text in seen_paths:
            raise ValueError(
                f"Empty label or duplicate path at {manifest_path}:{line_number}"
            )

        source_path = dataset_root.joinpath(*relative_path.parts).resolve()
        try:
            source_path.relative_to(dataset_root_resolved)
        except ValueError as exc:
            raise ValueError(f"Image escapes dataset root: {path_text}") from exc
        if not source_path.is_file():
            raise FileNotFoundError(
                f"Image referenced by {manifest_path}:{line_number} is missing: "
                f"{source_path}"
            )

        seen_paths.add(path_text)
        records.append((f"{path_text}\t{label}", source_path, path_text))
    if not records:
        raise ValueError(f"Manifest is empty: {manifest_path}")
    return records


def add_manifest(
    archive: tarfile.TarFile, manifest_name: str, records: list[tuple[str, Path, str]]
) -> str:
    content = "".join(f"{record[0]}\n" for record in records).encode("utf-8")
    archive_name = (PACKAGE_ROOT / manifest_name).as_posix()
    info = tarfile.TarInfo(archive_name)
    info.size = len(content)
    info.mode = 0o644
    info.mtime = 0
    archive.addfile(info, io.BytesIO(content))
    return archive_name


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_archive(
    output_path: Path,
    manifests: dict[str, list[tuple[str, Path, str]]],
    compress_level: int,
    overwrite: bool,
) -> None:
    output_path = output_path.resolve()
    if output_path.exists() and not overwrite:
        raise FileExistsError(f"Output already exists; pass --overwrite: {output_path}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f".{output_path.name}.tmp")
    if temporary_path.exists():
        temporary_path.unlink()

    expected_names: set[str] = set()
    try:
        with tarfile.open(
            temporary_path,
            mode="w:gz",
            compresslevel=compress_level,
            format=tarfile.PAX_FORMAT,
            encoding="utf-8",
            errors="strict",
        ) as archive:
            for manifest_name, records in manifests.items():
                expected_names.add(add_manifest(archive, manifest_name, records))
                for _, source_path, relative_path in records:
                    archive_name = (PACKAGE_ROOT / relative_path).as_posix()
                    if archive_name in expected_names:
                        raise ValueError(f"Duplicate archive path: {archive_name}")
                    archive.add(source_path, arcname=archive_name, recursive=False)
                    expected_names.add(archive_name)

        with tarfile.open(temporary_path, mode="r:gz", encoding="utf-8") as archive:
            actual_names = [member.name for member in archive.getmembers()]
        if len(actual_names) != len(set(actual_names)):
            raise RuntimeError("Archive validation found duplicate member names")
        if set(actual_names) != expected_names:
            raise RuntimeError("Archive validation found missing or unexpected members")
        os.replace(temporary_path, output_path)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()

    print(f"Archive: {output_path}")
    print(f"Size: {output_path.stat().st_size / 1024 / 1024:.2f} MiB")
    print(f"SHA256: {sha256_file(output_path)}")
    print("Extract on board to: /userdata/cblprd_eval/")


def main() -> None:
    args = parse_args()
    dataset_root = args.dataset_root.resolve()
    if not (dataset_root / "CBLPRD").is_dir():
        raise FileNotFoundError(f"Dataset image root not found: {dataset_root / 'CBLPRD'}")

    manifests: dict[str, list[tuple[str, Path, str]]] = {}
    all_image_paths: set[str] = set()
    print(f"Dataset root: {dataset_root}")
    print("-" * 42)
    print(f"{'manifest':<24}{'images':>12}")
    for manifest_name in MANIFEST_NAMES:
        records = read_manifest(dataset_root, manifest_name)
        for _, _, path_text in records:
            if path_text in all_image_paths:
                raise ValueError(f"Image appears in multiple manifests: {path_text}")
            all_image_paths.add(path_text)
        manifests[manifest_name] = records
        print(f"{manifest_name:<24}{len(records):>12,}")

    image_count = sum(len(records) for records in manifests.values())
    archive_member_count = image_count + len(manifests)
    print("-" * 42)
    print(f"Validation images: {image_count:,}")
    print(f"Archive file members: {archive_member_count:,}")
    if args.dry_run:
        print("Dry run passed; archive was not created.")
        return

    output_path = args.output or dataset_root / "cblprd_eval.tar.gz"
    build_archive(output_path, manifests, args.compress_level, args.overwrite)


if __name__ == "__main__":
    main()
