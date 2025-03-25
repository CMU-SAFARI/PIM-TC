DPU_DIR := dpu
HOST_DIR := host
BUILDDIR ?= bin
#The number of tasklets must be a power of two, otherwise it is necessary to change the number of splits in quicksort.h
NR_TASKLETS ?= 16
NR_DPUS ?= 10
NR_THREADS ?= 8

define conf_filename
	${BUILDDIR}/.NR_DPUS_$(1)_NR_TASKLETS_$(2)_NR_THREADS_$(3).conf
endef
CONF := $(call conf_filename,${NR_DPUS},${NR_TASKLETS},${NR_THREADS})

HOST_TARGET := ${BUILDDIR}/app
DPU_TARGET := ${BUILDDIR}/task

COMMON_INCLUDES := common
HOST_SOURCES := $(wildcard ${HOST_DIR}/*.c)
DPU_SOURCES := $(wildcard ${DPU_DIR}/*.c)

DPU_LIB := `dpu-pkg-config --cflags --libs dpu`

.PHONY: all clean test

__dirs := $(shell mkdir -p ${BUILDDIR})

COMMON_FLAGS := -Wall -Wextra -g -I${COMMON_INCLUDES}
HOST_FLAGS := ${COMMON_FLAGS} -std=gnu17 -O3 -march=native -lm -pthread ${DPU_LIB} -DNR_TASKLETS=${NR_TASKLETS} -DNR_DPUS=${NR_DPUS} -DNR_THREADS=${NR_THREADS}
DPU_FLAGS := ${COMMON_FLAGS} -O2 -DSTACK_SIZE_DEFAULT=768 -DNR_TASKLETS=${NR_TASKLETS} -DNR_DPUS=${NR_DPUS}

all: ${HOST_TARGET} ${DPU_TARGET}

${CONF}:
	$(RM) $(call conf_filename,*,*)
	touch ${CONF}

${HOST_TARGET}: ${HOST_SOURCES} ${COMMON_INCLUDES} ${CONF}
	$(CC) -o $@ ${HOST_SOURCES} ${HOST_FLAGS}

${DPU_TARGET}: ${DPU_SOURCES} ${COMMON_INCLUDES} ${CONF}
	dpu-upmem-dpurte-clang ${DPU_FLAGS} -o $@ ${DPU_SOURCES}

clean:
	$(RM) -r $(BUILDDIR)

test: all
	./${HOST_TARGET}
