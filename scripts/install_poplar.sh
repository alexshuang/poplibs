#!/bin/sh

set -ex

SRC=$1
DIR=${1%/*}
if [ x"$DIR" == x"$SRC" ]; then
  DIR=./
fi
FILE=${1##*/}

cd $DIR
tar xf $FILE
cd poplar*/poplar*
echo -e "\nexport POPLAR_INSTALL_DIR=$PWD\n" >> ~/.bashrc
echo "unset POPLAR_SDK_ENABLED" >> ~/.bashrc
echo "source \$POPLAR_INSTALL_DIR/enable.sh" >> ~/.bashrc
echo -e "\nexport IPUOF_VIPU_API_PARTITION_ID=p64" >> ~/.bashrc
echo "export IPUOF_VIPU_API_HOST=$HOSTNAME" >> ~/.bashrc
source ~/.bashrc

