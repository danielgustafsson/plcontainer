#!/bin/bash

export PATH=/usr/local/curl/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/curl/lib:$LD_LIBRARY_PATH

# Prepare target directory for GPDB installation
sudo rm -rf /usr/local/greenplum-db-4.3.99.0dev
sudo mkdir /usr/local/greenplum-db-4.3.99.0dev
sudo chown -R vagrant:vagrant /usr/local/greenplum-db-4.3.99.0dev
sudo rm -rf /usr/local/greenplum-db
sudo ln -s /usr/local/greenplum-db-4.3.99.0dev /usr/local/greenplum-db 

# Get the GPDB from github
sudo rm -rf /gpdb
sudo mkdir /gpdb
sudo chown -R vagrant:vagrant /gpdb
git clone https://github.com/greenplum-db/gpdb.git /gpdb

# Build GPDB
cd /gpdb
export BLD_ARCH=rhel5_x86_64
./configure --prefix=/usr/local/greenplum-db --enable-depend --enable-debug --with-python --with-libxml || exit 1
make || exit 1
make install || exit 1