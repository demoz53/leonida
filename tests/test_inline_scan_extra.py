"""Additional edge-case tests for tools/inline_scan.py."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import inline_scan


def test_add_cursor_advance_and_reversed_cmp_is_checked():
    listing = [
        "  0000000140001000: mov         rbx,rcx",
        "  0000000140001003: mov         rax,qword ptr [rbx+18h]",
        "  0000000140001007: cmp         qword ptr [rbx+28h],rax",
        "  000000014000100B: jbe         0000000140001030",
        "  000000014000100D: mov         dword ptr [rax],1",
        "  0000000140001013: add         rax,10h",
        "  0000000140001017: mov         qword ptr [rbx+18h],rax",
        "  000000014000101B: ret",
        "  0000000140001030: mov         rax,qword ptr [rbx]",
        "  0000000140001033: call        qword ptr [rax+80h]",
        "  0000000140001039: ret",
    ]
    sites = list(inline_scan.scan(listing))
    assert len(sites) == 1
    assert sites[0]["checked"] is True
    assert sites[0]["list"] is True
    assert sites[0]["size"] == 0x10
    assert sites[0]["limit"] == 0x28
    assert sites[0]["fallback_slot"] == 16


def test_cli_sites_flag_prints_site_details(tmp_path):
    listing_file = tmp_path / "sample.txt"
    listing_file.write_text(
        "\n".join([
            "  0000000140002000: mov         rbx,rcx",
            "  0000000140002003: mov         rax,qword ptr [rbx+18h]",
            "  0000000140002007: mov         dword ptr [rbx+140h],2",
            "  0000000140002011: mov         dword ptr [rax],9",
            "  0000000140002017: lea         rdx,[rax+8h]",
            "  000000014000201B: mov         qword ptr [rbx+18h],rdx",
            "  000000014000201F: ret",
        ]),
        encoding="utf-8",
    )
    tool = str(Path(__file__).resolve().parent.parent / "tools" / "inline_scan.py")
    proc = subprocess.run(
        [sys.executable, "-I", tool, str(listing_file), "--base", "0x140000000", "--sites"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    assert proc.returncode == 0, proc.stderr
    assert "UNCHECKED" in proc.stdout
    assert "0x201b" in proc.stdout.lower()
