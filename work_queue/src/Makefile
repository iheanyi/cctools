CCTOOLS_HOME = ../..

include ${CCTOOLS_HOME}/Makefile.config
include ${CCTOOLS_HOME}/Makefile.rules

WORK_QUEUE_LDFLAGS = -lwork_queue -lchirp -ldttools ${CCTOOLS_INTERNAL_LDFLAGS}

SOURCES =   batch_job.c  \
	    batch_job_chirp.c  \
	    batch_job_condor.c  \
	    batch_job_hadoop.c  \
	    batch_job_local.c  \
	    batch_job_mpi_queue.c  \
	    batch_job_cluster.c  \
	    batch_job_work_queue.c \
	    work_queue_catalog.c   \
	    work_queue_resources.c \
	    work_queue_status.c    \
	    work_queue.c

PUBLIC_HEADERS = batch_job.h work_queue.h

LIBDTTOOLS = ${CCTOOLS_HOME}/dttools/src/libdttools.a
LIBCHIRP = $(CCTOOLS_HOME)/chirp/src/libchirp.a
EXTERNALS = $(LIBDTTOOLS) $(LIBCHIRP)

OBJECTS = ${SOURCES:%.c=%.o}
HEADERS = ${SOURCES:%.c=%.h} ${EXTRA_HEADERS}
LIBRARIES = libwork_queue.a
PROGRAMS = work_queue_worker work_queue_status work_queue_example work_queue_pool

ifeq (${CCTOOLS_USE_MPI},true)
	PROGRAMS += mpi_queue_worker
endif

PROGRAM_SOURCES = ${PROGRAMS:%=%.c} ${TEST_PROGRAMS:%=%.c}

SCRIPTS = wq_submit_workers.common condor_submit_workers sge_submit_workers torque_submit_workers pbs_submit_workers ec2_submit_workers ec2_remove_workers slurm_submit_workers work_queue_graph_log

TEST_PROGRAMS = work_queue_example work_queue_workload_simulator

all: ${LIBRARIES} $(PROGRAMS) $(TEST_PROGRAMS) bindings


work_queue_worker: work_queue_worker.o ${EXTERNALS} libwork_queue.a
	${CCTOOLS_LD} work_queue_worker.o $(WORK_QUEUE_LDFLAGS) -o $@

mpi_queue_worker: mpi_queue_worker.o ${EXTERNALS} libwork_queue.a
	${CCTOOLS_MPI_LD} mpi_queue_worker.o $(WORK_QUEUE_LDFLAGS) -o $@

work_queue_status: work_queue_status.o ${EXTERNALS} libwork_queue.a
	${CCTOOLS_LD} work_queue_status.o $(WORK_QUEUE_LDFLAGS) -o $@

work_queue_example: work_queue_example.o ${EXTERNALS} libwork_queue.a
	${CCTOOLS_LD} work_queue_example.o $(WORK_QUEUE_LDFLAGS) -o $@

work_queue_workload_simulator: work_queue_workload_simulator.o ${EXTERNALS} libwork_queue.a
	${CCTOOLS_LD} work_queue_workload_simulator.o $(WORK_QUEUE_LDFLAGS) -o $@

work_queue_pool: work_queue_pool.o ${EXTERNALS} libwork_queue.a
	${CCTOOLS_LD} work_queue_pool.o $(WORK_QUEUE_LDFLAGS) -o $@

libwork_queue.a: ${OBJECTS}
	${CCTOOLS_AR} rv $@ $^
	ranlib $@

#phony gymnastics to stop compilation if one of the submakes fail
CCTOOLS_WORK_QUEUE_BINDINGS_INSTALL =  $(CCTOOLS_WORK_QUEUE_BINDINGS:%=install-%)
CCTOOLS_WORK_QUEUE_BINDINGS_CLEAN   =  $(CCTOOLS_WORK_QUEUE_BINDINGS:%=clean-%)
.PHONY: ${CCTOOLS_WORK_QUEUE_BINDINGS} ${CCTOOLS_WORK_QUEUE_BINDINGS_INSTALL} bindings install-bindings clean-bindings

bindings: ${CCTOOLS_WORK_QUEUE_BINDINGS}
${CCTOOLS_WORK_QUEUE_BINDINGS}: libwork_queue.a work_queue.i
	${MAKE} -C $@ all

install-bindings: ${CCTOOLS_WORK_QUEUE_BINDINGS}
${CCTOOLS_WORK_QUEUE_BINDINGS_INSTALL}: ${CCTOOLS_WORK_QUEUE_BINDINGS}
	${MAKE} -C $@ install

clean-bindings:
${CCTOOLS_WORK_QUEUE_BINDINGS_CLEAN}:
	${MAKE} -C $@ clean


install: all install-bindings
	mkdir -p ${CCTOOLS_INSTALL_DIR}/bin
	mkdir -p ${CCTOOLS_INSTALL_DIR}/lib
	mkdir -p ${CCTOOLS_INSTALL_DIR}/doc
	mkdir -p ${CCTOOLS_INSTALL_DIR}/include
	mkdir -p ${CCTOOLS_INSTALL_DIR}/include/cctools
	chmod 755 ${SCRIPTS}
	cp ${LIBRARIES} ${CCTOOLS_INSTALL_DIR}/lib
	cp ${PROGRAMS} ${CCTOOLS_INSTALL_DIR}/bin
	cp ${SCRIPTS} ${CCTOOLS_INSTALL_DIR}/bin
	cp work_queue_example.c ${CCTOOLS_INSTALL_DIR}/doc
	for file in ${PUBLIC_HEADERS} ; do if [ -f $$file ] ; then cp $$file ${CCTOOLS_INSTALL_DIR}/include/cctools ; fi ; done
	for file in ${CYGWINLIB} ; do if [ -f /bin/$$file ] ; then cp /bin/$$file ${CCTOOLS_INSTALL_DIR}/bin/$$file ; fi ; done
	for m in $(CCTOOLS_WORK_QUEUE_BINDINGS); do cd $$m; $(MAKE) $@; cd ..; done

test:

clean: clean-bindings
	rm -fr *.o ${LIBRARIES} ${PROGRAMS} ${TEST_PROGRAMS}
