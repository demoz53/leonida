"""The packed-multiply GPU fixture needs a 512-byte heap and checks all 32 lanes, through both runners.

    python -I tests/test_pk_mul_lo_gpu_fixture.py

Harness and translation stand-ins only; no GPU, LLVM or game files.
"""
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import run_gpu_tests as rgt  # noqa: E402
import check  # noqa: E402; run_gpu_tests adds the shader-tools directory

NAME = "cs_pk_mul_lo_const"
# The four RDNA2-measured words in cs_pk_mul_lo_const.asm, for all 32 lanes.
WORDS = [0x00009E88, 0x03675433, 0x00230000, 0x00077F6C] * 32
GOOD = struct.pack("<128I", *WORDS)
VALIDATION = {"LEONIDA_VK_VALIDATION": "1", "VK_KHRONOS_VALIDATION_GPUAV_ENABLE": "1",
              "LEONIDA_W64_EMULATE": "1"}


class PackedMultiplyFixture(unittest.TestCase):
    def exercise(self, runner, data=GOOD, code=0, diagnostic="", subgroup=None):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            spirv = work / "spirv"
            spirv.mkdir()
            (spirv / f"{NAME}.spv").touch()
            (spirv / "index.json").write_text(
                json.dumps({NAME: {"wave64_emulated": True, "subgroup_size": 32}}), encoding="utf-8", newline="\n")
            calls = []

            def harness(tool, shader, *args, env=None):
                calls.append((tool, shader, args, dict(env)))
                self.assertEqual(Path(tool).stem, "vk_compute_test")
                self.assertEqual(Path(shader).stem, NAME)
                self.assertEqual(args, ("1", "dump"))
                self.assertEqual(env["VKC_OUT_WORDS"], "128")
                for key, value in VALIDATION.items():
                    self.assertEqual(env[key], value)
                self.assertEqual(env["VKC_SUBGROUP_SIZE"], str(subgroup or 32))
                target = Path(env["VKC_DUMP"])
                self.assertFalse(target.exists())
                if data is not None:
                    target.write_bytes(data)
                # Correct console words cannot hide a corrupt tail or a validation failure.
                return code, diagnostic + "\nout[0..7] = " + " ".join(map(str, WORDS[:8])) + "\n"

            def translate(cmd, **kwargs):
                self.assertEqual(cmd[:3], [sys.executable, "-I", rgt.TRANSLATOR])
                (spirv / f"{NAME}.spv").touch()
                return 0, ""

            env = {**VALIDATION, "VKC_OUT_WORDS": "64"}
            if subgroup is not None:
                env["VKC_SUBGROUP_SIZE"] = str(subgroup)
            with (patch.dict(os.environ, env), patch.object(check, "run", side_effect=harness),
                  patch.object(rgt, "assemble_to_s", side_effect=lambda _mc, _src, dst, _name: Path(dst).touch()),
                  patch.object(rgt, "spirv_as"), patch.object(rgt, "run_cmd", side_effect=translate),
                  patch.object(rgt, "bary_fallback", return_value=False),
                  patch.object(rgt, "f16_interp_fallback", return_value=False)):
                if subgroup is None:
                    os.environ.pop("VKC_SUBGROUP_SIZE", None)
                if runner == "check":
                    result = check.pk_mul_lo_const_test(tmp, str(spirv))
                    status, output = (0 if not result else 1), str(result)
                else:
                    args = SimpleNamespace(filter=NAME, build=tmp, llvm_mc="unused")
                    output = io.StringIO()
                    with redirect_stdout(output):
                        status = rgt.run_suite(args, tmp)
                    output = output.getvalue()
            self.assertEqual(len(calls), 1)
            return status, output

    def test_both_runners_allocate_and_check_all_lanes_with_validation_enabled(self):
        for runner in ("check", "run_gpu_tests"):
            with self.subTest(runner=runner):
                status, output = self.exercise(runner)
                self.assertEqual(status, 0, output)

    def test_an_explicit_subgroup_size_is_forwarded(self):
        status, output = self.exercise("check", subgroup=64)
        self.assertEqual(status, 0, output)

    def test_bad_readback_or_harness_failure_never_passes(self):
        wrong = struct.pack("<128I", *WORDS[:-1], WORDS[-1] ^ 1)
        cases = (("last lane", wrong, 0, "", "word 127"),
                 ("old allocation", GOOD[:256], 0, "", "expected 512 dump bytes, got 256"),
                 ("missing dump", None, 0, "", "no VKC_DUMP file written"),
                 ("validation error", GOOD, 1, "validation: out-of-bounds store", "exit 1"))
        for runner in ("check", "run_gpu_tests"):
            for label, data, code, diagnostic, expected in cases:
                with self.subTest(runner=runner, case=label):
                    status, output = self.exercise(runner, data, code, diagnostic)
                    self.assertEqual(status, 1, output)
                    self.assertIn(expected, output)


if __name__ == "__main__":
    unittest.main()
