#!/usr/bin/env python
#
# Copyright 2016 Google Inc.
#
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.


"""Create the asset."""


from __future__ import print_function
import argparse
import glob
import os
import shutil
import subprocess
import sys


def create_asset(target_dir, gl_path):
  """Create the asset."""

  cmd = [
    'sudo','apt-get','install',
    'libgles2-mesa-dev',
    'libegl1-mesa-dev'
  ]
  print('About to run:')
  print(' '.join(cmd))
  print('Press Enter to Continue')
  raw_input()
  subprocess.check_call(cmd)


  lib_dir = os.path.join(target_dir, 'lib')
  os.mkdir(lib_dir)

  to_copy = glob.glob(os.path.join(gl_path,'libGL*'))
  to_copy.extend(glob.glob(os.path.join(gl_path,'libEGL*')))
  to_copy.extend(glob.glob(os.path.join(gl_path,'libmali*')))
  for f in to_copy:
    shutil.copy(f, lib_dir)

  include_dir = os.path.join(target_dir, 'include')
  os.mkdir(include_dir)
  shutil.copytree('/usr/include/EGL', os.path.join(include_dir, 'EGL'))
  shutil.copytree('/usr/include/KHR', os.path.join(include_dir, 'KHR'))
  shutil.copytree('/usr/include/GLES2', os.path.join(include_dir, 'GLES2'))
  shutil.copytree('/usr/include/GLES3', os.path.join(include_dir, 'GLES3'))


def main():
  if 'linux' not in sys.platform:
    print('This script only runs on Linux.', file=sys.stderr)
    sys.exit(1)
  parser = argparse.ArgumentParser()
  parser.add_argument('--target_dir', '-t', required=True)
  parser.add_argument('--lib_path', '-l', required=True)
  args = parser.parse_args()
  create_asset(args.target_dir, args.lib_path)


if __name__ == '__main__':
  main()
