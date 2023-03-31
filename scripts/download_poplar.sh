#!/bin/sh

set -ex

pip install requests

pushd `mktemp -d`
git clone ssh://git@phabricator.sourcevertex.net/diffusion/ARTIFACTORYAPI/artifactory_api.git
cd ./artifactory_api
read -p "Input your account name of https://artifactory.sourcevertex.net [e.g. xxx@graphcore.ai]: " ARTIFACTORY_ACCOUNT
./setup_token.py --username $ARTIFACTORY_ACCOUNT
cd -
git clone ssh://git@phabricator.sourcevertex.net/diffusion/PSETOOLS/pse_tools.git
cd pse_tools/popsdk
./build_and_install.sh

alias popsdk-activate='. /root/.local/bin/popsdk-activate'
alias popsdk-clean='. /root/.local/bin/popsdk-clean'
export PYTHONPATH=$PYTHONPATH:$PWD/pse_tools/popsdk/artifactory_api
export PATH=$PATH:/root/.local/bin
popd
