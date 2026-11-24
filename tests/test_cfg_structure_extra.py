"""Additional edge-case tests for tools/cfg_structure.py."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import cfg_structure as cs


def _run_tree(tree, step_fn):
    trace = []
    pc = 0

    def exec_items(items):
        nonlocal pc
        for item in items:
            kind = item[0]
            if kind == "block":
                b = item[1]
                trace.append(b)
                pc = step_fn(b)
            elif kind == "guard":
                if pc == item[1]:
                    exec_items(item[2])
            elif kind == "loop":
                hdr = item[1]
                while pc == hdr:
                    exec_items(item[2])
            else:
                raise AssertionError(f"unknown item kind: {kind}")

    exec_items(tree)
    assert pc == cs.END
    return trace


def test_single_block_cfg():
    tree = cs.structure({0: []})
    assert tree == [("block", 0)]
    assert _run_tree(tree, lambda _b: cs.END) == [0]


def test_irreducible_two_entry_loop_returns_none():
    succ = {0: [1, 2], 1: [2], 2: [1, 3], 3: []}
    assert cs.structure(succ) is None


def test_nested_loop_with_early_exit_executes_exact_trace():
    succ = {0: [1], 1: [2, 4], 2: [3, 2], 3: [1, 4], 4: []}
    tree = cs.structure(succ)
    assert tree is not None

    # Outer iter 1: 0 -> 1 -> 2 -> 2 -> 3 -> 1 -> 4 -> END
    b2_calls = 0

    def step(b):
        nonlocal b2_calls
        if b == 0:
            return 1
        if b == 1:
            return 2 if b2_calls == 0 else 4
        if b == 2:
            b2_calls += 1
            return 2 if b2_calls == 1 else 3
        if b == 3:
            return 1
        if b == 4:
            return cs.END
        raise AssertionError(b)

    assert _run_tree(tree, step) == [0, 1, 2, 2, 3, 1, 4]
