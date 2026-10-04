#!/usr/bin/env python3
"""Regression checks for dependency/symbol evidence parsing."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('gate', Path(__file__).resolve().parents[1] / 'scripts/check_product_no_raylib.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class GateTests(unittest.TestCase):
    def test_product_objects_only(self):
        self.assertEqual(gate.graph_objects('"1" [label="dir/a.cpp.o"]\n"2" [label="liba.a"]\n"3" [label="dir/a.cpp.o"]'), ['dir/a.cpp.o'])

    def test_raylib_paths(self):
        for path in ('/deps/raylib-src/src/raylib.h', ' /deps/raylib-build/libraylib.a ', 'raylib', '/include/rlgl.h', '/include/raymath.h'):
            self.assertIsNotNone(gate.RAYLIB_PATH.search(path), path)
        self.assertIsNone(gate.RAYLIB_PATH.search('/scripts/check_product_no_raylib.py'))

    def test_defined_and_undefined_symbols(self):
        self.assertEqual(gate.forbidden_symbols('000000 T _InitAudioDevice\n U _UpdateTexture\n000 T _glfwInit\n000 T _SDL_InitAudio\n000 T __ZN3rrv3fooEv'), ['InitAudioDevice', 'UpdateTexture', 'glfwInit'])


if __name__ == '__main__':
    unittest.main()
