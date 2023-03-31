#!/bin/bash

NAME=$1
IMG=$2

gc-docker -- -it \
		   -v /localdata/cn-customer-engineering/$USER:/work \
		   --privileged --cap-add=SYS_PTRACE \
		   --name $NAME \
		   $IMG
