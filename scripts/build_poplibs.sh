#!/bin/sh

git clone https://github.com/graphcore/poplibs
cd poplibs && mkdir build install
cd build
cmake ../ -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../install -DBOOST_ROOT=$BOOST_INSTALL_DIR -GNinja -DCMAKE_PREFIX_PATH=$SPDLOG_INSTALL_DIR -DZOLTAN_ROOT=$ZOLTAN_INSTALL_DIR
ninja -j$(nproc)
