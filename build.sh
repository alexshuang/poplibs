#!/bin/sh

set -ex

cd build
ninja -j64 install
