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

docker load -i plcontainer_devel_images/plcontainer-devel-images.tar.gz

runuser gpadmin -c "bash /tmp/plcontainer_install_test.sh $WORKDIR $PLCGPPKG $TMPDIR $GPDBVER"
RETCODE=$?

if [ $RETCODE -ne 0 ]; then
    echo "PL/Container test failed"
    echo "====================================================================="
    echo "========================= REGRESSION DIFFS =========================="
    echo "====================================================================="
    cat $TMPDIR/tests/regression.out
    cat $TMPDIR/tests/regression.diffs
    echo "====================================================================="
    echo "============================== RESULTS =============================="
    echo "====================================================================="
    cat $TMPDIR/tests/results/plcontainer_test_python.out
    cat $TMPDIR/tests/results/plcontainer_test_anaconda.out
    cat $TMPDIR/tests/results/plcontainer_test_r.out
else
    echo "PL/Container test succeeded"
fi

stop_docker || exit 1

exit $RETCODE