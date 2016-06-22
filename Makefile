MODULE_big = plcontainer
PLC_VER=0.1
PLC_REL=2
PLC_CONTAINER_VERSION=1

ifeq ($(CIBUILD),1)
  IMAGE_TAG=devel
else
  IMAGE_TAG=$(PLC_VER).$(PLC_REL)-$(PLC_CONTAINER_VERSION)
endif

# Directories
SRCDIR = ./src
MGMTDIR = ./management
DOCKERFILEDIR = ./dockerfiles
PYCLIENTDIR = src/pyclient/bin
RCLIENTDIR = src/rclient/bin

# Files to build
FILES = $(shell find $(SRCDIR) -not -path "*client*" -type f -name "*.c")
OBJS = $(foreach FILE,$(FILES),$(subst .c,.o,$(FILE)))

PGXS := $(shell pg_config --pgxs)
include $(PGXS)

PLCONTAINERDIR = $(DESTDIR)$(datadir)/plcontainer

all: all-lib

install: all installdirs install-lib install-extra

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(bindir)'
	$(MKDIR_P) '$(PLCONTAINERDIR)'

.PHONY: uninstall
uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(bindir)/plcontainer-config'
	rm -rf '$(PLCONTAINERDIR)'

.PHONY: install-extra
install-extra:
	# Management
	$(INSTALL_PROGRAM) '$(MGMTDIR)/bin/plcontainer-config'               '$(DESTDIR)$(bindir)/plcontainer-config'
	$(INSTALL_DATA)    '$(MGMTDIR)/config/plcontainer_configuration.xml' '$(PLCONTAINERDIR)'
	$(INSTALL_DATA)    '$(MGMTDIR)/sql/plcontainer_install.sql'          '$(PLCONTAINERDIR)'

.PHONY: installcheck
installcheck:
	$(MAKE) -C tests

.PHONY: clients
clients:
	$(MAKE) -C $(SRCDIR)/pyclient
	$(MAKE) -C $(SRCDIR)/rclient

.PHONY: container_r
container_r:
	docker build -f dockerfiles/Dockerfile.R -t pivotaldata/plcontainer_r:$(IMAGE_TAG) .

.PHONY: container_python
container_python:
	docker build -f dockerfiles/Dockerfile.python -t pivotaldata/plcontainer_python:$(IMAGE_TAG) .

.PHONY: container_r_shared
container_r_shared:
	docker build -f dockerfiles/Dockerfile.R.shared -t pivotaldata/plcontainer_r_shared:$(IMAGE_TAG) .

.PHONY: container_python_shared
container_python_shared:
	docker build -f dockerfiles/Dockerfile.python.shared -t pivotaldata/plcontainer_python_shared:$(IMAGE_TAG) .

.PHONY: container_anaconda
container_anaconda:
	docker build -f dockerfiles/Dockerfile.python.anaconda -t pivotaldata/plcontainer_anaconda:$(IMAGE_TAG) .

.PHONY: containersonly
containersonly: container_r container_python container_r_shared container_python_shared container_anaconda

.PHONY: containers
containers: clients containersonly

.PHONY: cleancontainers
cleancontainers:
	-docker rmi -f pivotaldata/plcontainer_python:$(IMAGE_TAG)
	-docker rmi -f pivotaldata/plcontainer_r:$(IMAGE_TAG)
	-docker rmi -f pivotaldata/plcontainer_r_shared:$(IMAGE_TAG)
	-docker rmi -f pivotaldata/plcontainer_python_shared:$(IMAGE_TAG)
	-docker rmi -f pivotaldata/plcontainer_anaconda:$(IMAGE_TAG)
	-docker ps -a | grep -v CONTAINER | awk '{ print $$1}' | xargs -i docker rm {}

.PHONY: basecontainers
basecontainers:
	-docker rmi -f pivotaldata/plcontainer_r_base:0.1
	-docker rmi -f pivotaldata/plcontainer_anaconda_base:0.1
	docker pull pivotaldata/plcontainer_r_base:0.1
	docker pull pivotaldata/plcontainer_anaconda_base:0.1