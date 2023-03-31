#!/bin/sh

set -e

pushd `mktemp -d`
git clone ssh://git@phabricator.sourcevertex.net/diffusion/ARTIFACTORYAPI/artifactory_api.git
cd ./artifactory_api
read -p "Input your account name of https://artifactory.sourcevertex.net [e.g. xxx@graphcore.ai]: " ARTIFACTORY_ACCOUNT
./setup_token.py --username $ARTIFACTORY_ACCOUNT
cd -
git clone ssh://git@phabricator.sourcevertex.net/diffusion/PSETOOLS/pse_tools.git
cd pse_tools/popsdk
./build_and_install.sh
popd

source ~/.bashrc

#popsdk-download $1
