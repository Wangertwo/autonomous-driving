#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# ///
import csv
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "large_log_tool.cpp"
BUFFERED_SOURCE = ROOT / "large_log_tool_buffered.cpp"

MAGIC = b"ADLG"
VERSION = 1
FILE_HEADER_SIZE = 8
RECORD_HEADER = struct.Struct("=QHI")


def run(args, *, check=True):
    result = subprocess.run(args, text=True, capture_output=True, check=False)
    if check and result.returncode != 0:
        raise AssertionError(
            f"command failed: {' '.join(map(str, args))}\n"
            f"exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
        )
    return result


def compile_cpp(source: Path, binary: Path) -> Path:
    run([
        "g++",
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-pedantic",
        str(source),
        "-o",
        str(binary),
    ])
    return binary


def compile_tool(tmp: Path) -> Path:
    return compile_cpp(SOURCE, tmp / "large_log_tool")


def compile_buffered_tool(tmp: Path) -> Path:
    return compile_cpp(BUFFERED_SOURCE, tmp / "large_log_tool_buffered")


def read_records(path: Path):
    data = path.read_bytes()
    assert data[:4] == MAGIC
    assert struct.unpack_from("=I", data, 4)[0] == VERSION

    offset = FILE_HEADER_SIZE
    records = []
    while offset < len(data):
        timestamp_ns, topic_len, payload_len = RECORD_HEADER.unpack_from(data, offset)
        cursor = offset + RECORD_HEADER.size
        topic = data[cursor:cursor + topic_len].decode()
        cursor += topic_len
        payload = data[cursor:cursor + payload_len]
        cursor += payload_len
        records.append({
            "offset": offset,
            "timestamp_ns": timestamp_ns,
            "topic": topic,
            "payload_len": payload_len,
            "payload": payload,
            "next_offset": cursor,
        })
        offset = cursor
    return records


def parse_next_offset(stdout: str) -> int:
    for line in stdout.splitlines():
        if line.startswith("next_offset="):
            return int(line.split("=", 1)[1])
    raise AssertionError(f"missing next_offset in output:\n{stdout}")


def test_compile_and_generate(tool: Path, tmp: Path):
    log = tmp / "demo.log"
    run([tool, "generate", log, "100"])

    records = read_records(log)
    assert len(records) == 100
    assert records[0]["offset"] == FILE_HEADER_SIZE
    assert records[0]["topic"] == "/camera/front"
    assert records[1]["topic"] == "/lidar/points"
    assert records[0]["timestamp_ns"] == 1_700_000_000_000_000_000
    assert records[1]["timestamp_ns"] - records[0]["timestamp_ns"] == 10_000_000


def test_scan_checkpoint_resume(tool: Path, tmp: Path):
    log = tmp / "demo.log"
    checkpoint = tmp / "demo.ckpt"
    run([tool, "generate", log, "100"])
    records = read_records(log)

    first = run([tool, "scan", log, checkpoint, "10"])
    assert "scanned_records=10" in first.stdout
    assert parse_next_offset(first.stdout) == records[9]["next_offset"]
    assert int(checkpoint.read_text()) == records[9]["next_offset"]

    second = run([tool, "scan", log, checkpoint, "10"])
    assert "scanned_records=10" in second.stdout
    assert parse_next_offset(second.stdout) == records[19]["next_offset"]
    assert int(checkpoint.read_text()) == records[19]["next_offset"]


def test_bad_checkpoint_resets_to_header(tool: Path, tmp: Path):
    log = tmp / "demo.log"
    checkpoint = tmp / "bad.ckpt"
    run([tool, "generate", log, "10"])
    checkpoint.write_text("9\n")

    result = run([tool, "scan", log, checkpoint, "1"])
    assert "checkpoint is not on a record boundary" in result.stderr
    assert "scanned_records=1" in result.stdout

    records = read_records(log)
    assert int(checkpoint.read_text()) == records[0]["next_offset"]


def test_index_matches_record_offsets(tool: Path, tmp: Path):
    log = tmp / "demo.log"
    index = tmp / "index.csv"
    run([tool, "generate", log, "20"])
    run([tool, "index", log, index])

    records = read_records(log)
    with index.open(newline="") as f:
        rows = list(csv.reader(f))

    assert len(rows) == len(records)
    for row, record in zip(rows, records):
        assert int(row[0]) == record["timestamp_ns"]
        assert int(row[1]) == record["offset"]
        assert row[2] == record["topic"]
        assert int(row[3]) == record["payload_len"]


def test_cut_preserves_complete_records(tool: Path, tmp: Path):
    log = tmp / "demo.log"
    cut = tmp / "cut.log"
    checkpoint = tmp / "cut.ckpt"
    run([tool, "generate", log, "100"])
    records = read_records(log)

    begin = records[10]["timestamp_ns"]
    end = records[15]["timestamp_ns"]
    run([tool, "cut", log, cut, str(begin), str(end)])

    cut_records = read_records(cut)
    assert len(cut_records) == 5
    assert [r["timestamp_ns"] for r in cut_records] == [r["timestamp_ns"] for r in records[10:15]]
    assert [r["topic"] for r in cut_records] == [r["topic"] for r in records[10:15]]
    assert [r["payload"] for r in cut_records] == [r["payload"] for r in records[10:15]]

    scan = run([tool, "scan", cut, checkpoint, "0"])
    assert "scanned_records=5" in scan.stdout


def test_buffered_scan_matches_original(tool: Path, buffered_tool: Path, tmp: Path):
    log = tmp / "buffered.log"
    checkpoint = tmp / "buffered.ckpt"
    run([tool, "generate", log, "1000"])

    original = run([tool, "scan", log, checkpoint, "0"])
    for buffer_size in [14, 32, 4 * 1024 * 1024]:
        buffered = run([buffered_tool, "scan", log, str(buffer_size)])
        assert "scanned_records=1000" in buffered.stdout
        for topic in ["/camera/front", "/canbus/chassis", "/lidar/points", "/planning/trajectory"]:
            assert f"{topic} 250" in original.stdout
            assert f"{topic} 250" in buffered.stdout


def main():
    with tempfile.TemporaryDirectory(prefix="large-log-tool-test-") as tmpdir:
        tmp = Path(tmpdir)
        tool = compile_tool(tmp)
        buffered_tool = compile_buffered_tool(tmp)
        tests = [
            test_compile_and_generate,
            test_scan_checkpoint_resume,
            test_bad_checkpoint_resets_to_header,
            test_index_matches_record_offsets,
            test_cut_preserves_complete_records,
        ]
        for test in tests:
            test(tool, tmp)
            print(f"PASS {test.__name__}")
        test_buffered_scan_matches_original(tool, buffered_tool, tmp)
        print("PASS test_buffered_scan_matches_original")


if __name__ == "__main__":
    main()
