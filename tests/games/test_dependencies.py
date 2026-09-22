"""Exercise dependency resolution without sibling repositories or network access."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class DependencyResolutionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="bsp deps ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "standalone"
        self.source.mkdir()
        shutil.copy(ROOT / "examples/common/game_dependencies.cmake", self.source)
        self.lock = {}
        self.checkouts = {}
        for name, marker in (
            ("engine", "cmake/mosaico_game_sdk.cmake"),
            ("utils", "mosaico-tools/cmake/raylib_lite_engine.cmake"),
        ):
            checkout = self.root / "origins" / name
            (checkout / marker).parent.mkdir(parents=True)
            (checkout / marker).write_text("# Test dependency\n")
            self.git(checkout, "init", "-q")
            self.git(checkout, "add", ".")
            self.git(checkout, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                     "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
            revision = self.git(checkout, "rev-parse", "HEAD").strip()
            self.lock[name] = {"repository": str(checkout), "revision": revision}
            self.checkouts[name] = checkout
        (self.source / "game_dependencies.json").write_text(json.dumps(self.lock))
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\nproject(probe NONE)\n'
            'include("${CMAKE_CURRENT_LIST_DIR}/game_dependencies.cmake")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/resolved.txt" '
            '"${RAYLIB_LITE_ENGINE_ROOT}\n${MOSAICO_UTILS_ROOT}\n")\n')
        self.build = self.root / "build"

    @staticmethod
    def git(cwd, *args):
        return subprocess.check_output(["git", "-C", str(cwd), *args], text=True)

    def configure(self, *args, success=True):
        result = subprocess.run(["cmake", "-S", str(self.source), "-B", str(self.build), *args],
                                capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def test_downloads_exact_pins_then_reconfigures_without_origins(self):
        self.configure()
        for name, path in zip(("engine", "utils"), (self.build / "resolved.txt").read_text().splitlines()):
            self.assertEqual(self.git(path, "rev-parse", "HEAD").strip(), self.lock[name]["revision"])
            self.assertIn(self.build, Path(path).parents)
        for checkout in self.checkouts.values():
            shutil.rmtree(checkout)
        self.configure("-DFETCHCONTENT_UPDATES_DISCONNECTED=ON")

    def test_build_reconfigures_when_the_pin_changes(self):
        self.configure()
        checkout = self.checkouts["engine"]
        (checkout / "cmake/mosaico_game_sdk.cmake").write_text("# Updated dependency\n")
        self.git(checkout, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qam", "new pin")
        revision = self.git(checkout, "rev-parse", "HEAD").strip()
        self.lock["engine"]["revision"] = revision
        (self.source / "game_dependencies.json").write_text(json.dumps(self.lock))
        result = subprocess.run(["cmake", "--build", str(self.build)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        resolved = (self.build / "resolved.txt").read_text().splitlines()[0]
        self.assertEqual(self.git(resolved, "rev-parse", "HEAD").strip(), revision)

    def test_explicit_checkouts_do_not_fetch(self):
        (self.source / "game_dependencies.json").write_text("{}")
        self.configure("-DRAYLIB_LITE_ENGINE_ROOT=" + str(self.checkouts["engine"]),
                       "-DMOSAICO_UTILS_ROOT=" + str(self.checkouts["utils"]))
        self.assertFalse((self.build / "_deps").exists())

    def test_invalid_explicit_checkout_does_not_fall_back(self):
        result = self.configure("-DRAYLIB_LITE_ENGINE_ROOT=" + str(self.root / "missing"), success=False)
        self.assertIn("RAYLIB_LITE_ENGINE_ROOT does not contain", result.stderr)
        self.assertFalse((self.build / "_deps").exists())

    def test_host_only_never_fetches_utils(self):
        self.lock.pop("utils")
        (self.source / "game_dependencies.json").write_text(json.dumps(self.lock))
        self.configure("-DMOSAICO_GAME_HOST_ONLY=ON")
        self.assertFalse((self.build / "_deps/mosaico_game_utils-src").exists())


if __name__ == "__main__":
    unittest.main()
