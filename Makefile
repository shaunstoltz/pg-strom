#
# Common definitions for PG-Strom Makefile
#
PG_CONFIG := pg_config
PSQL := $(shell dirname $(shell which $(PG_CONFIG)))/psql
CREATEDB := $(shell dirname $(shell which $(PG_CONFIG)))/createdb
MKDOCS := mkdocs

ifndef STROM_BUILD_ROOT
STROM_BUILD_ROOT = .
endif

# Custom configurations if any
-include $(STROM_BUILD_ROOT)/Makefile.custom

#
# PG-Strom version
#
PGSTROM_VERSION := 2.3
PGSTROM_RELEASE := devel

#
# Installation related
#
__PGSTROM_SQL = pg_strom--2.2.sql pg_strom--2.2--2.3.sql
PGSTROM_SQL := $(addprefix $(STROM_BUILD_ROOT)/sql/, $(__PGSTROM_SQL))

#
# Source file of CPU portion
#
__STROM_OBJS = main.o nvrtc.o shmbuf.o codegen.o datastore.o \
        cuda_program.o gpu_device.o gpu_context.o gpu_mmgr.o \
        nvme_strom.o relscan.o gpu_tasks.o \
        gpuscan.o gpujoin.o inners.o gpupreagg.o \
		aggfuncs.o pl_cuda.o gstore_buf.o gstore_fdw.o \
		arrow_fdw.o arrow_nodes.o arrow_write.o arrow_pgsql.o \
		matrix.o float2.o largeobject.o misc.o
__STROM_HEADERS = pg_strom.h nvme_strom.h arrow_defs.h \
		device_attrs.h cuda_filelist
__PLCUDA_HOST = host_plcuda.o
STROM_OBJS = $(addprefix $(STROM_BUILD_ROOT)/src/, $(__STROM_OBJS) $(__PLCUDA_HOST))
PLCUDA_HOST = $(addprefix $(STROM_BUILD_ROOT)/src/, $(__PLCUDA_HOST:.o=.c))

#
# Source file of GPU portion
#
__GPU_FATBIN := cuda_common cuda_numeric cuda_primitive \
                cuda_timelib cuda_textlib cuda_misclib cuda_jsonlib \
                cuda_gpuscan cuda_gpujoin cuda_gpupreagg cuda_gpusort
__GPU_HEADERS := $(__GPU_FATBIN) cuda_utils cuda_basetype \
                 cuda_rangetype cuda_plcuda arrow_defs
GPU_HEADERS := $(addprefix $(STROM_BUILD_ROOT)/src/, \
               $(addsuffix .h, $(__GPU_HEADERS)))
GPU_FATBIN := $(addprefix $(STROM_BUILD_ROOT)/src/, \
              $(addsuffix .fatbin, $(__GPU_FATBIN)))
GPU_DEBUG_FATBIN := $(GPU_FATBIN:.fatbin=.gfatbin)

# 32k / 128 = 256 threads per SM
MAXREGCOUNT := 128

# MEMO: Some of kernel functions shall be built to launch 1024 threads
# per block, by KERNEL_FUNCTION_MAXTHREADS(). It saves usage of registers
# per thread. Right now, NVCC/NVRTC configures 32x1024 = 32k registers per SM.
# Our logic can be improved in the furture version regardless of the block-
# size, however, we use 32 registers per thread is a safety configuration for
# all the run-time build.


#
# Source file of utilities
#
__STROM_UTILS = gpuinfo pg2arrow dbgen-ssbm testapp_largeobject
STROM_UTILS = $(addprefix $(STROM_BUILD_ROOT)/utils/, $(__STROM_UTILS))

GPUINFO := $(STROM_BUILD_ROOT)/utils/gpuinfo
GPUINFO_SOURCE := $(STROM_BUILD_ROOT)/utils/gpuinfo.c
GPUINFO_DEPEND := $(GPUINFO_SOURCE) \
                  $(STROM_BUILD_ROOT)/src/nvme_strom.h
GPUINFO_CFLAGS = $(PGSTROM_FLAGS) -I $(IPATH) -L $(LPATH)

PG2ARROW = $(STROM_BUILD_ROOT)/utils/pg2arrow
PG2ARROW_SOURCE = $(STROM_BUILD_ROOT)/utils/pg2arrow.c \
                  $(STROM_BUILD_ROOT)/src/arrow_nodes.c \
                  $(STROM_BUILD_ROOT)/src/arrow_write.c \
                  $(STROM_BUILD_ROOT)/src/arrow_pgsql.c
PG2ARROW_DEPEND = $(PG2ARROW_SOURCE) \
                  $(STROM_BUILD_ROOT)/src/arrow_defs.h \
                  $(STROM_BUILD_ROOT)/src/arrow_ipc.h
PG2ARROW_CFLAGS = -D__PG2ARROW__=1 -D_GNU_SOURCE -g -Wall \
			-I $(STROM_BUILD_ROOT)/src \
			-I $(STROM_BUILD_ROOT)/utils \
			-I $(shell $(PG_CONFIG) --includedir) \
			-I $(shell $(PG_CONFIG) --includedir-server) \
			-L $(shell $(PG_CONFIG) --libdir)

SSBM_DBGEN = $(STROM_BUILD_ROOT)/utils/dbgen-ssbm
__SSBM_DBGEN_SOURCE = bcd2.c  build.c load_stub.c print.c text.c \
		bm_utils.c driver.c permute.c rnd.c speed_seed.c dists.dss.h
SSBM_DBGEN_SOURCE = $(addprefix $(STROM_BUILD_ROOT)/utils/ssbm/, \
                                $(__SSBM_DBGEN_SOURCE))
SSBM_DBGEN_DISTS_DSS = $(STROM_BUILD_ROOT)/utils/ssbm/dists.dss.h
SSBM_DBGEN_CFLAGS = -DDBNAME=\"dss\" -DLINUX -DDB2 -DSSBM -DTANDEM \
                    -DSTATIC_DISTS=1 \
                    -O2 -g -I. -I$(STROM_BUILD_ROOT)/utils/ssbm
__SSBM_SQL_FILES = ssbm-11.sql ssbm-12.sql ssbm-13.sql \
                   ssbm-21.sql ssbm-22.sql ssbm-23.sql \
                   ssbm-31.sql ssbm-32.sql ssbm-33.sql ssbm-34.sql \
                   ssbm-41.sql ssbm-42.sql ssbm-43.sql
TESTAPP_LARGEOBJECT = $(STROM_BUILD_ROOT)/utils/testapp_largeobject
TESTAPP_LARGEOBJECT_SOURCE = $(TESTAPP_LARGEOBJECT).cu

#
# Markdown (document) files
#
__DOC_FILES = index.md install.md partition.md \
              operations.md sys_admin.md brin.md partition.md troubles.md \
	      ssd2gpu.md arrow_fdw.md gstore_fdw.md plcuda.md \
	      ref_types.md ref_devfuncs.md ref_sqlfuncs.md ref_params.md \
	      release_note.md

#
# Files to be packaged
#
__PACKAGE_FILES = LICENSE README.md Makefile pg_strom.control	\
	          src sql utils test man
ifdef PGSTROM_VERSION
ifeq ($(PGSTROM_RELEASE),1)
__STROM_TGZ = pg_strom-$(PGSTROM_VERSION)
else
__STROM_TGZ = pg_strom-$(PGSTROM_VERSION)-$(PGSTROM_RELEASE)
endif
else
__STROM_TGZ = pg_strom-master
endif
STROM_TGZ = $(addprefix $(STROM_BUILD_ROOT)/, $(__STROM_TGZ).tar.gz)
ifdef PGSTROM_GITHASH
__STROM_TGZ_GITHASH = $(PGSTROM_GITHASH)
else
__STROM_TGZ_GITHASH = HEAD
endif

__SPECFILE = pg_strom-PG$(MAJORVERSION)

#
# Header and Libraries of CUDA
#
CUDA_PATH_LIST := /usr/local/cuda /usr/local/cuda-*
CUDA_PATH := $(shell for x in $(CUDA_PATH_LIST);    \
           do test -e "$$x/include/cuda.h" && echo $$x; done | head -1)
IPATH := $(CUDA_PATH)/include
BPATH := $(CUDA_PATH)/bin
LPATH := $(CUDA_PATH)/lib64
NVCC  := $(CUDA_PATH)/bin/nvcc
CUDA_VERSION := $(shell grep -E '^\#define[ ]+CUDA_VERSION[ ]+[0-9]+$$' $(IPATH)/cuda.h | awk '{print $$3}')

#
# Flags to build
# --------------
# NOTE: we assume to put the following line in Makefile.custom according to
#       the purpose of this build
#
#       PGSTROM_FLAGS_CUSTOM := -g -O0 -Werror
#
PGSTROM_FLAGS += $(PGSTROM_FLAGS_CUSTOM)
ifdef PGSTROM_VERSION
PGSTROM_FLAGS += "-DPGSTROM_VERSION=\"$(PGSTROM_VERSION)\""
endif
PGSTROM_FLAGS += -DCPU_ARCH=\"$(shell uname -m)\"
PGSTROM_FLAGS += -DPGSHAREDIR=\"$(shell $(PG_CONFIG) --sharedir)\"
PGSTROM_FLAGS += -DPGSERV_INCLUDEDIR=\"$(shell $(PG_CONFIG) --includedir-server)\"
PGSTROM_FLAGS += -DCUDA_INCLUDE_PATH=\"$(IPATH)\"
PGSTROM_FLAGS += -DCUDA_BINARY_PATH=\"$(BPATH)\"
PGSTROM_FLAGS += -DCUDA_LIBRARY_PATH=\"$(LPATH)\"
PGSTROM_FLAGS += -DCUDA_MAXREGCOUNT=$(MAXREGCOUNT)
PGSTROM_FLAGS += -DCMD_GPUINFO_PATH=\"$(shell $(PG_CONFIG) --bindir)/gpuinfo\"
PG_CPPFLAGS := $(PGSTROM_FLAGS) -I $(IPATH)
SHLIB_LINK := -L $(LPATH) -lcuda

# also, flags to build GPU libraries
NVCC_FLAGS := $(NVCC_FLAGS_CUSTOM)
NVCC_FLAGS += -I $(shell $(PG_CONFIG) --includedir-server) \
              --fatbin --relocatable-device-code=true \
              --maxrregcount=$(MAXREGCOUNT) \
              --gpu-architecture=compute_60
# supported device depends on CUDA version
ifeq ($(shell test $(CUDA_VERSION) -ge 10000; echo $$?), 0)
  NVCC_FLAGS += --gpu-code=sm_60,sm_61,sm_70,sm_75
else ifeq ($(shell test $(CUDA_VERSION) -ge 9000; echo $$?), 0)
  NVCC_FLAGS += --gpu-code=sm_60,sm_61,sm_70
else
  NVCC_FLAGS += --gpu-code=sm_60,sm_61
endif
NVCC_DEBUG_FLAGS := $(NVCC_FLAGS) --source-in-ptx --device-debug

#
# Definition of PG-Strom Extension
#
MODULE_big = pg_strom
MODULEDIR = pg_strom
OBJS =  $(STROM_OBJS)
EXTENSION = pg_strom
DATA = $(GPU_HEADERS) $(PGSTROM_SQL)
DATA_built = $(GPU_FATBIN) $(GPU_DEBUG_FATBIN)

# Support utilities
SCRIPTS_built = $(STROM_UTILS)
# Extra files to be cleaned
EXTRA_CLEAN = $(STROM_UTILS) $(PLCUDA_HOST) \
	$(shell ls $(STROM_BUILD_ROOT)/man/docs/*.md 2>/dev/null) \
	$(shell ls */Makefile 2>/dev/null | sed 's/Makefile/pg_strom.control/g') \
	$(shell ls pg-strom-*.tar.gz 2>/dev/null) \
	$(STROM_BUILD_ROOT)/man/markdown_i18n \
	$(SSBM_DBGEN_DISTS_DSS) \
	$(TESTAPP_LARGEOBJECT)

#
# Regression Test
#
USE_MODULE_DB := 1
REGRESS := --schedule=$(STROM_BUILD_ROOT)/test/parallel_schedule
REGRESS_INIT_SQL := $(STROM_BUILD_ROOT)/test/sql/init_regress.sql
REGRESS_DBNAME := contrib_regression_$(MODULE_big)
REGRESS_REVISION := 20191112
REGRESS_REVISION_QUERY := 'SELECT pgstrom.regression_testdb_revision() = $(REGRESS_REVISION)'
REGRESS_OPTS = --inputdir=$(STROM_BUILD_ROOT)/test \
               --outputdir=$(STROM_BUILD_ROOT)/test \
               --encoding=UTF-8 \
               --load-extension=pg_strom \
               --launcher="env PGDATABASE=$(REGRESS_DBNAME) PATH=$(shell dirname $(SSBM_DBGEN)):$$PATH PGAPPNAME=$(REGRESS_REVISION)" \
               $(shell test "`$(PSQL) -At -c $(REGRESS_REVISION_QUERY) $(REGRESS_DBNAME)`" = "t" && echo "--use-existing")
REGRESS_PREP = $(SSBM_DBGEN) $(TESTAPP_LARGEOBJECT) $(REGRESS_INIT_SQL)

#
# Build chain of PostgreSQL
#
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifneq ($(STROM_BUILD_ROOT), .)
pg_strom.control: $(addprefix $(STROM_BUILD_ROOT)/, pg_strom.control)
	cp -f $< $@
endif

#
# GPU Libraries
#
%.fatbin:  %.cu $(GPU_HEADERS)
	$(NVCC) $(NVCC_FLAGS) -o $@ $<

%.gfatbin: %.cu $(GPU_HEADERS)
	$(NVCC) $(NVCC_DEBUG_FLAGS) -o $@ $<

# PL/CUDA Host Template
$(PLCUDA_HOST): $(PLCUDA_HOST:.c=.cu)
	@(echo "const char *pgsql_host_plcuda_code =";			\
	  sed -e 's/\\/\\\\/g' -e 's/\t/\\t/g' -e 's/"/\\"/g'	\
	      -e 's/^/  "/g'   -e 's/$$/\\n"/g' < $*.cu;		\
	  echo ";") > $@

#
# Build documentation
#
$(STROM_BUILD_ROOT)/man/markdown_i18n: $(STROM_BUILD_ROOT)/man/markdown_i18n.c
	$(CC) $(CFLAGS) -o $@ $(addsuffix .c,$@)

docs:	$(STROM_BUILD_ROOT)/man/markdown_i18n
	for x in $(__DOC_FILES);			\
	do						\
	  $(STROM_BUILD_ROOT)/man/markdown_i18n		\
	    -f $(STROM_BUILD_ROOT)/man/$$x		\
	    -o $(STROM_BUILD_ROOT)/man/docs/$$x;	\
	done
	$(STROM_BUILD_ROOT)/man/markdown_i18n		\
	    -f $(STROM_BUILD_ROOT)/man/mkdocs.yml	\
	    -o $(STROM_BUILD_ROOT)/man/mkdocs.en.yml
	pushd $(STROM_BUILD_ROOT)/man;			\
	env LANG=en_US.utf8				\
		$(MKDOCS) build -c -f mkdocs.en.yml -d ../docs;	\
	popd
	for x in $(__DOC_FILES);			\
	do						\
	  $(STROM_BUILD_ROOT)/man/markdown_i18n -l ja	\
	    -f $(STROM_BUILD_ROOT)/man/$$x		\
	    -o $(STROM_BUILD_ROOT)/man/docs/$$x;	\
	done
	$(STROM_BUILD_ROOT)/man/markdown_i18n -l ja	\
	    -f $(STROM_BUILD_ROOT)/man/mkdocs.yml	\
	    -o $(STROM_BUILD_ROOT)/man/mkdocs.ja.yml
	pushd $(STROM_BUILD_ROOT)/man;			\
	env LANG=ja_JP.utf8			 	\
		$(MKDOCS) build -c -f mkdocs.ja.yml -d ../docs/ja; \
	popd

#
# Build utilities
#
$(GPUINFO): $(GPUINFO_DEPEND)
	$(CC) $(GPUINFO_CFLAGS) $(GPUINFO_SOURCE) -o $@ -lcuda

$(PG2ARROW): $(PG2ARROW_DEPEND)
	$(CC) $(PG2ARROW_CFLAGS) $(PG2ARROW_SOURCE) -o $@ -lpq -lpgcommon -lpgport

$(SSBM_DBGEN): $(SSBM_DBGEN_SOURCE) $(SSBM_DBGEN_DISTS_DSS)
	$(CC) $(SSBM_DBGEN_CFLAGS) $(SSBM_DBGEN_SOURCE) -o $@ -lm

$(SSBM_DBGEN_DISTS_DSS): $(basename $(SSBM_DBGEN_DISTS_DSS))
	@(echo "const char *static_dists_dss ="; \
	  sed -e 's/\\/\\\\/g' -e 's/\t/\\t/g' -e 's/"/\\"/g' \
	      -e 's/^/  "/g' -e 's/$$/\\n"/g' < $^; \
	  echo ";") > $@

$(TESTAPP_LARGEOBJECT): $(TESTAPP_LARGEOBJECT_SOURCE)
	$(NVCC) -I $(shell $(PG_CONFIG) --pkgincludedir) \
	        -L $(shell $(PG_CONFIG) --pkglibdir) \
	        -Xcompiler \"-Wl,-rpath,$(shell $(PG_CONFIG) --pkglibdir)\" \
	        -lpq -o $@ $^

#
# Tarball
#
$(STROM_TGZ):
	(cd $(STROM_BUILD_ROOT);                 \
	 git archive	--format=tar.gz          \
			--prefix=$(__STROM_TGZ)/ \
			-o $(__STROM_TGZ).tar.gz \
			$(__STROM_TGZ_GITHASH) $(__PACKAGE_FILES))

tarball: $(STROM_TGZ)

#
# RPM Package
#
rpm: tarball
	cp -f $(STROM_TGZ) `rpmbuild -E %{_sourcedir}` || exit 1
	git show --format=raw $(__STROM_TGZ_GITHASH):$(STROM_BUILD_ROOT)/files/systemd-pg_strom.conf > `rpmbuild -E %{_sourcedir}`/systemd-pg_strom.conf || exit 1
	git show --format=raw $(__STROM_TGZ_GITHASH):$(STROM_BUILD_ROOT)/files/pg_strom.spec.in | \
	sed -e "s/@@STROM_VERSION@@/$(PGSTROM_VERSION)/g" \
	    -e "s/@@STROM_RELEASE@@/$(PGSTROM_RELEASE)/g" \
	    -e "s/@@STROM_TARBALL@@/$(__STROM_TGZ)/g"     \
	    -e "s/@@PGSQL_VERSION@@/$(MAJORVERSION)/g"    \
	> `rpmbuild -E %{_specdir}`/pg_strom-PG$(MAJORVERSION).spec
	rpmbuild -ba `rpmbuild -E %{_specdir}`/pg_strom-PG$(MAJORVERSION).spec

.PHONY: docs
