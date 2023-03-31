#!/bin/sh

mkdir -p deps

set -ex

export ZOLTAN_INSTALL_DIR=$(pwd)/deps/zoltan-3.8.3/install_dir/
export SPDLOG_INSTALL_DIR=$(pwd)/deps/spdlog-1.8.0/install_dir/
export BOOST_INSTALL_DIR=$(pwd)/deps/boost_1_70_0/install_dir/

pushd `mktemp -d`
wget https://boostorg.jfrog.io/artifactory/main/release/1.70.0/source/boost_1_70_0.tar.bz2
tar xf boost_1_70_0.tar.bz2
cd boost_1_70_0
./bootstrap.sh --prefix=$BOOST_INSTALL_DIR
./b2 link=static runtime-link=static --abbreviate-paths variant=release toolset=gcc "cxxflags= -fno-semantic-interposition -fPIC" cxxstd=14 --with=all install
popd

pushd `mktemp -d`
git clone --branch v1.8.0 https://github.com/gabime/spdlog.git
cd spdlog && mkdir build && cd build
cmake .. -GNinja -DCMAKE_INSTALL_PREFIX=$SPDLOG_INSTALL_DIR && cmake --build . --target install
popd

pushd `mktemp -d`
wget https://github.com/sandialabs/Zoltan/archive/refs/tags/v3.83.tar.gz
tar xvf v3.83.tar.gz
cd Zoltan-3.83
mkdir build
cd build
../configure --disable-mpi --disable-zoltan-cppdriver --with-cflags='-fPIC' --with-cxxflags='-fPIC' --disable-tests  --disable-zoltan-tests --prefix=$ZOLTAN_INSTALL_DIR
make -j$(nproc)
make install
popd

echo "export ZOLTAN_INSTALL_DIR=${ZOLTAN_INSTALL_DIR}" >> ~/.bashrc
echo "export SPDLOG_INSTALL_DIR=${SPDLOG_INSTALL_DIR}" >> ~/.bashrc
echo "export BOOST_INSTALL_DIR=${BOOST_INSTALL_DIR}" >> ~/.bashrc
source ~/.bashrc
