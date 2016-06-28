#!/bin/bash

set -x

WORKDIR=`pwd`
GPDBTAR=$1
PLCGPPKG=$2
GPDBVER=$3
GPHDFS=$4
TMPDIR=/tmp/localplccopy

# Put GPDB binaries in place to get pg_config
cp $GPDBTAR/$GPDBTAR.tar.gz /usr/local
pushd /usr/local
tar zxvf $GPDBTAR.tar.gz
if [ "$GPHDFS" != "none" ]; then
    cp $WORKDIR/$GPHDFS/gphdfs.so /usr/local/greenplum-db/lib/postgresql/gphdfs.so
fi
popd
source /usr/local/greenplum-db/greenplum_path.sh || exit 1

# GPDB Installation Preparation
mkdir /data
source plcontainer_src/concourse/scripts/gpdb_install_functions.sh || exit 1
setup_gpadmin_user
setup_sshd

# GPDB Installation
cp plcontainer_src/concourse/scripts/*.sh /tmp
chmod 777 /tmp/*.sh
runuser gpadmin -c "source /usr/local/greenplum-db/greenplum_path.sh && bash /tmp/gpdb_install.sh /data" || exit 1

# Preparing for Docker and starting it
source plcontainer_src/concourse/scripts/docker_scripts.sh
start_docker || exit 1
echo 'agrishchenko@pivotal.io\n\n' | docker login -u agrishchenko -p MyDockerPassword9283 || exit 1

# Pulling new images from Docker Hub
docker pull pivotaldata/plcontainer_python:devel || exit 1
docker pull pivotaldata/plcontainer_r:devel || exit 1

runuser gpadmin -c "bash /tmp/plcontainer_install_endurance_test.sh $WORKDIR $PLCGPPKG $TMPDIR $GPDBVER"
RETCODE=$?

if [ $RETCODE -ne 0 ]; then
    echo "PL/Container endurance test failed!"
else
    echo "PL/Container endurance test succeeded"
fi

stop_docker || exit 1

exit $RETCODE