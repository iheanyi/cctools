/*
Copyright (C) 2008- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

/*
The following major problems must be fixed:
- The capacity code assumes one task per worker.
- The log specification need to be updated.
*/

#include "work_queue.h"
#include "work_queue_protocol.h"
#include "work_queue_internal.h"
#include "work_queue_resources.h"

#include "int_sizes.h"
#include "link.h"
#include "link_auth.h"
#include "debug.h"
#include "stringtools.h"
#include "catalog_query.h"
#include "datagram.h"
#include "domain_name_cache.h"
#include "hash_table.h"
#include "itable.h"
#include "list.h"
#include "macros.h"
#include "username.h"
#include "create_dir.h"
#include "xxmalloc.h"
#include "load_average.h"
#include "buffer.h"
#include "link_nvpair.h"
#include "rmonitor.h"
#include "copy_stream.h"
#include "random_init.h"
#include "process.h"
#include "path.h"
#include "md5.h"

#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <limits.h>

#ifdef CCTOOLS_OPSYS_SUNOS
extern int setenv(const char *name, const char *value, int overwrite);
#endif

// The default capacity reported before information is available.
#define WORK_QUEUE_DEFAULT_CAPACITY 10

// The minimum number of task reports to keep
#define WORK_QUEUE_TASK_REPORT_MIN_SIZE 20

// Seconds between updates to the catalog
#define WORK_QUEUE_UPDATE_INTERVAL 60

#define WORKER_ADDRPORT_MAX 32
#define WORKER_HASHKEY_MAX 32

// Result codes for signaling the completion of operations in WQ
#define SUCCESS 1
#define WORKER_FAILURE 0
#define APP_FAILURE -1

#define RESOURCE_MONITOR_TASK_SUMMARY_NAME "cctools-work-queue-%d-resource-monitor-task-%d"

#define MAX_TASK_STDOUT_STORAGE (1*GIGABYTE)

double wq_option_fast_abort_multiplier = -1.0;
int wq_option_scheduler = WORK_QUEUE_SCHEDULE_TIME;

struct work_queue {
	char *name;
	int port;
	int priority;

	char workingdir[PATH_MAX];

	struct datagram  *update_port;   // outgoing udp connection to catalog
	struct link      *master_link;   // incoming tcp connection for workers.
	struct link_info *poll_table;
	int poll_table_size;

	struct list    *ready_list;      // ready to be sent to a worker
	struct itable  *running_tasks;   // running on a worker
	struct itable  *finished_tasks;  // have output waiting on a worker
	struct list    *complete_list;   // completed and awaiting return to the master process

	struct hash_table *worker_table;
	struct hash_table *worker_blacklist;
	struct itable  *worker_task_map;

	struct hash_table *workers_with_available_results;

	int64_t total_tasks_submitted;
	int64_t total_tasks_complete;
	int64_t total_tasks_cancelled;
	int64_t total_tasks_failed;

	int64_t total_bytes_sent;
	int64_t total_bytes_received;
	int64_t total_workers_joined;
	int64_t total_workers_removed;

	timestamp_t start_time;
	timestamp_t total_send_time;
	timestamp_t total_receive_time;
	timestamp_t total_good_transfer_time;  // send time for tasks with t->result == WQ_RESULT_SUCCESS
	timestamp_t total_execute_time;
	timestamp_t total_good_execute_time;  // execute time for tasks with t->result == WQ_RESULT_SUCCESS

	double fast_abort_multiplier;
	int worker_selection_algorithm;
	int task_ordering;
	int process_pending_check;
	int workers_to_wait;

	int short_timeout;		// timeout to send/recv a brief message from worker
	int long_timeout;		// timeout to send/recv a brief message from a foreman

	struct list * task_reports;	// list of last N work_queue_task_reports
	timestamp_t total_idle_time;	// sum of time spent waiting for workers
	timestamp_t total_app_time;	// sum of time spend above work_queue_wait

	double asynchrony_multiplier;     /* Times the resource value, but disk */
	int    asynchrony_modifier;       /* Plus this many cores or unlabeled tasks */

	int minimum_transfer_timeout;
	int foreman_transfer_timeout;
	int transfer_outlier_factor;
	int default_transfer_rate;

	char *catalog_host;
	int catalog_port;

	FILE *logfile;
	int keepalive_interval;
	int keepalive_timeout;
	timestamp_t link_poll_end;	//tracks when we poll link; used to timeout unacknowledged keepalive checks

	int monitor_mode;
	int monitor_fd;
	char *monitor_exe;

	char *password;
	double bandwidth;
};

struct work_queue_worker {
	char *hostname;
	char *os;
	char *arch;
	char *version;
	char addrport[WORKER_ADDRPORT_MAX];
	char hashkey[WORKER_HASHKEY_MAX];
	int  foreman;                             // 0 if regular worker, 1 if foreman
	struct work_queue_resources *resources;
	int64_t unlabeled_allocated;
	int64_t cores_allocated;
	int64_t memory_allocated;
	int64_t disk_allocated;
	int64_t gpus_allocated;
	struct hash_table *current_files;
	struct link *link;
	struct itable *current_tasks;
	int finished_tasks;
	int64_t total_tasks_complete;
	int64_t total_bytes_transferred;
	timestamp_t total_task_time;
	timestamp_t total_transfer_time;
	timestamp_t start_time;
	timestamp_t last_msg_recv_time;
	timestamp_t keepalive_check_sent_time;
};

struct work_queue_task_report {
	timestamp_t transfer_time;
	timestamp_t exec_time;
};

static void handle_failure(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, int fail_type);
static void handle_worker_failure(struct work_queue *q, struct work_queue_worker *w);
static void handle_app_failure(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t);

static void start_task_on_worker(struct work_queue *q, struct work_queue_worker *w);
static void add_task_report(struct work_queue *q, struct work_queue_task *t );

static void commit_task_to_worker(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t);
static void reap_task_from_worker(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t);

static int process_workqueue(struct work_queue *q, struct work_queue_worker *w, const char *line);
static int process_result(struct work_queue *q, struct work_queue_worker *w, const char *line);
static void process_available_results(struct work_queue *q, struct work_queue_worker *w, int max_count);
static int process_queue_status(struct work_queue *q, struct work_queue_worker *w, const char *line, time_t stoptime);
static int process_resource(struct work_queue *q, struct work_queue_worker *w, const char *line); 

static struct nvpair * queue_to_nvpair( struct work_queue *q, struct link *foreman_uplink );

/** Clone a @ref work_queue_file
This performs a deep copy of the file struct.
@param file The file to clone.
@return A newly allocated file.
*/
static struct work_queue_file *work_queue_file_clone(const struct work_queue_file *file);

/** Clone a list of @ref work_queue_file structs
Thie performs a deep copy of the file list.
@param list The list to clone.
@return A newly allocated list of files.
*/
static struct list *work_queue_task_file_list_clone(struct list *list);


/******************************************************/
/********** work_queue internal functions *************/
/******************************************************/

static int64_t overcommitted_resource_total(struct work_queue *q, int64_t total, int cores_flag) {
	int64_t r = 0;
	if(total > 0)
	{
		r = ceil(total * q->asynchrony_multiplier);

		if(cores_flag)
		{
			r += q->asynchrony_modifier;
		}
	}

	return r;
}

//Returns count of workers that have identified themselves.
static int known_workers(struct work_queue *q) {
	struct work_queue_worker *w;
	char* id;
	int known_workers = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &id, (void**)&w)) {
		if(strcmp(w->hostname, "unknown")){
			known_workers++;
		}	
	}
	
	return known_workers;
}

//Returns count of workers that are available to run tasks.
static int available_workers(struct work_queue *q) {
	struct work_queue_worker *w;
	char* id;
	int available_workers = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &id, (void**)&w)) {
		if(strcmp(w->hostname, "unknown")){
			if(w->unlabeled_allocated > 0)
			{
				if(overcommitted_resource_total(q, w->resources->workers.total, 1) > w->unlabeled_allocated) {
					available_workers++;
				}
			}
			else if(overcommitted_resource_total(q, w->resources->cores.total, 1) > w->cores_allocated || w->resources->disk.total > w->disk_allocated || overcommitted_resource_total(q, w->resources->memory.total, 0) > w->memory_allocated){
				available_workers++;
			}
		}	
	}

	return available_workers;
}

//Returns count of workers that are running at least 1 task.
static int workers_with_tasks(struct work_queue *q) {
	struct work_queue_worker *w;
	char* id;
	int workers_with_tasks = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &id, (void**)&w)) {
		if(strcmp(w->hostname, "unknown")){
			if(itable_size(w->current_tasks)){
				workers_with_tasks++;
			}
		}	
	}
	
	return workers_with_tasks;
}

static void log_worker_stats(struct work_queue *q)
{
	struct work_queue_stats s;
	
	debug(D_WQ, "workers status -- total: %d, active: %d, available: %d.",
		hash_table_size(q->worker_table),
		known_workers(q),
		available_workers(q));
		
	if(!q->logfile) return;
	
	work_queue_get_stats(q, &s);

	//print in the order described by the headers in specify_log().
	fprintf(q->logfile, "%" PRIu64 " ", timestamp_get()); 
	fprintf(q->logfile, "%d %d %d %d %d %d ", s.total_workers_connected, s.workers_init, s.workers_idle, s.workers_busy, s.total_workers_joined, s.total_workers_removed);	
	fprintf(q->logfile, "%d %d %d %d %d %d ", s.tasks_waiting, s.tasks_running, s.tasks_complete, s.total_tasks_dispatched, s.total_tasks_complete, s.total_tasks_cancelled);
	fprintf(q->logfile, "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %f %f %d ", s.start_time, s.total_send_time, s.total_receive_time, s.total_bytes_sent, s.total_bytes_received, s.efficiency, s.idle_percentage, s.capacity);
	fprintf(q->logfile, "%f %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " ", s.bandwidth, s.total_cores, s.total_memory, s.total_disk, s.total_gpus);
	fprintf(q->logfile, "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " ", s.min_cores, s.max_cores, s.min_memory, s.max_memory, s.min_disk, s.max_disk, s.min_gpus, s.max_gpus);
	fprintf(q->logfile, "%" PRIu64 " %" PRIu64 " ", s.total_execute_time, s.total_good_execute_time);
	fprintf(q->logfile, "\n");
}

static void link_to_hash_key(struct link *link, char *key)
{
	sprintf(key, "0x%p", link);
}

/**
 * This function sends a message to the worker and records the time the message is 
 * successfully sent. This timestamp is used to determine when to send keepalive checks.
 */
__attribute__ (( format(printf,3,4) ))
static int send_worker_msg( struct work_queue *q, struct work_queue_worker *w, const char *fmt, ... ) 
{
	char debug_msg[2*WORK_QUEUE_LINE_MAX];
	va_list va;
	va_list debug_va;
	
	va_start(va,fmt);

	time_t stoptime;

	//If foreman, then we wait until foreman gives the master some attention.
	if(w->foreman)
		stoptime = time(0) + q->long_timeout;
	else
		stoptime = time(0) + q->short_timeout;
	
	sprintf(debug_msg, "tx to %s (%s): ", w->hostname, w->addrport);
	strcat(debug_msg, fmt);
	va_copy(debug_va, va);
	vdebug(D_WQ, debug_msg, debug_va);
	
	int result = link_putvfstring(w->link, fmt, stoptime, va);	
	va_end(va);

	return result;  
}

int process_name(struct work_queue *q, struct work_queue_worker *w, char *line)
{
	debug(D_WQ, "Sending project name to worker (%s)", w->addrport);

	//send project name (q->name) if there is one. otherwise send blank line
	send_worker_msg(q, w, "%s\n", q->name ? q->name : "");

	return 0;
}


/**
 * This function receives a message from worker and records the time a message is successfully 
 * received. This timestamp is used in keepalive timeout computations. 
 * Its return value is:
 *  0 : a message was received and processed 
 *  1 : a message was received but NOT processed 
 * -1 : failure to read from link or in processing received message
 */
static int recv_worker_msg(struct work_queue *q, struct work_queue_worker *w, char *line, size_t length ) 
{
	time_t stoptime;
	
	//If foreman, then we wait until foreman gives the master some attention.
	if(w->foreman)
		stoptime = time(0) + q->long_timeout;
	else
		stoptime = time(0) + q->short_timeout;

	int result = link_readline(w->link, line, length, stoptime);
	
	if (result <= 0) {
		return -1;
	}
	
	w->last_msg_recv_time = timestamp_get();

	debug(D_WQ, "rx from %s (%s): %s", w->hostname, w->addrport, line);
	
	// Check for status updates that can be consumed here.
	if(string_prefix_is(line, "alive")) {
		result = 0;	
	} else if(string_prefix_is(line, "workqueue")) {
		result = process_workqueue(q, w, line);
	} else if (string_prefix_is(line,"queue_status") || string_prefix_is(line, "worker_status") || string_prefix_is(line, "task_status")) {
		result = process_queue_status(q, w, line, stoptime);
	} else if (string_prefix_is(line, "available_results")) { 
		hash_table_insert(q->workers_with_available_results, w->hashkey, w);
		result = 0;
	} else if (string_prefix_is(line, "resource")) {
		result = process_resource(q, w, line);
	} else if (string_prefix_is(line, "auth")) {
		debug(D_WQ|D_NOTICE,"worker (%s) is attempting to use a password, but I do not have one.",w->addrport);
		result = -1;
	} else if (string_prefix_is(line,"ready")) {
		debug(D_WQ|D_NOTICE,"worker (%s) is an older worker that is not compatible with this master.",w->addrport);
		result = -1;
	} else if (string_prefix_is(line, "name")) {
		result = process_name(q, w, line);
	} else {
		// Message is not a status update: return it to the user.
		return 1;
	}

	return result; 
}


/*
Call recv_worker_msg and silently retry if the result indicates
an asynchronous update message like 'keepalive' or 'resource'.
Returns 1 or -1 (as above) but does not return zero.
*/

int recv_worker_msg_retry( struct work_queue *q, struct work_queue_worker *w, char *line, int length )
{
	int result=0;

	do {
		result = recv_worker_msg(q, w,line,length);
	} while(result==0);

	return result;
}

static double get_queue_transfer_rate(struct work_queue *q, char **data_source) 
{
	double queue_transfer_rate; // bytes per second
	int64_t     q_total_bytes_transferred = q->total_bytes_sent + q->total_bytes_received;
	timestamp_t q_total_transfer_time     = q->total_send_time  + q->total_receive_time;

	// Note q_total_transfer_time is timestamp_t with units of milliseconds.
	if(q_total_transfer_time>1000000) {
		queue_transfer_rate = 1000000.0 * q_total_bytes_transferred / q_total_transfer_time;
		if (data_source) {
			*data_source = xxstrdup("overall queue");
		}	
	} else {
		queue_transfer_rate = q->default_transfer_rate;
		if (data_source) {
			*data_source = xxstrdup("conservative default");
		}	
	}

	return queue_transfer_rate;
}

/*
Select an appropriate timeout value for the transfer of a certain number of bytes.
We do not know in advance how fast the system will perform.

So do this by starting with an assumption of bandwidth taken from the worker,
from the queue, or from a (slow) default number, depending on what information is available.
The timeout is chosen to be a multiple of the expected transfer time from the assumed bandwidth.

The overall effect is to reject transfers that are 10x slower than what has been seen before.

Two exceptions are made:
- The transfer time cannot be below a configurable minimum time.
- A foreman must have a high minimum, because its attention is divided
  between the master and the workers that it serves.
*/

static timestamp_t get_transfer_wait_time(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, int64_t length)
{
	double avg_transfer_rate; // bytes per second
	char *data_source;

	if(w->total_transfer_time>1000000) {
		// Note w->total_transfer_time is timestamp_t with units of milliseconds.
		avg_transfer_rate = 1000000 * w->total_bytes_transferred / w->total_transfer_time;
		data_source = xxstrdup("worker's observed");
	} else {
		avg_transfer_rate = get_queue_transfer_rate(q, &data_source);
	}
	
	debug(D_WQ,"%s (%s) using %s average transfer rate of %.2lf MB/s\n", w->hostname, w->addrport, data_source, avg_transfer_rate/MEGABYTE);

	double tolerable_transfer_rate = avg_transfer_rate / q->transfer_outlier_factor; // bytes per second

	int timeout = length / tolerable_transfer_rate;

	if(w->foreman) {
		// A foreman must have a much larger minimum timeout, b/c it does not respond immediately to the master.
		timeout = MAX(q->foreman_transfer_timeout,timeout);
	} else {
		// An ordinary master has a lower minimum timeout b/c it responds immediately to the master.
		timeout = MAX(q->minimum_transfer_timeout,timeout);
	}

	debug(D_WQ, "%s (%s) will try up to %d seconds to transfer this %.2lf MB file.", w->hostname, w->addrport, timeout, length/1000000.0);

	free(data_source);
	return timeout;
}

static void update_catalog(struct work_queue *q, struct link *foreman_uplink, int force_update )
{
	static time_t last_update_time = 0;
	char address[LINK_ADDRESS_MAX];

	// Only advertise if we have a name.
	if(!q->name) return;

	// Only advertise every last_update_time seconds.
	if(!force_update && (time(0) - last_update_time) < WORK_QUEUE_UPDATE_INTERVAL) 
		return;

	// If host and port are not set, pick defaults.
	if(!q->catalog_host)	q->catalog_host = strdup(CATALOG_HOST);
	if(!q->catalog_port)	q->catalog_port = CATALOG_PORT;
	if(!q->update_port)	q->update_port = datagram_create(DATAGRAM_PORT_ANY);

	if(!domain_name_cache_lookup(q->catalog_host, address)) {
		debug(D_WQ,"could not resolve address of catalog server %s!",q->catalog_host);
		// don't try again until the next update period
		last_update_time = time(0);
		return;
	}

	// Generate the master status in an nvpair, and print it to a buffer.
	char buffer[DATAGRAM_PAYLOAD_MAX];
	struct nvpair *nv = queue_to_nvpair(q,foreman_uplink);
	nvpair_print(nv,buffer,sizeof(buffer));

	// Send the buffer.
	debug(D_WQ, "Advertising master status to the catalog server at %s:%d ...", q->catalog_host, q->catalog_port);
	datagram_send(q->update_port, buffer, strlen(buffer), address, q->catalog_port);

	// Clean up.
	nvpair_delete(nv);
	last_update_time = time(0);
}

static void cleanup_worker(struct work_queue *q, struct work_queue_worker *w)
{
	char *key, *value;
	struct work_queue_task *t;
	uint64_t taskid;

	if(!q || !w) return;
	
	hash_table_firstkey(w->current_files);
	while(hash_table_nextkey(w->current_files, &key, (void **) &value)) {
		hash_table_remove(w->current_files, key);
		free(value);
		hash_table_firstkey(w->current_files);
	}

	itable_firstkey(w->current_tasks);
	while(itable_nextkey(w->current_tasks, &taskid, (void **)&t)) {
		t->result = 0;	
		t->total_bytes_transferred = 0;
		t->total_transfer_time = 0;
		t->cmd_execution_time = 0;
		if (t->time_execute_cmd_start >= t->time_committed) {
			t->total_cmd_execution_time += timestamp_get() - t->time_execute_cmd_start;
		}
		if(t->output) {
			free(t->output);
		}
		t->output = 0;
		if(t->unlabeled) {
			t->cores = t->memory = t->disk = t->gpus = -1;
		}
		list_push_head(q->ready_list, t);

		reap_task_from_worker(q, w, t);

		itable_remove(q->finished_tasks, t->taskid);
	}
	itable_clear(w->current_tasks);
	w->finished_tasks = 0;
}

static void remove_worker(struct work_queue *q, struct work_queue_worker *w)
{
	if(!q || !w) return;

	debug(D_WQ, "worker %s (%s) removed", w->hostname, w->addrport);

	q->total_workers_removed++;

	cleanup_worker(q, w);

	hash_table_remove(q->worker_table, w->hashkey);
	hash_table_remove(q->workers_with_available_results, w->hashkey);
	
	log_worker_stats(q);
	
	if(w->link)
		link_close(w->link);

	itable_delete(w->current_tasks);
	hash_table_delete(w->current_files);
	work_queue_resources_delete(w->resources);
	free(w->hostname);
	free(w->os);
	free(w->arch);
	free(w->version);
	free(w);

	debug(D_WQ, "%d workers are connected in total now", hash_table_size(q->worker_table));
}

static int release_worker(struct work_queue *q, struct work_queue_worker *w)
{
	if(!w) return 0;
	send_worker_msg(q,w,"release\n");
	remove_worker(q, w);
	return 1;
}

static void add_worker(struct work_queue *q)
{
	struct link *link;
	struct work_queue_worker *w;
	char addr[LINK_ADDRESS_MAX];
	int port;

	link = link_accept(q->master_link, time(0) + q->short_timeout);
	if(!link) return;

	link_keepalive(link, 1);
	link_tune(link, LINK_TUNE_INTERACTIVE);

	if(!link_address_remote(link, addr, &port)) {
		link_close(link);
		return;
	}

	debug(D_WQ,"worker %s:%d connected",addr,port);

	if(q->password) {
		debug(D_WQ,"worker %s:%d authenticating",addr,port);
		if(!link_auth_password(link,q->password,time(0)+q->short_timeout)) {
			debug(D_WQ|D_NOTICE,"worker %s:%d presented the wrong password",addr,port);
			link_close(link);
			return;
		}
	}

	w = malloc(sizeof(*w));
	if(!w) {
		debug(D_NOTICE, "Cannot allocate memory for worker %s:%d.", addr, port);
		link_close(link);
		return;
	}

	memset(w, 0, sizeof(*w));
	w->hostname = strdup("unknown");
	w->os = strdup("unknown");
	w->arch = strdup("unknown");
	w->version = strdup("unknown");
	w->foreman = 0;
	w->link = link;
	w->current_files = hash_table_create(0, 0);
	w->current_tasks = itable_create(0);
	w->finished_tasks = 0;
	w->start_time = timestamp_get();
	w->resources = work_queue_resources_create();
	link_to_hash_key(link, w->hashkey);
	sprintf(w->addrport, "%s:%d", addr, port);
	hash_table_insert(q->worker_table, w->hashkey, w);
	log_worker_stats(q);
	q->total_workers_joined++;

	debug(D_WQ, "%d workers are connected in total now", hash_table_size(q->worker_table));
	
	return;
}

/*
Get a single file from a remote worker.
Returns 1 on success, 0 on failure to receive, -1 on failure to access.
*/
static int get_file( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, const char *local_name, int64_t length, int64_t * total_bytes)
{
	// If a bandwidth limit is in effect, choose the effective stoptime.
	timestamp_t effective_stoptime = 0;
	if(q->bandwidth) {
		effective_stoptime = (length/q->bandwidth)*1000000 + timestamp_get();
	}

	// Choose the actual stoptime.
	time_t stoptime = time(0) + get_transfer_wait_time(q, w, t, length);

	// If necessary, create parent directories of the file.
	if(strchr(local_name,'/')) {
		char dirname[WORK_QUEUE_LINE_MAX];
		path_dirname(local_name,dirname);
		if(!create_dir(dirname, 0700)) {
			debug(D_WQ, "Could not create directory - %s (%s)", dirname, strerror(errno));
			link_soak(w->link, length, stoptime);
			return APP_FAILURE;
		}
	}

	// Create the local file.
	debug(D_WQ, "Receiving file %s (size: %"PRId64" bytes) from %s (%s) ...", local_name, length, w->addrport, w->hostname);
	int fd = open(local_name, O_WRONLY | O_TRUNC | O_CREAT, 0700);
	if(fd < 0) {
		debug(D_NOTICE, "Cannot open file %s for writing: %s", local_name, strerror(errno));
		link_soak(w->link, length, stoptime);
		return APP_FAILURE;
	}

	// Write the data on the link to file.
	int64_t actual = link_stream_to_fd(w->link, fd, length, stoptime);

	close(fd);

	if(actual != length) {
		debug(D_WQ, "Received item size (%"PRId64") does not match the expected size - %"PRId64" bytes.", actual, length);
		unlink(local_name);
		return WORKER_FAILURE;
	}

	*total_bytes += length;

	// If the transfer was too fast, slow things down.
	timestamp_t current_time = timestamp_get();
	if(effective_stoptime && effective_stoptime > current_time) {
		usleep(effective_stoptime - current_time);
	}

	return SUCCESS;
}

/*
This function implements the recursive get protocol.
The master sents a single get message, then the worker
responds with a continuous stream of dir and file message
that indicate the entire contents of the directory.
This makes it efficient to move deep directory hierarchies with
high throughput and low latency.
Return 1 on success, 0 on failure to receive, -1 on failure to create.
*/
static int get_file_or_directory( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, const char *remote_name, const char *local_name, int64_t * total_bytes)
{
	// Remember the length of the specified remote path so it can be chopped from the result.
	int remote_name_len = strlen(remote_name);

	// Send the name of the file/dir name to fetch
	debug(D_WQ, "%s (%s) sending back %s to %s", w->hostname, w->addrport, remote_name, local_name);
	send_worker_msg(q,w, "get %s 1\n",remote_name);

	int result = SUCCESS; //return success unless something fails below

	// Process the recursive file/dir responses as they are sent.
	while(1) {
		char line[WORK_QUEUE_LINE_MAX];
		char tmp_remote_path[WORK_QUEUE_LINE_MAX];
		int64_t length;
		int errnum;

		if(recv_worker_msg_retry(q, w, line, sizeof(line)) < 0) {
			result = WORKER_FAILURE; 
			break;	
		}

		if(sscanf(line,"dir %s", tmp_remote_path)==1) {
			char *tmp_local_name = string_format("%s%s",local_name,&tmp_remote_path[remote_name_len]);
			result = create_dir(tmp_local_name,0700);
			if(!result) {
				debug(D_WQ, "Could not create directory - %s (%s)", tmp_local_name, strerror(errno));
				result = APP_FAILURE;
				free(tmp_local_name);
				break;
			}
			free(tmp_local_name);
		} else if(sscanf(line,"file %s %"SCNd64, tmp_remote_path, &length)==2) {
			char *tmp_local_name = string_format("%s%s",local_name,&tmp_remote_path[remote_name_len]);
			result = get_file(q,w,t,tmp_local_name,length,total_bytes);
			free(tmp_local_name);
			//Return if worker failure. Else wait for end message from worker.	
			if(result == WORKER_FAILURE) break;
		} else if(sscanf(line,"missing %s %d",tmp_remote_path,&errnum)==2) {
			// If the output file is missing, we make a note of that in the task result,
			// but we continue and consider the transfer a 'success' so that other
			// outputs are transferred and the task is given back to the caller.
			debug(D_WQ, "%s (%s): could not access requested file %s (%s)",w->hostname,w->addrport,remote_name,strerror(errnum));
			t->result |= WORK_QUEUE_RESULT_OUTPUT_MISSING;
		} else if(!strcmp(line,"end")) {
			// We have to return on receiving an end message.
			if (result == SUCCESS) {
				return result;
			} else {
				break;
			}
		} else {
			debug(D_WQ, "%s (%s): sent invalid response to get: %s",w->hostname,w->addrport,line);
			result = WORKER_FAILURE; //signal sys-level failure	
			break;
		}
	}

	// If we failed to *transfer* the output file, then that is a hard
	// failure which causes this function to return failure and the task
	// to be returned to the queue to be attempted elsewhere.
	debug(D_WQ, "%s (%s) failed to return output %s to %s", w->addrport, w->hostname, remote_name, local_name);
	if(result == APP_FAILURE) t->result |= WORK_QUEUE_RESULT_OUTPUT_MISSING;
	return result;
}

/*
For a given task and file, generate the name under which the file
should be stored in the remote cache directory.

The basic strategy is to construct a name that is unique to the
namespace from where the file is drawn, so that tasks sharing
the same input file can share the same copy.

In the common case of files, the cached name is based on the
hash of the local path, with the basename of the local path
included simply to assist with debugging.

In each of the other file types, a similar approach is taken,
including a hash and a name where one is known, or another
unique identifier where no name is available.
*/

char * make_cached_name( struct work_queue_task *t, struct work_queue_file *f )
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	md5_buffer(f->payload,strlen(f->payload),digest);

	switch(f->type) {
		case WORK_QUEUE_FILE:
		case WORK_QUEUE_DIRECTORY:
			return string_format("file-%s-%s",md5_string(digest),path_basename(f->payload));
			break;
		case WORK_QUEUE_FILE_PIECE:
			return string_format("piece-%s-%s-%lld-%lld",md5_string(digest),path_basename(f->payload),(long long)f->offset,(long long)f->piece_length);
			break;
		case WORK_QUEUE_REMOTECMD:
			return string_format("cmd-%s",md5_string(digest));
			break;
		case WORK_QUEUE_URL:
			return string_format("url-%s",md5_string(digest));
			break;
		case WORK_QUEUE_BUFFER:
		default:
			return string_format("buffer-%s",md5_string(digest));
			break;
	}
}

/*
This function stores an output file from the remote cache directory
to a third-party location, which can be either a remote filesystem
(WORK_QUEUE_FS_PATH) or a command to run (WORK_QUEUE_FS_CMD).
Returns 1 on success at worker and 0 on invalid message from worker.
*/
static int do_thirdput( struct work_queue *q, struct work_queue_worker *w,  const char *cached_name, const char *payload, int command )
{
	char line[WORK_QUEUE_LINE_MAX];
	int result; 

	send_worker_msg(q,w,"thirdput %d %s %s\n",command,cached_name,payload);

	if(recv_worker_msg_retry(q, w, line, WORK_QUEUE_LINE_MAX) < 0) return WORKER_FAILURE;
				
	if(sscanf(line, "thirdput-complete %d", &result)) {
		return result;
	} else {
		debug(D_WQ, "Error: invalid message received (%s)\n", line);
		return WORKER_FAILURE;
	}
}

/*
Get a single output file, located at the worker under 'cached_name'.
Returns 1 on success, 0 on failure to get, -1 on failure to access file.
*/
static int get_output_file( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, struct work_queue_file *f, const char *cached_name )
{
	int64_t total_bytes = 0;
	int result = SUCCESS; //return success unless something fails below.
			
	timestamp_t open_time = timestamp_get();

	if(f->flags & WORK_QUEUE_THIRDPUT) {
		if(!strcmp(cached_name, f->payload)) {
			debug(D_WQ, "output file %s already on shared filesystem", cached_name);
			f->flags |= WORK_QUEUE_PREEXIST;
		} else {
			result = do_thirdput(q,w,cached_name,f->payload,WORK_QUEUE_FS_PATH);
		}
	} else if(f->type == WORK_QUEUE_REMOTECMD) {
		result = do_thirdput(q,w,cached_name,f->payload,WORK_QUEUE_FS_CMD);
	} else {
		result = get_file_or_directory(q, w, t, cached_name, f->payload, &total_bytes);
	}

	timestamp_t close_time = timestamp_get();
	timestamp_t sum_time = close_time - open_time;

	if(total_bytes>0) {
		q->total_bytes_received += total_bytes;
		q->total_receive_time += sum_time;
		t->total_bytes_received += total_bytes;
		t->total_bytes_transferred += total_bytes;
		t->total_transfer_time += sum_time;
		w->total_bytes_transferred += total_bytes;
		w->total_transfer_time += sum_time;
		debug(D_WQ, "%s (%s) sent %.2lf MB in %.02lfs (%.02lfs MB/s) average %.02lfs MB/s", w->hostname, w->addrport, total_bytes / 1000000.0, sum_time / 1000000.0, (double) total_bytes / sum_time, (double) w->total_bytes_transferred / w->total_transfer_time);
	}

	// If the transfer was successful, make a record of it in the cache.
	if(result && f->flags & WORK_QUEUE_CACHE) {
		struct stat local_info;
		if (stat(f->payload,&local_info) == 0) {
			struct stat *remote_info = malloc(sizeof(*remote_info));
			if(!remote_info) {
				debug(D_NOTICE, "Cannot allocate memory for cache entry for output file %s at %s (%s)", f->payload, w->hostname, w->addrport);
				return APP_FAILURE;
			}
			memcpy(remote_info, &local_info, sizeof(local_info));
			hash_table_insert(w->current_files, cached_name, remote_info);
		} else {
			debug(D_NOTICE, "Cannot stat file %s: %s", f->payload, strerror(errno)); 
		}
	}

	return result;
}

static int get_output_files( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t )
{
	struct work_queue_file *f;
	int result = 1;

	if(t->output_files) {
		list_first_item(t->output_files);
		while((f = list_next_item(t->output_files))) {
			char * cached_name = make_cached_name(t,f);
			result = get_output_file(q,w,t,f,cached_name); 
			
			//if success or app-level failure, continue to get other files.
			//if worker failure, return.
			if(result == 0) {
				break;
			} 
					
			free(cached_name);
		}
	}

	// tell the worker you no longer need that task's output directory.
	send_worker_msg(q,w, "kill %d\n",t->taskid);

	return result;
}

// Sends "unlink file" for every file in the list except those that match one or more of the "except_flags"
static void delete_worker_files( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, struct list *files, int except_flags ) {
	struct work_queue_file *tf;
	
	if(!files) return;

	list_first_item(files);
	while((tf = list_next_item(files))) {
		if(!(tf->flags & except_flags)) {
			char *cached_name = make_cached_name(t,tf);
			send_worker_msg(q,w, "unlink %s\n",cached_name);
			hash_table_remove(w->current_files, cached_name);
			free(cached_name);
		}
	}
}

static void delete_task_output_files(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t) 
{
	delete_worker_files(q, w, t, t->output_files, 0);
}

static void delete_uncacheable_files( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t )
{
	delete_worker_files(q, w, t, t->input_files, WORK_QUEUE_CACHE | WORK_QUEUE_PREEXIST);
	delete_worker_files(q, w, t, t->output_files, WORK_QUEUE_CACHE | WORK_QUEUE_PREEXIST);
}

void work_queue_monitor_append_report(struct work_queue *q, struct work_queue_task *t)
{
	struct flock lock;
	FILE        *fsummary;
	char        *summary = string_format(RESOURCE_MONITOR_TASK_SUMMARY_NAME ".summary", getpid(), t->taskid);
	char        *msg; 

	lock.l_type   = F_WRLCK;
	lock.l_start  = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len    = 0;

	fcntl(q->monitor_fd, F_SETLKW, &lock);
	
	msg = string_format("# Work Queue pid: %d Task: %d summary:\n", getpid(), t->taskid);
	write(q->monitor_fd, msg, strlen(msg));
	free(msg);

	if( (fsummary = fopen(summary, "r")) == NULL )
	{
		msg = string_format("# Summary for task %d:%d was not available.\n", getpid(), t->taskid);
		write(q->monitor_fd, msg, strlen(msg));
		free(msg);
	}
	else
	{
		copy_stream_to_fd(fsummary, q->monitor_fd);
		fclose(fsummary);	
	}

	write(q->monitor_fd, "\n\n", 2);

	lock.l_type   = F_ULOCK;
	fcntl(q->monitor_fd, F_SETLK, &lock);

	if(unlink(summary) != 0)
		debug(D_NOTICE, "Summary %s could not be removed.\n", summary);
}

static void fetch_output_from_worker(struct work_queue *q, struct work_queue_worker *w, int taskid)
{
	struct work_queue_task *t;
	int result = 1;

	t = itable_lookup(w->current_tasks, taskid);
	if(!t) {
		debug(D_WQ, "Failed to find task %d at worker %s (%s).", taskid, w->hostname, w->addrport);
		handle_failure(q, w, t, 0); 
		return;	
	}

	// Receiving output ...
	t->time_receive_output_start = timestamp_get();
	result = get_output_files(q,w,t);
	if(result <= 0) {
		debug(D_WQ, "Failed to receive output from worker %s (%s).", w->hostname, w->addrport);
		handle_failure(q, w, t, result); 
		return;	
	}
	t->time_receive_output_finish = timestamp_get();

	delete_uncacheable_files(q,w,t);

	// At this point, a task is completed.
	reap_task_from_worker(q, w, t);

	itable_remove(q->finished_tasks, t->taskid);
	list_push_head(q->complete_list, t);

	w->finished_tasks--;
	t->time_task_finish = timestamp_get();

	/* if q is monitoring, append the task summary to the single
	 * queue summary, and delete the task summary. */
	if(q->monitor_mode)
		work_queue_monitor_append_report(q, t);

	// Record statistics information for capacity estimation
	add_task_report(q,t);
		
	// Update completed tasks and the total task execution time.
	q->total_tasks_complete++;
	w->total_tasks_complete++;

	w->total_task_time += t->cmd_execution_time;

	if(t->result == WORK_QUEUE_RESULT_SUCCESS)
	{
		q->total_good_execute_time  += t->cmd_execution_time;
		q->total_good_transfer_time += t->total_transfer_time;
	}

	debug(D_WQ, "%s (%s) done in %.02lfs total tasks %lld average %.02lfs",
		w->hostname,
		w->addrport,
		(t->time_receive_output_finish - t->time_send_input_start) / 1000000.0,
		(long long) w->total_tasks_complete,
		w->total_task_time / w->total_tasks_complete / 1000000.0);
	
	return;
}

/*
This function handles app-level failures. It remove the task from WQ and marks
the task as complete so it is returned to the application. 
*/
static void handle_app_failure(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t)
{
	reap_task_from_worker(q, w, t);

	//remove the task from tables that track dispatched tasks. 
	itable_remove(q->finished_tasks, t->taskid); 

	
	//add the task to complete list so it is given back to the application.
	list_push_head(q->complete_list, t);

	/*If the failure happened after a task execution, we remove all the output
	files specified for that task from the worker's cache.  This is because the
	application may resubmit the task and the resubmitted task may produce
	different outputs. */
	if(t) {
		if(t->time_execute_cmd_start > 0) {
			delete_task_output_files(q,w,t);
		}	
	}

	return;
}

static void handle_worker_failure(struct work_queue *q, struct work_queue_worker *w) 
{
	//WQ failures happen in the master-worker interactions. In this case, we
	//remove the worker and retry the tasks dispatched to it elsewhere.
	remove_worker(q, w);
	return;
}

static void handle_failure(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, int fail_type)
{
	if(fail_type < 0) {
		handle_app_failure(q, w, t);
	} else {
		handle_worker_failure(q, w);
	}
	return;
}

static int process_workqueue(struct work_queue *q, struct work_queue_worker *w, const char *line)
{
	char items[4][WORK_QUEUE_LINE_MAX];
	int worker_protocol;

	int n = sscanf(line,"workqueue %d %s %s %s %s",&worker_protocol,items[0],items[1],items[2],items[3]);
	if(n!=5) return -1;

	if(worker_protocol!=WORK_QUEUE_PROTOCOL_VERSION) {
		debug(D_WQ|D_NOTICE,"worker (%s) is using work queue protocol %d, but I am using protocol %d",w->addrport,worker_protocol,WORK_QUEUE_PROTOCOL_VERSION);
		return -1;
	}

	if(w->hostname) free(w->hostname);
	if(w->os)       free(w->os);
	if(w->arch)     free(w->arch);
	if(w->version)  free(w->version);

	w->hostname = strdup(items[0]);
	w->os       = strdup(items[1]);
	w->arch     = strdup(items[2]);
	w->version  = strdup(items[3]);

	if(!strcmp(w->os, "foreman"))
	{
		w->foreman = 1;
	}

	log_worker_stats(q);
	debug(D_WQ, "%s (%s) running CCTools version %s on %s (operating system) with architecture %s is ready", w->hostname, w->addrport, w->version, w->os, w->arch);
	
	if(strcmp(CCTOOLS_VERSION, w->version)) {
		debug(D_DEBUG, "Warning: potential worker version mismatch: worker %s (%s) is version %s, and master is version %s", w->hostname, w->addrport, w->version, CCTOOLS_VERSION);
	}
	
	return 0;
}

/* 
Returns 1 on success,  0 on failure to receive.
Failure to store result is treated as success so we continue to retrieve the
output files of the task. 
*/
static int process_result(struct work_queue *q, struct work_queue_worker *w, const char *line) {

	if(!q || !w || !line) return WORKER_FAILURE; 

	int task_result;
	uint64_t taskid;
	int64_t output_length, retrieved_output_length;
	timestamp_t execution_time;
	struct work_queue_task *t;
	int64_t actual;
	timestamp_t observed_execution_time;
	timestamp_t effective_stoptime = 0;
	time_t stoptime;

	//Format: result, output length, execution time, taskid
	char items[3][WORK_QUEUE_PROTOCOL_FIELD_MAX];
	int n = sscanf(line, "result %s %s %s %" SCNd64, items[0], items[1], items[2], &taskid);

	if(n < 4) {
		debug(D_WQ, "Invalid message from worker %s (%s): %s", w->hostname, w->addrport, line);
		return WORKER_FAILURE;
	}
	
	task_result = atoi(items[0]);
	output_length = atoll(items[1]);
	
	t = itable_lookup(w->current_tasks, taskid);
	if(!t) {
		debug(D_WQ, "Unknown task result from worker %s (%s): no task %" PRId64" assigned to worker.  Ignoring result.", w->hostname, w->addrport, taskid);
		stoptime = time(0) + get_transfer_wait_time(q, w, 0, output_length);
		link_soak(w->link, output_length, stoptime);
		return SUCCESS; 
	}
	
	t->time_receive_result_start = timestamp_get();
	observed_execution_time = timestamp_get() - t->time_execute_cmd_start;
	
	if(n >= 3) {
		execution_time = atoll(items[2]);
		t->cmd_execution_time = observed_execution_time > execution_time ? execution_time : observed_execution_time;
	} else {
		t->cmd_execution_time = observed_execution_time;
	}
	t->total_cmd_execution_time += t->cmd_execution_time;

	if(q->bandwidth) {
		effective_stoptime = (output_length/q->bandwidth)*1000000 + timestamp_get();
	}

	if(output_length <= MAX_TASK_STDOUT_STORAGE) {
		retrieved_output_length = output_length; 
	} else {
		retrieved_output_length = MAX_TASK_STDOUT_STORAGE; 
		fprintf(stderr, "warning: stdout of task %"PRId64" requires %2.2lf GB of storage. This exceeds maximum supported size of %d GB. Only %d GB will be retreived.\n", taskid, ((double) output_length)/MAX_TASK_STDOUT_STORAGE, MAX_TASK_STDOUT_STORAGE/GIGABYTE, MAX_TASK_STDOUT_STORAGE/GIGABYTE);
		t->result |= WORK_QUEUE_RESULT_STDOUT_MISSING;
	}

	t->output = malloc(retrieved_output_length+1);
	if(t->output == NULL) {
		fprintf(stderr, "error: allocating memory of size %"PRId64" bytes failed for storing stdout of task %"PRId64".\n", retrieved_output_length, taskid);
		//drop the entire length of stdout on the link	
		stoptime = time(0) + get_transfer_wait_time(q, w, t, output_length);
		link_soak(w->link, output_length, stoptime);
		retrieved_output_length = 0; 
		t->result |= WORK_QUEUE_RESULT_STDOUT_MISSING;
	} 

	if(retrieved_output_length > 0) {
		debug(D_WQ, "Receiving stdout of task %"PRId64" (size: %"PRId64" bytes) from %s (%s) ...", taskid, retrieved_output_length, w->addrport, w->hostname);
		
		//First read the bytes we keep.
		stoptime = time(0) + get_transfer_wait_time(q, w, t, retrieved_output_length);
		actual = link_read(w->link, t->output, retrieved_output_length, stoptime);
		if(actual != retrieved_output_length) {
			debug(D_WQ, "Failure: actual received stdout size (%"PRId64" bytes) is different from expected (%"PRId64" bytes).", actual, retrieved_output_length);
			t->output[actual] = '\0';
			return WORKER_FAILURE;
		}
		debug(D_WQ, "Retrieved %"PRId64" bytes from %s (%s)", actual, w->hostname, w->addrport);
		
		//Then read the bytes we need to throw away.
		if(output_length > retrieved_output_length) {
			debug(D_WQ, "Dropping the remaining %"PRId64" bytes of the stdout of task %"PRId64" since stdout length is limited to %d bytes.\n", (output_length-MAX_TASK_STDOUT_STORAGE), taskid, MAX_TASK_STDOUT_STORAGE);
			stoptime = time(0) + get_transfer_wait_time(q, w, t, (output_length-retrieved_output_length));
			link_soak(w->link, (output_length-retrieved_output_length), stoptime);
		
			//overwrite the last few bytes of buffer to signal truncated stdout.
			char *truncate_msg = string_format("\n>>>>>> WORK QUEUE HAS TRUNCATED THE STDOUT AFTER THIS POINT.\n>>>>>> MAXIMUM OF %d BYTES REACHED, %" PRId64 " BYTES TRUNCATED.", MAX_TASK_STDOUT_STORAGE, output_length - retrieved_output_length);	
			strncpy(t->output+MAX_TASK_STDOUT_STORAGE-strlen(truncate_msg), truncate_msg, strlen(truncate_msg));
			free(truncate_msg);
		}
		
		timestamp_t current_time = timestamp_get();
		if(effective_stoptime && effective_stoptime > current_time) {
			usleep(effective_stoptime - current_time);
		}
	} else {
		actual = 0;
	}
	
	if(t->output)	
		t->output[actual] = 0;
	
	t->time_receive_result_finish = timestamp_get();

	t->return_status = task_result;

	t->time_execute_cmd_finish = t->time_execute_cmd_start + t->cmd_execution_time;
	q->total_execute_time += t->cmd_execution_time;

	itable_remove(q->running_tasks, taskid);
	itable_insert(q->finished_tasks, taskid, (void*)t);

	w->finished_tasks++;

	return SUCCESS;
}

static void process_available_results(struct work_queue *q, struct work_queue_worker *w, int max_count)
{
	//max_count == -1, tells the worker to send all available results.

	send_worker_msg(q, w, "send_results %d\n", max_count);
	debug(D_WQ, "Reading result(s) from %s (%s)", w->hostname, w->addrport);

	char line[WORK_QUEUE_LINE_MAX];
	int i = 0;
	
	int result = SUCCESS; //return success unless something fails below.

	while(1) {
		if(recv_worker_msg_retry(q, w, line, sizeof(line)) < 0) {
			result = WORKER_FAILURE; 
			break; 
		}
		
		if(string_prefix_is(line,"result")) {
			result = process_result(q, w, line);
			if(result != SUCCESS) break; 
			i++;
		} else if(!strcmp(line,"end")) {
			//Only return success if last message is end.
			break;
		} else {
			debug(D_WQ, "%s (%s): sent invalid response to send_results: %s",w->hostname,w->addrport,line);
			result = WORKER_FAILURE;
			break;
		}
	}
	
	if(max_count > 0 && i > max_count) {
		debug(D_WQ, "%s (%s): sent %d results. At most %d were expected.",w->hostname,w->addrport, i, max_count);
		result = WORKER_FAILURE; //if the worker disobeyed, consider it as failed.
	} 

	if(result != SUCCESS) {
		handle_failure(q, w, NULL, result);
	}

	return;
}

/*
queue_to_nvpair examines the overall queue status and creates
an nvair which can be sent to the catalog or directly to the
user that connects via work_queue_status.
*/

static struct nvpair * queue_to_nvpair( struct work_queue *q, struct link *foreman_uplink )
{
	struct nvpair *nv = nvpair_create();
	if(!nv) return 0;

	// Insert all properties from work_queue_stats

	struct work_queue_stats info;
	work_queue_get_stats(q,&info);

	nvpair_insert_integer(nv,"port",info.port);
	nvpair_insert_integer(nv,"priority",info.priority);

	//send info on workers
	nvpair_insert_integer(nv,"workers",info.total_workers_connected);
	nvpair_insert_integer(nv,"workers_init",info.workers_init);
	nvpair_insert_integer(nv,"workers_idle",info.workers_idle);
	nvpair_insert_integer(nv,"workers_busy",info.workers_busy);
	nvpair_insert_integer(nv,"workers_ready",info.workers_ready); //workers_ready is deprecated
	nvpair_insert_integer(nv,"total_workers_connected",info.total_workers_connected);
	nvpair_insert_integer(nv,"total_workers_joined",info.total_workers_joined);
	nvpair_insert_integer(nv,"total_workers_removed",info.total_workers_removed);
	
	//send info on tasks
	nvpair_insert_integer(nv,"tasks_waiting",info.tasks_waiting);
	nvpair_insert_integer(nv,"tasks_running",info.tasks_running);
	// KNOWN HACK: The following line is inconsistent but kept for compatibility reasons.
	// Everyone wants to know total_tasks_complete, but few are interested in tasks_complete.
	nvpair_insert_integer(nv,"tasks_complete",info.total_tasks_complete); 
	nvpair_insert_integer(nv,"total_tasks_complete",info.total_tasks_complete);
	nvpair_insert_integer(nv,"total_tasks_dispatched",info.total_tasks_dispatched);
	nvpair_insert_integer(nv,"total_tasks_cancelled",info.total_tasks_cancelled);

	//send info on queue
	nvpair_insert_integer(nv,"start_time",info.start_time);
	nvpair_insert_integer(nv,"total_send_time",info.total_send_time);
	nvpair_insert_integer(nv,"total_receive_time",info.total_receive_time);
	nvpair_insert_integer(nv,"total_bytes_sent",info.total_bytes_sent);
	nvpair_insert_integer(nv,"total_bytes_received",info.total_bytes_received);
	nvpair_insert_float(nv,"efficiency",info.efficiency);
	nvpair_insert_float(nv,"idle_percentage",info.idle_percentage);
	nvpair_insert_integer(nv,"capacity",info.capacity);
	nvpair_insert_integer(nv,"total_execute_time",info.total_execute_time);
	nvpair_insert_integer(nv,"total_good_execute_time",info.total_good_execute_time);

	// Add the resources computed from tributary workers.
	struct work_queue_resources r;
	aggregate_workers_resources(q,&r);
	work_queue_resources_add_to_nvpair(&r,nv);

	char owner[USERNAME_MAX];
	username_get(owner);

	// Add special properties expected by the catalog server
	nvpair_insert_string(nv,"type","wq_master");
	if(q->name) nvpair_insert_string(nv,"project",q->name);
	nvpair_insert_integer(nv,"starttime",(q->start_time/1000000)); // catalog expects time_t not timestamp_t
	nvpair_insert_string(nv,"working_dir",q->workingdir);
	nvpair_insert_string(nv,"owner",owner);
	nvpair_insert_string(nv,"version",CCTOOLS_VERSION);

	// If this is a foreman, add the master address and the disk resources
	if(foreman_uplink) {
		int port;
		char address[LINK_ADDRESS_MAX];
		char addrport[WORK_QUEUE_LINE_MAX];

		link_address_remote(foreman_uplink,address,&port);
		sprintf(addrport,"%s:%d",address,port);
		nvpair_insert_string(nv,"my_master",addrport);

		// get foreman local resources and overwrite disk usage
		struct work_queue_resources local_resources;
		work_queue_resources_measure_locally(&local_resources,q->workingdir);
		r.disk.total = local_resources.disk.total;
		r.disk.inuse = local_resources.disk.inuse;
		work_queue_resources_add_to_nvpair(&r,nv);
	}

	return nv;
}


void current_tasks_to_nvpair( struct nvpair *nv, struct work_queue_worker *w )
{
	struct work_queue_task *t;
	uint64_t taskid;
	int n = 0;

	itable_firstkey(w->current_tasks);
	while(itable_nextkey(w->current_tasks, &taskid, (void**)&t)) {
		char task_string[WORK_QUEUE_LINE_MAX];

		sprintf(task_string, "current_task_%03d_id", n);
		nvpair_insert_integer(nv,task_string,t->taskid);

		sprintf(task_string, "current_task_%03d_command", n);
		nvpair_insert_string(nv,task_string,t->command_line);
		n++;
	}
}

struct nvpair * worker_to_nvpair( struct work_queue *q, struct work_queue_worker *w )
{
	struct nvpair *nv = nvpair_create();
	if(!nv) return 0;

	nvpair_insert_string(nv,"hostname",w->hostname);
	nvpair_insert_string(nv,"os",w->os);
	nvpair_insert_string(nv,"arch",w->arch);
	nvpair_insert_string(nv,"address_port",w->addrport);
	nvpair_insert_integer(nv,"ncpus",w->resources->cores.total);
	nvpair_insert_integer(nv,"total_tasks_complete",w->total_tasks_complete);
	nvpair_insert_integer(nv,"total_tasks_running",itable_size(w->current_tasks));
	nvpair_insert_integer(nv,"total_bytes_transferred",w->total_bytes_transferred);
	nvpair_insert_integer(nv,"total_transfer_time",w->total_transfer_time);
	nvpair_insert_integer(nv,"start_time",w->start_time);
	nvpair_insert_integer(nv,"current_time",timestamp_get()); 


	work_queue_resources_add_to_nvpair(w->resources,nv);

	current_tasks_to_nvpair(nv, w);

	return nv;
}

struct nvpair * task_to_nvpair( struct work_queue_task *t, const char *state, const char *host )
{
	struct nvpair *nv = nvpair_create();
	if(!nv) return 0;

	nvpair_insert_integer(nv,"taskid",t->taskid);
	nvpair_insert_string(nv,"state",state);
	if(t->tag) nvpair_insert_string(nv,"tag",t->tag);
	nvpair_insert_string(nv,"command",t->command_line);
	if(host) nvpair_insert_string(nv,"host",host);

	return nv;
}

static int process_queue_status( struct work_queue *q, struct work_queue_worker *target, const char *line, time_t stoptime )
{
	char request[WORK_QUEUE_LINE_MAX];
	struct link *l = target->link;
	
	free(target->hostname);
	target->hostname = strdup("QUEUE_STATUS");

	if(!sscanf(line, "%[^_]_status", request) == 1) {
		return -1;
	}
	
	if(!strcmp(request, "queue")) {
		struct nvpair *nv = queue_to_nvpair( q, 0 );
		if(nv) {
			link_nvpair_write(l,nv,stoptime);
			nvpair_delete(nv);
		}
	} else if(!strcmp(request, "task")) {
		struct work_queue_task *t;
		struct work_queue_worker *w;
		struct nvpair *nv;
		uint64_t key;

		itable_firstkey(q->running_tasks);
		while(itable_nextkey(q->running_tasks,&key,(void**)&t)) {
			w = itable_lookup(q->worker_task_map, t->taskid);
			if(w) {
				nv = task_to_nvpair(t,"running",w->hostname);
				if(nv) {
					// Include detailed information on where the task is running:
					// address and port, workspace
					nvpair_insert_string(nv, "address_port", w->addrport);

					// Timestamps on running task related events 
					nvpair_insert_integer(nv, "submit_to_queue_time", t->time_task_submit); 
					nvpair_insert_integer(nv, "send_input_start_time", t->time_send_input_start);
					nvpair_insert_integer(nv, "execute_cmd_start_time", t->time_execute_cmd_start);
					nvpair_insert_integer(nv, "current_time", timestamp_get()); 

					link_nvpair_write(l,nv,stoptime);
					nvpair_delete(nv);
				}
			}
		}

		list_first_item(q->ready_list);
		while((t = list_next_item(q->ready_list))) {
			nv = task_to_nvpair(t,"waiting",0);
			if(nv) {
				link_nvpair_write(l,nv,stoptime);
				nvpair_delete(nv);
			}
		}

		list_first_item(q->complete_list);
		while((t = list_next_item(q->complete_list))) {
			nv = task_to_nvpair(t,"complete",0);
			if(nv) {
				link_nvpair_write(l,nv,stoptime);
				nvpair_delete(nv);
			}
		}
	} else if(!strcmp(request, "worker")) {
		struct work_queue_worker *w;
		struct nvpair *nv;
		char *key;

		hash_table_firstkey(q->worker_table);
		while(hash_table_nextkey(q->worker_table,&key,(void**)&w)) {
			// If the worker has not been initializd, ignore it.
			if(!strcmp(w->hostname, "unknown")) continue;
			nv = worker_to_nvpair(q, w);
			if(nv) {
				link_nvpair_write(l,nv,stoptime);
				nvpair_delete(nv);
			}
		}
	}

	link_write(l, "\n", 1, stoptime);
	return 0;
}

static int process_resource( struct work_queue *q, struct work_queue_worker *w, const char *line )
{
	char category[WORK_QUEUE_LINE_MAX];
	struct work_queue_resource r;

	int n = sscanf(line, "resource %s %"PRId64 " %"PRId64" %"PRId64" %"PRId64" %"PRId64, category, &r.inuse,&r.committed,&r.total,&r.smallest,&r.largest);
		
	if(n == 2 && !strcmp(category,"tag"))
	{
		/* Shortcut, inuse has the tag, as "resources tag" only sends one value */
		w->resources->tag = r.inuse;
		log_worker_stats(q);

	} else if(n == 6) {
		if(!strcmp(category,"cores")) {
			w->resources->cores = r;
		} else if(!strcmp(category,"memory")) {
			w->resources->memory = r;
		} else if(!strcmp(category,"disk")) {
			w->resources->disk = r;
		} else if(!strcmp(category,"gpus")) {
			w->resources->gpus = r;
		} else if(!strcmp(category,"workers")) {
			w->resources->workers = r;
		}
	}

	return 0;
}

static void handle_worker(struct work_queue *q, struct link *l)
{
	char line[WORK_QUEUE_LINE_MAX];
	char key[WORK_QUEUE_LINE_MAX];
	struct work_queue_worker *w;

	link_to_hash_key(l, key);
	w = hash_table_lookup(q->worker_table, key);

	int worker_failure = 0;
	int result = recv_worker_msg(q, w, line, sizeof(line));

	//if result > 0, it means a message is left to consume
	if(result > 0) {
		debug(D_WQ, "Invalid message from worker %s (%s): %s", w->hostname, w->addrport, line);
		worker_failure = 1;
	} else if(result < 0){
		if(!strcmp(w->hostname, "QUEUE_STATUS")) {
			debug(D_WQ, "Work Queue Status worker disconnected (%s)", w->addrport);
		} else {
			debug(D_WQ, "Failed to read from worker %s (%s)", w->hostname, w->addrport);
		}
		worker_failure = 1;
	} // otherwise do nothing..message was consumed and processed in recv_worker_msg()

	if(worker_failure) {
		handle_worker_failure(q, w);
	}
}

static int build_poll_table(struct work_queue *q, struct link *master)
{
	int n = 0;
	char *key;
	struct work_queue_worker *w;

	// Allocate a small table, if it hasn't been done yet.
	if(!q->poll_table) {
		q->poll_table = malloc(sizeof(*q->poll_table) * q->poll_table_size);
		if(!q->poll_table) {
			//if we can't allocate a poll table, we can't do anything else.
			fatal("allocating memory for poll table failed.");
			return 0;
		}
	}
	// The first item in the poll table is the master link, which accepts new connections.
	q->poll_table[0].link = q->master_link;
	q->poll_table[0].events = LINK_READ;
	q->poll_table[0].revents = 0;
	n = 1;

	if(master) {
		q->poll_table[n].link = master;
		q->poll_table[n].events = LINK_READ;
		q->poll_table[n].revents = 0;
		n++;
	}

	// For every worker in the hash table, add an item to the poll table
	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {

		// If poll table is not large enough, reallocate it
		if(n >= q->poll_table_size) {
			q->poll_table_size *= 2;
			q->poll_table = realloc(q->poll_table, sizeof(*q->poll_table) * q->poll_table_size);
			if(q->poll_table == NULL) {
				//if we can't allocate a poll table, we can't do anything else.
				fatal("reallocating memory for poll table failed.");
				return 0;
			}
		}

		q->poll_table[n].link = w->link;
		q->poll_table[n].events = LINK_READ;
		q->poll_table[n].revents = 0;
		n++;
	}

	return n;
}

//Send a file. Returns 1 on success, 0 on failure to send, -1 on failure to access.
static int send_file( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, const char *localname, const char *remotename, off_t offset, int64_t length, int64_t *total_bytes, int flags)
{
	struct stat local_info;
	time_t stoptime;
	timestamp_t effective_stoptime = 0;
	int64_t actual = 0;
	
	if(stat(localname, &local_info) < 0) {
		debug(D_NOTICE, "Cannot stat file %s: %s", localname, strerror(errno)); 
		return APP_FAILURE;
	}

	/* normalize the mode so as not to set up invalid permissions */
	local_info.st_mode |= 0600;
	local_info.st_mode &= 0777;
	
	if(!length) {
		length = local_info.st_size;
	}
	
	debug(D_WQ, "%s (%s) needs file %s bytes %lld:%lld as '%s'", w->hostname, w->addrport, localname, (long long) offset, (long long) offset+length, remotename);
	int fd = open(localname, O_RDONLY, 0);
	if(fd < 0) {
		debug(D_NOTICE, "Cannot open file %s: %s", localname, strerror(errno)); 
		return APP_FAILURE;
	}

	//We want to send bytes starting from 'offset'. So seek to it first.
	if (offset >= 0 && (offset+length) <= local_info.st_size) {
		if(lseek(fd, offset, SEEK_SET) == -1) {
			debug(D_NOTICE, "Cannot seek file %s to offset %lld: %s", localname, (long long) offset, strerror(errno));
			close(fd);
			return APP_FAILURE;
		}
	} else {
		debug(D_NOTICE, "File specification %s (%lld:%lld) is invalid", localname, (long long) offset, (long long) offset+length);
		close(fd);
		return APP_FAILURE;
	}
	
	if(q->bandwidth) {
		effective_stoptime = (length/q->bandwidth)*1000000 + timestamp_get();
	}
	
	stoptime = time(0) + get_transfer_wait_time(q, w, t, length);
	send_worker_msg(q,w, "put %s %"PRId64" 0%o %d\n",remotename, length, local_info.st_mode, flags);
	actual = link_stream_from_fd(w->link, fd, length, stoptime);
	close(fd);
	
	*total_bytes += actual;
	
	if(actual != length) 
		return WORKER_FAILURE;
		
	timestamp_t current_time = timestamp_get();
	if(effective_stoptime && effective_stoptime > current_time) {
		usleep(effective_stoptime - current_time);
	}
	
	return SUCCESS;
}

/*
Send a directory and all of its contentss.
Returns 1 on success, 0 on failure to send, -1 on failure to access directory.
*/
static int send_directory( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, const char *dirname, const char *remotedirname, int64_t * total_bytes, int flags )
{
	DIR *dir = opendir(dirname);
	if(!dir) {
		debug(D_NOTICE, "Cannot open dir %s: %s", dirname, strerror(errno)); 
		return APP_FAILURE;
	}

	int result = SUCCESS;

	// When putting a file its parent directories are automatically
	// created by the worker, so no need to manually create them.
	struct dirent *d;
	while((d = readdir(dir))) {
		if(!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;

		char *localpath = string_format("%s/%s",dirname,d->d_name);
		char *remotepath = string_format("%s/%s",remotedirname,d->d_name);
	
		struct stat local_info;
		if(stat(localpath, &local_info)>=0) {
			if(S_ISDIR(local_info.st_mode))  {
				result = send_directory( q, w, t, localpath, remotepath, total_bytes, flags );
			} else {
				result = send_file( q, w, t, localpath, remotepath, 0, 0, total_bytes, flags );
			}	
		} else {
			debug(D_NOTICE, "Cannot stat file %s: %s", localpath, strerror(errno)); 
			result = APP_FAILURE;
		}

		free(localpath);
		free(remotepath);

		if(result != SUCCESS) break;
	}
	
	closedir(dir);
	return result;
}

/*
Send a file or directory to a remote worker, if it is not already cached.
The local file name should already have been expanded by the caller.
Returns 1 on success, 0 on failure to send, -1 on failure to access file/directory.
*/
static int send_file_or_directory( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, struct work_queue_file *tf, const char *expanded_local_name, int64_t * total_bytes)
{
	struct stat local_info;
	struct stat *remote_info;

	if(stat(expanded_local_name, &local_info) < 0) {
		debug(D_NOTICE, "Cannot stat file %s: %s", expanded_local_name, strerror(errno)); 
		return APP_FAILURE;
	}	
	
	int result = SUCCESS;

	// Look in the current files hash to see if the file is already cached.
	char *cached_name = make_cached_name(t,tf);
	remote_info = hash_table_lookup(w->current_files, cached_name);

	// If not cached, or the metadata has changed, then send the item.
	if(!remote_info || remote_info->st_mtime != local_info.st_mtime || remote_info->st_size != local_info.st_size) {

		if(remote_info) {
			hash_table_remove(w->current_files, cached_name);
			free(remote_info);
		}

		if(S_ISDIR(local_info.st_mode)) {
			result = send_directory(q, w, t, expanded_local_name, cached_name, total_bytes, tf->flags);
		} else {
			result = send_file(q, w, t, expanded_local_name, cached_name, tf->offset, tf->piece_length, total_bytes, tf->flags);
		}

		if(result && tf->flags & WORK_QUEUE_CACHE) {
			remote_info = malloc(sizeof(*remote_info));
			if(remote_info) {
				memcpy(remote_info, &local_info, sizeof(local_info));
				hash_table_insert(w->current_files, cached_name, remote_info);
			} else {
				debug(D_NOTICE, "Cannot allocate memory for cache entry for input file %s at %s (%s)", expanded_local_name, w->hostname, w->addrport);
			}
		}
	}

	free(cached_name);
	return result;
}

/** 
 *	This function expands Work Queue environment variables such as
 * 	$OS, $ARCH, that are specified in the definition of Work Queue 
 * 	input files. It expands these variables based on the info reported 
 *	by each connected worker.
 *	Will always return a non-empty string. That is if no match is found
 *	for any of the environment variables, it will return the input string
 *	as is.
 * 	*/
static char *expand_envnames(struct work_queue_worker *w, const char *payload)
{
	char *expanded_name;
	char *str, *curr_pos;
	char *delimtr = "$";
	char *token;

	// Shortcut: If no dollars anywhere, duplicate the whole string.
	if(!strchr(payload,'$')) return strdup(payload);

	str = xxstrdup(payload);

	expanded_name = (char *) malloc(strlen(payload) + (50 * sizeof(char)));
	if(expanded_name == NULL) {
		debug(D_NOTICE, "Cannot allocate memory for filename %s.\n", payload);
		return NULL;
	} else {
		//Initialize to null byte so it works correctly with strcat.
		*expanded_name = '\0';
	}

	token = strtok(str, delimtr);
	while(token) {
		if((curr_pos = strstr(token, "ARCH"))) {
			if((curr_pos - token) == 0) {
				strcat(expanded_name, w->arch);
				strcat(expanded_name, token + 4);
			} else {
				//No match. So put back '$' and rest of the string.
				strcat(expanded_name, "$");
				strcat(expanded_name, token);
			}
		} else if((curr_pos = strstr(token, "OS"))) {
			if((curr_pos - token) == 0) {
				//Cygwin oddly reports OS name in all caps and includes version info.
				if(strstr(w->os, "CYGWIN")) {
					strcat(expanded_name, "Cygwin");
				} else {
					strcat(expanded_name, w->os);
				}
				strcat(expanded_name, token + 2);
			} else {
				strcat(expanded_name, "$");
				strcat(expanded_name, token);
			}
		} else {
			//If token and str don't point to same location, then $ sign was before token and needs to be put back.
			if((token - str) > 0) {
				strcat(expanded_name, "$");
			}
			strcat(expanded_name, token);
		}
		token = strtok(NULL, delimtr);
	}

	free(str);

	debug(D_WQ, "File name %s expanded to %s for %s (%s).", payload, expanded_name, w->hostname, w->addrport);

	return expanded_name;
}

static int send_input_file(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t, struct work_queue_file *f)
{
	int64_t total_bytes = 0;
	int64_t actual = 0;
	int result = SUCCESS; //return success unless something fails below

	char *cached_name = make_cached_name(t,f);

	timestamp_t open_time = timestamp_get();

	switch (f->type) {

	case WORK_QUEUE_BUFFER:
		debug(D_WQ, "%s (%s) needs literal as %s", w->hostname, w->addrport, f->remote_name);
		time_t stoptime = time(0) + get_transfer_wait_time(q, w, t, f->length);
		send_worker_msg(q,w, "put %s %d %o %d\n",cached_name, f->length, 0777, f->flags);
		actual = link_putlstring(w->link, f->payload, f->length, stoptime);
		if(actual!=f->length) {
			result = WORKER_FAILURE;	
		}	
		total_bytes = actual;
		break;

	case WORK_QUEUE_REMOTECMD:
		debug(D_WQ, "%s (%s) needs %s from remote filesystem using %s", w->hostname, w->addrport, f->remote_name, f->payload);
		send_worker_msg(q,w, "thirdget %d %s %s\n",WORK_QUEUE_FS_CMD, cached_name, f->payload);
		break;

	case WORK_QUEUE_URL:
		debug(D_WQ, "%s (%s) needs %s from the url, %s %d", w->hostname, w->addrport, cached_name, f->payload, f->length);
		send_worker_msg(q,w, "url %s %d 0%o %d\n",cached_name, f->length, 0777, f->flags);
		link_putlstring(w->link, f->payload, f->length, time(0) + q->short_timeout);
		break;

	case WORK_QUEUE_DIRECTORY:
		// Do nothing.  Empty directories are handled by the task specification, while recursive directories are implemented as WORK_QUEUE_FILEs
		break;

	case WORK_QUEUE_FILE:
	case WORK_QUEUE_FILE_PIECE:
		if(f->flags & WORK_QUEUE_THIRDGET) {
			debug(D_WQ, "%s (%s) needs %s from shared filesystem as %s", w->hostname, w->addrport, f->payload, f->remote_name);

			if(!strcmp(f->remote_name, f->payload)) {
				f->flags |= WORK_QUEUE_PREEXIST;
			} else {
				if(f->flags & WORK_QUEUE_SYMLINK) {
					send_worker_msg(q,w, "thirdget %d %s %s\n", WORK_QUEUE_FS_SYMLINK, cached_name, f->payload);
				} else {
					send_worker_msg(q,w, "thirdget %d %s %s\n", WORK_QUEUE_FS_PATH, cached_name, f->payload);
				}
			}
		} else {
			char *expanded_payload = expand_envnames(w, f->payload);
			if(expanded_payload) { 
				result = send_file_or_directory(q,w,t,f,expanded_payload,&total_bytes);
				free(expanded_payload);
			} else {
				result = APP_FAILURE; //signal app-level failure.
			}
		}
		break;
	}

	if(result == SUCCESS) {
		timestamp_t close_time = timestamp_get();
		timestamp_t elapsed_time = close_time-open_time;

		t->total_bytes_sent += total_bytes;
		t->total_bytes_transferred += total_bytes;
		t->total_transfer_time += elapsed_time;

		w->total_bytes_transferred += total_bytes;
		w->total_transfer_time += elapsed_time;

		q->total_bytes_sent += total_bytes;
		q->total_send_time += elapsed_time;

		// Avoid division by zero below.
		if(elapsed_time==0) elapsed_time = 1;

		if(total_bytes > 0) {
			debug(D_WQ, "%s (%s) received %.2lf MB in %.02lfs (%.02lfs MB/s) average %.02lfs MB/s",
				w->hostname,
				w->addrport,
				total_bytes / 1000000.0,
				elapsed_time / 1000000.0,
				(double) total_bytes / elapsed_time,
				(double) w->total_bytes_transferred / w->total_transfer_time
			);
		}
	} else {
		debug(D_WQ, "%s (%s) failed to send %s (%" PRId64 " bytes sent).",
			w->hostname,
			w->addrport,
			f->type == WORK_QUEUE_BUFFER ? "literal data" : f->payload,
			total_bytes);
	
		if(result == APP_FAILURE) t->result |= WORK_QUEUE_RESULT_INPUT_MISSING;
	}
	
	free(cached_name);
	return result;
}

//returns 1 on success, 0 on failure to send, and -1 on failure to access locally.
static int send_input_files( struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t )
{
	struct work_queue_file *f;
	struct stat s;

	// Check for existence of each input file first.
	// If any one fails to exist, set the failure condition and return failure.
	if(t->input_files) {
		list_first_item(t->input_files);
		while((f = list_next_item(t->input_files))) {
			if(f->type == WORK_QUEUE_FILE || f->type == WORK_QUEUE_FILE_PIECE) {
				char * expanded_payload = expand_envnames(w, f->payload);
				if(!expanded_payload) {
					t->result |= WORK_QUEUE_RESULT_INPUT_MISSING;
					return APP_FAILURE;	
				}	
				if(stat(expanded_payload, &s) != 0) {
					debug(D_WQ,"Could not stat %s: %s\n", expanded_payload, strerror(errno));
					free(expanded_payload);
					t->result |= WORK_QUEUE_RESULT_INPUT_MISSING;
					return APP_FAILURE;
				}
				free(expanded_payload);
			}
		}
	}

	// Send each of the input files.
	// If any one fails to be sent, return failure.
	if(t->input_files) {
		list_first_item(t->input_files);
		while((f = list_next_item(t->input_files))) {
			int result = send_input_file(q,w,t,f);
			if(result != SUCCESS) return result;
		}
	}

	return SUCCESS;
}

static int start_one_task(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t)
{
	t->time_send_input_start = timestamp_get();
	int result = send_input_files(q, w, t);  
	if (result != SUCCESS) 
		return result;

	t->time_send_input_finish = timestamp_get();
	t->time_execute_cmd_start = timestamp_get();
	t->hostname = xxstrdup(w->hostname);
	t->host = xxstrdup(w->addrport);
	
	send_worker_msg(q,w, "task %lld\n",  (long long) t->taskid);
	send_worker_msg(q,w, "cmd %lld\n%s", (long long) strlen(t->command_line), t->command_line);
	send_worker_msg(q,w, "cores %d\n",   t->cores );
	send_worker_msg(q,w, "memory %"PRId64"\n",  t->memory );
	send_worker_msg(q,w, "disk %"PRId64"\n",    t->disk );
	send_worker_msg(q,w, "gpus %d\n",    t->gpus );

	if(t->input_files) {
		struct work_queue_file *tf;
		list_first_item(t->input_files);
		while((tf = list_next_item(t->input_files))) {
			if(tf->type == WORK_QUEUE_DIRECTORY) {
				send_worker_msg(q,w, "dir %s\n", tf->remote_name);
			} else {
				char *cached_name = make_cached_name(t,tf);
				send_worker_msg(q,w, "infile %s %s %d\n", cached_name, tf->remote_name, tf->flags);
				free(cached_name);
			}
		}
	}

	if(t->output_files) {
		struct work_queue_file *tf;
		list_first_item(t->output_files);
		while((tf = list_next_item(t->output_files))) {
			char *cached_name = make_cached_name(t,tf);
			send_worker_msg(q,w, "outfile %s %s %d\n", cached_name, tf->remote_name, tf->flags);
			free(cached_name);
		}
	}

	// send_worker_msg returns the number of bytes sent, or a number less than
	// zero to indicate errors. We are lazy here, we only check the last
	// message we sent to the worker (other messages may have failed above).
	result = send_worker_msg(q,w, "end\n");

	if(result > -1)
	{
		debug(D_WQ, "%s (%s) busy on '%s'", w->hostname, w->addrport, t->command_line);
		return SUCCESS;
	}
	else
	{
		return WORKER_FAILURE;
	}
}

/*
Store a report summarizing the performance of a completed task.
Keep a list of reports equal to the number of workers connected.
Used for computing queue capacity below.
*/

static void add_task_report( struct work_queue *q, struct work_queue_task *t )
{
	struct work_queue_task_report *tr;

	// Create a new report object and add it to the list.
	tr = malloc(sizeof(struct work_queue_task_report));
	if(!tr) return;
	tr->transfer_time = t->total_transfer_time;
	tr->exec_time     = t->cmd_execution_time;
	list_push_tail(q->task_reports,tr);

	// Trim the list to the current number of useful workers.
	int count = MAX(WORK_QUEUE_TASK_REPORT_MIN_SIZE, hash_table_size(q->worker_table) );
	while(list_size(q->task_reports) >= count) {
	  tr = list_pop_head(q->task_reports);
		free(tr);
	}
}

/*
Compute queue capacity based on stored task reports
and the summary of master activity.
*/

static double compute_capacity( const struct work_queue *q )
{
	timestamp_t avg_transfer_time = 0;
	timestamp_t avg_exec_time = 0;
	int count = 0;

	struct list_node *n;

	// Sum up the task reports available.
	for(n=q->task_reports->head;n;n=n->next) {
		struct work_queue_task_report *tr = n->data;
		avg_transfer_time += tr->transfer_time;
		avg_exec_time += tr->exec_time;
		count++;
	}

	// Compute the average task properties.
	if(count==0) return WORK_QUEUE_DEFAULT_CAPACITY;
	avg_transfer_time /= count;
	avg_exec_time /= count;

	// Compute the average time spent outside of work_queue_wait
	if(q->total_tasks_complete==0) return WORK_QUEUE_DEFAULT_CAPACITY;
	timestamp_t avg_app_time = q->total_app_time / q->total_tasks_complete;

	// Capacity is the ratio of task execution time to time spent in the master doing other things.
	if(avg_transfer_time==0) return WORK_QUEUE_DEFAULT_CAPACITY;
	return (double) avg_exec_time / (avg_transfer_time + avg_app_time);
}

static int check_worker_against_task(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t) {
	int64_t cores_used, disk_used, mem_used, gpus_used;
	int ok = 1;

	if (hash_table_lookup(q->worker_blacklist,w->hostname) && !w->foreman) {
        ok = 0;
		return ok;
    }
	
	if(t->unlabeled)
	{
		// Do not allow labeled/unlabeled mix.
		if(w->cores_allocated > 0 || w->memory_allocated > 0 || w->disk_allocated > 0 || w->gpus_allocated > 0) {
			ok = 0;
		}

		if(w->unlabeled_allocated + 1 > overcommitted_resource_total(q, w->resources->workers.total, 1)) {
			ok = 0;
		}
	} else {
		// Otherwise use any values given, and assume the task will take "whatever it can get" for unlabled resources
		cores_used = MAX(t->cores, 0);
		mem_used = MAX(t->memory, 0);
		disk_used = MAX(t->disk, 0);
		gpus_used = MAX(t->gpus, 0);

		if(w->unlabeled_allocated > 0) {
			ok = 0;
		} else if(w->cores_allocated + cores_used > overcommitted_resource_total(q, w->resources->cores.total, 1)) {
			ok = 0;
		} else if(w->memory_allocated + mem_used > overcommitted_resource_total(q, w->resources->memory.total, 0)) {
			ok = 0;
		} else if(w->disk_allocated + disk_used > w->resources->disk.total) { /* No overcommit disk */
			ok = 0;
		} else if(w->gpus_allocated + gpus_used > overcommitted_resource_total(q, w->resources->gpus.total, 0)) {
			ok = 0;
		}
	}

	return ok;
}


static struct work_queue_worker *find_worker_by_files(struct work_queue *q, struct work_queue_task *t)
{
	char *key;
	struct work_queue_worker *w;
	struct work_queue_worker *best_worker = 0;
	int64_t most_task_cached_bytes = 0;
	int64_t task_cached_bytes;
	struct stat *remote_info;
	struct work_queue_file *tf;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if( check_worker_against_task(q, w, t) ) {
			task_cached_bytes = 0;
			list_first_item(t->input_files);
			while((tf = list_next_item(t->input_files))) {
				if((tf->type == WORK_QUEUE_FILE || tf->type == WORK_QUEUE_FILE_PIECE) && (tf->flags & WORK_QUEUE_CACHE)) {
					char *cached_name = make_cached_name(t,tf);
					remote_info = hash_table_lookup(w->current_files, cached_name);
					if(remote_info)
						task_cached_bytes += remote_info->st_size;
					free(cached_name);
				}
			}

			if(!best_worker || task_cached_bytes > most_task_cached_bytes) {
				best_worker = w;
				most_task_cached_bytes = task_cached_bytes;
			}
		}
	}

	return best_worker;
}

static struct work_queue_worker *find_worker_by_fcfs(struct work_queue *q, struct work_queue_task *t)
{
	char *key;
	struct work_queue_worker *w;
	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w)) {
		if( check_worker_against_task(q, w, t) ) {
			return w;
		}
	}
	return NULL;
}

static struct work_queue_worker *find_worker_by_random(struct work_queue *q, struct work_queue_task *t)
{
	char *key;
	struct work_queue_worker *w = NULL;
	struct list *valid_workers = list_create();
	int random_worker;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w)) {
		if(check_worker_against_task(q, w, t)) {
			list_push_tail(valid_workers, w);
		}
	}

	if(list_size(valid_workers) > 0) {
		random_worker = (rand() % list_size(valid_workers)) + 1;
	} else {
		list_delete(valid_workers);
		return NULL;
	}

	w = NULL;
	while(random_worker && list_size(valid_workers)) {
		w = list_pop_head(valid_workers);
		random_worker--;
	}
	list_delete(valid_workers);
	
	return w;
}

static struct work_queue_worker *find_worker_by_time(struct work_queue *q, struct work_queue_task *t)
{
	char *key;
	struct work_queue_worker *w;
	struct work_queue_worker *best_worker = 0;
	double best_time = HUGE_VAL;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if(check_worker_against_task(q, w, t)) {
			if(w->total_tasks_complete > 0) {
				double t = (w->total_task_time + w->total_transfer_time) / w->total_tasks_complete;
				if(!best_worker || t < best_time) {
					best_worker = w;
					best_time = t;
				}
			}
		}
	}

	if(best_worker) {
		return best_worker;
	} else {
		return find_worker_by_fcfs(q, t);
	}
}

// use task-specific algorithm if set, otherwise default to the queue's setting.
static struct work_queue_worker *find_best_worker(struct work_queue *q, struct work_queue_task *t)
{
	int a = t->worker_selection_algorithm;

	if(a == WORK_QUEUE_SCHEDULE_UNSET) {
		a = q->worker_selection_algorithm;
	}

	switch (a) {
	case WORK_QUEUE_SCHEDULE_FILES:
		return find_worker_by_files(q, t);
	case WORK_QUEUE_SCHEDULE_TIME:
		return find_worker_by_time(q, t);
	case WORK_QUEUE_SCHEDULE_RAND:
		return find_worker_by_random(q, t);
	case WORK_QUEUE_SCHEDULE_FCFS:
	default:
		return find_worker_by_fcfs(q, t);
	}
}

static void commit_task_to_worker(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t)
{
	t->time_committed = timestamp_get();

	itable_insert(w->current_tasks, t->taskid, t);
	itable_insert(q->running_tasks, t->taskid, t); 
	itable_insert(q->worker_task_map, t->taskid, w); //add worker as execution site for t.

	t->total_submissions += 1;

	if(t->unlabeled) {
		w->unlabeled_allocated++;
	} else {
		// Otherwise use any values given, and assume the task will take "whatever it can get" for unlabeled resources
		w->cores_allocated  += MAX(t->cores, 0);
		w->memory_allocated += MAX(t->memory,0);
		w->disk_allocated   += MAX(t->disk,  0);
		w->gpus_allocated   += MAX(t->gpus,  0);
	}

	log_worker_stats(q);
}

static void reap_task_from_worker(struct work_queue *q, struct work_queue_worker *w, struct work_queue_task *t)
{
	struct work_queue_worker *wr = itable_lookup(q->worker_task_map, t->taskid);

	if(wr != w)
	{
		debug(D_WQ, "Cannot reap task %d from worker. It is not being run by %s (%s)\n", t->taskid, w->hostname, w->addrport);
	}

	//update tables.
	itable_remove(w->current_tasks, t->taskid);
	itable_remove(q->running_tasks, t->taskid); 
	itable_remove(q->worker_task_map, t->taskid);
	
	if(t->unlabeled)
	{
		w->unlabeled_allocated--;

	}
	else
	{
		w->cores_allocated  -= MAX(t->cores, 0);
		w->memory_allocated -= MAX(t->memory,0);
		w->disk_allocated   -= MAX(t->disk,  0);
		w->gpus_allocated   -= MAX(t->gpus,  0);
	}

	if(w->unlabeled_allocated < 0 || w->cores_allocated < 0 || w->memory_allocated < 0 || w->disk_allocated < 0 || w->gpus_allocated < 0)
	{
		w->unlabeled_allocated = MAX(w->unlabeled_allocated,0);
		w->cores_allocated     = MAX(w->cores_allocated,0);
		w->memory_allocated    = MAX(w->memory_allocated,0);
		w->disk_allocated      = MAX(w->disk_allocated,0);
		w->gpus_allocated      = MAX(w->gpus_allocated,0);

		debug(D_WQ, "Worker resource accounting error! Task: %d. Worker: %s (%s) Reseting to zero.\n", t->taskid, w->hostname, w->addrport);
	}

	log_worker_stats(q);
}

static void start_task_on_worker(struct work_queue *q, struct work_queue_worker *w)
{
	struct work_queue_task *t = list_pop_head(q->ready_list);
	if(!t)
		return;

	commit_task_to_worker(q, w, t);
	int result = start_one_task(q, w, t);
	if(result != SUCCESS) {
		debug(D_WQ, "Failed to send task %d to worker %s (%s).", t->taskid, w->hostname, w->addrport);
		handle_failure(q, w, t, result);
	}
	
	return;
}

static int start_tasks(struct work_queue *q, time_t stoptime)
{			
	//start as many tasks as possible
	struct work_queue_task *t;
	struct work_queue_worker *w;

	int task_started = 0;

	if(list_size(q->ready_list) > 0)
	{
		// Start at least one task, regardless of the stoptime value.
		do {
			t = list_peek_head(q->ready_list);
			w = find_best_worker(q, t);
			if(w) {
				start_task_on_worker(q, w);
				task_started++;
			} else {
				//Move task to the end of queue when there is at least one available worker.  
				//This prevents a resource-hungry task from clogging the entire queue.
				if(available_workers(q) > 0) {
					list_push_tail(q->ready_list, list_pop_head(q->ready_list));
				}	
				break;
			}
			//stoptime <= 0 means an infinite timeout
		} while(list_size(q->ready_list) && (stoptime <= 0 || time(0) < stoptime));
	}

	return task_started;
}

static int receive_tasks(struct work_queue *q, time_t stoptime)
{
	struct work_queue_task *t;

	int tasks_received = 0;

	struct work_queue_worker *w;
	uint64_t taskid;

	if(itable_size(q->finished_tasks) > 0)
	{
		itable_firstkey(q->finished_tasks);
		// Receive at least one task, regardless of the stoptime value.
		do {
			itable_nextkey(q->finished_tasks, &taskid, (void **)&t);

			w = itable_lookup(q->worker_task_map, taskid);
			fetch_output_from_worker(q, w, taskid);
			itable_firstkey(q->finished_tasks);  // fetch_output removes the resolved task from the itable, thus potentially corrupting our current location.  This resets it to the top.
			tasks_received++;

			//stoptime <= 0 means an infinite timeout
		} while(itable_size(q->finished_tasks) && (stoptime <=0 || time(0) < stoptime));
	}

	return tasks_received;
}


//Sends keepalives to check if connected workers are responsive. If not, removes those workers. 
static void remove_unresponsive_workers(struct work_queue *q) {
	struct work_queue_worker *w;
	char *key;
	int last_recv_elapsed_time;
	timestamp_t current_time = timestamp_get();
	
	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if(q->keepalive_interval > 0) {
			if(!strcmp(w->hostname, "unknown")){ 
				last_recv_elapsed_time = (int)(current_time - w->start_time)/1000000;
			} else {
				last_recv_elapsed_time = (int)(current_time - w->last_msg_recv_time)/1000000;
			}
		
			// send new keepalive check only (1) if we received a response since last keepalive check AND 
			// (2) we are past keepalive interval 
			if(w->last_msg_recv_time >= w->keepalive_check_sent_time) {	
				if(last_recv_elapsed_time >= q->keepalive_interval) {
					if(send_worker_msg(q,w, "check\n")<0) {
						debug(D_WQ, "Failed to send keepalive check to worker %s (%s).", w->hostname, w->addrport);
						handle_worker_failure(q, w);
					} else {
						debug(D_WQ, "Sent keepalive check to worker %s (%s)", w->hostname, w->addrport);
						w->keepalive_check_sent_time = current_time;	
					}	
				}
			} else { 
				// we haven't received a message from worker since its last keepalive check. Check if time 
				// since we last polled link for responses has exceeded keepalive timeout. If so, remove worker.
				if (q->link_poll_end > w->keepalive_check_sent_time) {
					if ((int)((q->link_poll_end - w->keepalive_check_sent_time)/1000000) >= q->keepalive_timeout) { 
						debug(D_WQ, "Removing worker %s (%s): hasn't responded to keepalive check for more than %d s", w->hostname, w->addrport, q->keepalive_timeout);
						handle_worker_failure(q, w);
					}
				}
			}
		}
	}
}

static void abort_slow_workers(struct work_queue *q)
{
	struct work_queue_worker *w;
	struct work_queue_task *t;
	uint64_t key;
	const double multiplier = q->fast_abort_multiplier;

	if(q->total_tasks_complete - q->total_tasks_failed < 10)
		return;

	timestamp_t average_task_time = (q->total_good_execute_time + q->total_good_transfer_time) / (q->total_tasks_complete - q->total_tasks_failed);
	timestamp_t current = timestamp_get();

	itable_firstkey(q->running_tasks);
	while(itable_nextkey(q->running_tasks, &key, (void **) &t)) {
		timestamp_t runtime = current - t->time_send_input_start;
		if(runtime > (average_task_time * multiplier)) {
			w = itable_lookup(q->worker_task_map, t->taskid);
			if(!w->foreman)
			{
				debug(D_WQ, "Removing worker %s (%s): takes too long to execute the current task - %.02lf s (average task execution time by other workers is %.02lf s)", w->hostname, w->addrport, runtime / 1000000.0, average_task_time / 1000000.0);
				remove_worker(q, w);
			}
		}
	}
}

static int shut_down_worker(struct work_queue *q, struct work_queue_worker *w)
{
	if(!w) return 0;
	send_worker_msg(q,w,"exit\n");
	remove_worker(q, w);
	return 1;
}

//comparator function for checking if a task matches given taskid.
static int taskid_comparator(void *t, const void *r) {

	struct work_queue_task *task_in_queue = t;
	const int *taskid = r;

	if (task_in_queue->taskid == *taskid) {
		return 1;
	}	
	return 0;
}

//comparator function for checking if a task matches given tag.
static int tasktag_comparator(void *t, const void *r) {

	struct work_queue_task *task_in_queue = t;
	const char *tasktag = r;

	if (!strcmp(task_in_queue->tag, tasktag)) {
		return 1;
	}	
	return 0;
}


static int cancel_running_task(struct work_queue *q, struct work_queue_task *t) {

	struct work_queue_worker *w = itable_lookup(q->worker_task_map, t->taskid);
	
	if (w) {
		//send message to worker asking to kill its task.
		send_worker_msg(q,w, "kill %d\n",t->taskid);
		debug(D_WQ, "Task with id %d is aborted at worker %s (%s) and removed.", t->taskid, w->hostname, w->addrport);
			
		//Delete any input files that are not to be cached.
		delete_worker_files(q, w, t, t->input_files, WORK_QUEUE_CACHE | WORK_QUEUE_PREEXIST);

		//Delete all output files since they are not needed as the task was aborted.
		delete_worker_files(q, w, t, t->output_files, 0);

		//update tables.
		reap_task_from_worker(q, w, t);

		itable_remove(q->finished_tasks, t->taskid);

		return 1;
	}
	
	return 0;
}

static struct work_queue_task *find_running_task_by_id(struct work_queue *q, int taskid) {
	
	struct work_queue_task *t;

	t=itable_lookup(q->running_tasks, taskid);
	if (t){
		return t;
	}
	
	t = itable_lookup(q->finished_tasks, taskid);
	if(t) {
		return t;
	}
	
	return NULL;
}

static struct work_queue_task *find_running_task_by_tag(struct work_queue *q, const char *tasktag) {
	
	struct work_queue_task *t;
	uint64_t taskid;

	itable_firstkey(q->running_tasks);
	while(itable_nextkey(q->running_tasks, &taskid, (void**)&t)) {
		if (tasktag_comparator(t, tasktag)) {
			return t;
		}
	}

	itable_firstkey(q->finished_tasks);
	while(itable_nextkey(q->finished_tasks, &taskid, (void**)&t)) {
		if (tasktag_comparator(t, tasktag)) {
			return t;
		}
	}
	
	return NULL;
}


static struct work_queue_file *work_queue_file_clone(const struct work_queue_file *file) {
  const int file_t_size = sizeof(struct work_queue_file);
  struct work_queue_file *new = xxmalloc(file_t_size);
  
  memcpy(new, file, file_t_size);
  //allocate new memory for strings so we don't segfault when the original
  //memory is freed. 
  new->payload = xxstrdup(file->payload);
  new->remote_name = xxstrdup(file->remote_name);
  
  return new;
}


static struct list *work_queue_task_file_list_clone(struct list *list) {
  struct list *new = list_create();
  struct work_queue_file *old_file, *new_file;

  list_first_item(list);
  while ((old_file = list_next_item(list))) {
    new_file = work_queue_file_clone(old_file);
    list_push_tail(new, new_file);
  }
  return new;
}


/******************************************************/
/********** work_queue_task public functions **********/
/******************************************************/

struct work_queue_task *work_queue_task_create(const char *command_line)
{
	struct work_queue_task *t = malloc(sizeof(*t));
	if(!t) {
		fprintf(stderr, "Error: failed to allocate memory for task.\n");
		return NULL;
	}
	memset(t, 0, sizeof(*t));

	if(command_line) t->command_line = xxstrdup(command_line);

	t->worker_selection_algorithm = WORK_QUEUE_SCHEDULE_UNSET;
	t->input_files = list_create();
	t->output_files = list_create();
	t->return_status = -1;

	t->time_committed = 0;
	t->time_execute_cmd_start = 0;
	t->total_cmd_execution_time = 0;

	/* In the absence of additional information, a task consumes an entire worker. */
	t->memory = -1;
	t->disk = -1;
	t->cores = -1;
	t->gpus = -1;
	t->unlabeled = 1;

	return t;
}

struct work_queue_task *work_queue_task_clone(const struct work_queue_task *task)
{
  struct work_queue_task *new = xxmalloc(sizeof(*new));
  memcpy(new, task, sizeof(*new));

  //allocate new memory so we don't segfault when original memory is freed. 
  if(task->tag) { 
	new->tag = xxstrdup(task->tag); 
  }
  
  if(task->command_line) { 
	new->command_line = xxstrdup(task->command_line); 
  }

  new->input_files = work_queue_task_file_list_clone(task->input_files);
  new->output_files = work_queue_task_file_list_clone(task->output_files);

  if(task->output) { 
	new->output = xxstrdup(task->output); 
  }
  
  if(task->host) { 
	new->host = xxstrdup(task->host); 
  }
  
  if(task->hostname) { 
	new->hostname = xxstrdup(task->hostname); 
  }

  return new;
}


void work_queue_task_specify_command( struct work_queue_task *t, const char *cmd )
{
	if(t->command_line) free(t->command_line);
	t->command_line = xxstrdup(cmd);
}


static void set_task_unlabel_flag( struct work_queue_task *t )
{
	if(t->cores < 0 && t->memory < 0 && t->disk < 0 && t->gpus < 0)
	{
		t->unlabeled = 1;
	}
}

void work_queue_task_specify_memory( struct work_queue_task *t, int64_t memory )
{
	if(memory < 0)
	{
		t->memory = -1;
	}
	else
	{
		t->memory = memory;
		t->unlabeled = 0;
	}

	set_task_unlabel_flag(t);
}

void work_queue_task_specify_disk( struct work_queue_task *t, int64_t disk )
{
	if(disk < 0)
	{
		t->disk = -1;
	}
	else
	{
		t->disk = disk;
		t->unlabeled = 0;
	}

	set_task_unlabel_flag(t);
}

void work_queue_task_specify_cores( struct work_queue_task *t, int cores )
{
	if(cores < 0)
	{
		t->cores = -1;
	}
	else
	{
		t->cores = cores;
		t->unlabeled = 0;
	}

	set_task_unlabel_flag(t);
}

void work_queue_task_specify_gpus( struct work_queue_task *t, int gpus )
{
	if(gpus < 0)
	{
		t->gpus = -1;
	}
	else
	{
		t->gpus = gpus;
		t->unlabeled = 0;
	}

	set_task_unlabel_flag(t);
}

void work_queue_task_specify_tag(struct work_queue_task *t, const char *tag)
{
	if(t->tag)
		free(t->tag);
	t->tag = xxstrdup(tag);
}

struct work_queue_file * work_queue_file_create(const char * remote_name, int type, int flags)
{
	struct work_queue_file *f;
	
	f = malloc(sizeof(*f));
	if(!f) {
		debug(D_NOTICE, "Cannot allocate memory for file %s.\n", remote_name);
		return NULL;
	}

	memset(f, 0, sizeof(*f));
	
	f->remote_name = xxstrdup(remote_name);
	f->type = type;
	f->flags = flags;
	
	return f;
}

int work_queue_task_specify_url(struct work_queue_task *t, const char *file_url, const char *remote_name, int type, int flags)
{
	struct list *files;
	struct work_queue_file *tf;

	if(!t || !file_url || !remote_name) {
		fprintf(stderr, "Error: Null arguments for task, url, and remote name not allowed in specify_url.\n");
		return 0;
	}
	if(remote_name[0] == '/') {
		fprintf(stderr, "Error: Remote name %s contains absolute path.\n", remote_name);
		return 0;
	}

	if(type == WORK_QUEUE_INPUT) {
		files = t->input_files;
		
		//check if two different urls map to the same remote name for inputs. 	
		list_first_item(t->input_files);
		while((tf = (struct work_queue_file*)list_next_item(files))) {
			if(!strcmp(remote_name, tf->remote_name) && strcmp(file_url, tf->payload)) {
				fprintf(stderr, "Error: input url %s conflicts with another input pointing to same remote name (%s).\n", file_url, remote_name);
				return 0;       
			}
		}
		//check if there is an output file with the same remote name. 
		list_first_item(t->output_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name)){
				fprintf(stderr, "Error: input url %s conflicts with an output pointing to same remote name (%s).\n", file_url, remote_name);
				return 0;	
			}
		}
	} else {
		files = t->output_files;
		
		//check if two different different remote names map to the same url for outputs. 	
		list_first_item(t->output_files);
		while((tf = (struct work_queue_file*)list_next_item(files))) {
			if(!strcmp(file_url, tf->payload) && strcmp(remote_name, tf->remote_name)) {
				fprintf(stderr, "Error: output url remote name %s conflicts with another output pointing to same url (%s).\n", remote_name, file_url);
				return 0;       
			}
		}
		
		//check if there is an input file with the same remote name. 
		list_first_item(t->input_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name)){
				fprintf(stderr, "Error: output url %s conflicts with an input pointing to same remote name (%s).\n", file_url, remote_name);
				return 0;	
			}
		}
	}

	tf = work_queue_file_create(remote_name, WORK_QUEUE_URL, flags);
	if(!tf) return 0;

	tf->length = strlen(file_url);
	tf->payload = xxstrdup(file_url);

	list_push_tail(files, tf);

	return 1;
}

int work_queue_task_specify_file(struct work_queue_task *t, const char *local_name, const char *remote_name, int type, int flags)
{
	struct list *files;
	struct work_queue_file *tf;
	
	if(!t || !local_name || !remote_name) {
		fprintf(stderr, "Error: Null arguments for task, local name, and remote name not allowed in specify_file.\n");
		return 0;
	}

	// @param remote_name is the path of the file as on the worker machine. In
	// the Work Queue framework, workers are prohibitted from writing to paths
	// outside of their workspaces. When a task is specified, the workspace of
	// the worker(the worker on which the task will be executed) is unlikely to
	// be known. Thus @param remote_name should not be an absolute path.
	if(remote_name[0] == '/') {
		fprintf(stderr, "Error: Remote name %s contains absolute path.\n", remote_name);
		return 0;
	}
	
	
	if(type == WORK_QUEUE_INPUT) {
		files = t->input_files;
		
		//check if two different local names map to the same remote name for inputs.	
		list_first_item(t->input_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name) && strcmp(local_name, tf->payload)){
				fprintf(stderr, "Error: input file %s conflicts with another input pointing to same remote name (%s).\n", local_name, remote_name);
				return 0;	
			}
		} 
		
		//check if there is an output file with the same remote name. 
		list_first_item(t->output_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name)){
				fprintf(stderr, "Error: input file %s conflicts with an output pointing to same remote name (%s).\n", local_name, remote_name);
				return 0;	
			}
		} 	
	} else {
		files = t->output_files;
		
		//check if two different different remote names map to the same local name for outputs. 	
		list_first_item(files);
		while((tf = (struct work_queue_file*)list_next_item(files))) {
			if(!strcmp(local_name, tf->payload) && strcmp(remote_name, tf->remote_name)) {
				fprintf(stderr, "Error: output file %s conflicts with another output pointing to same remote name (%s).\n", local_name, remote_name);
				return 0;       
			}
		}
		
		//check if there is an input file with the same remote name. 
		list_first_item(t->input_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name)){
				fprintf(stderr, "Error: output file %s conflicts with an input pointing to same remote name (%s).\n", local_name, remote_name);
				return 0;	
			}
		}
	}
	
	tf = work_queue_file_create(remote_name, WORK_QUEUE_FILE, flags);
	if(!tf) return 0;

	tf->length = strlen(local_name);
	tf->payload = xxstrdup(local_name);

	list_push_tail(files, tf);
	return 1;
}

int work_queue_task_specify_directory(struct work_queue_task *t, const char *local_name, const char *remote_name, int type, int flags, int recursive) {
	struct list *files;
	struct work_queue_file *tf;
	
	if(!t || !remote_name) {
		fprintf(stderr, "Error: Null arguments for task and remote name not allowed in specify_directory.\n");
		return 0;
	}

	// @param remote_name is the path of the file as on the worker machine. In
	// the Work Queue framework, workers are prohibitted from writing to paths
	// outside of their workspaces. When a task is specified, the workspace of
	// the worker(the worker on which the task will be executed) is unlikely to
	// be known. Thus @param remote_name should not be an absolute path.
	if(remote_name[0] == '/') {
		fprintf(stderr, "Error: Remote name %s contains absolute path.\n", remote_name);
		return 0;
	}

	if(type == WORK_QUEUE_OUTPUT || recursive) {
		return work_queue_task_specify_file(t, local_name, remote_name, type, flags);
	}
	
	files = t->input_files;
	
	list_first_item(files);
	while((tf = (struct work_queue_file*)list_next_item(files))) {
		if(!strcmp(remote_name, tf->remote_name))
		{	return 0;	}
	}

	tf = work_queue_file_create(remote_name, WORK_QUEUE_DIRECTORY, flags);
	if(!tf) return 0;

	//KNOWN HACK: Every file passes through make_cached_name() which expects the
	//payload field to be set. So we simply set the payload to remote name if
	//local name is null. This doesn't affect the behavior of the file transfers.
	if(local_name) {
		tf->length = strlen(local_name);
		tf->payload = xxstrdup(local_name);
	} else {
		tf->length = strlen(remote_name);
		tf->payload = xxstrdup(remote_name);
	}

	list_push_tail(files, tf);
	return 1;
	
}

int work_queue_task_specify_file_piece(struct work_queue_task *t, const char *local_name, const char *remote_name, off_t start_byte, off_t end_byte, int type, int flags)
{
	struct list *files;
	struct work_queue_file *tf;
	if(!t || !local_name || !remote_name) {
		fprintf(stderr, "Error: Null arguments for task, local name, and remote name not allowed in specify_file_piece.\n");
		return 0;
	}

	// @param remote_name should not be an absolute path. @see
	// work_queue_task_specify_file
	if(remote_name[0] == '/') {
		fprintf(stderr, "Error: Remote name %s contains absolute path.\n", remote_name);
		return 0;
	}

	if(end_byte < start_byte) {
		fprintf(stderr, "Error: End byte lower than start byte for %s.\n", remote_name);
		return 0;
	}

	if(type == WORK_QUEUE_INPUT) {
		files = t->input_files;
		
		//check if two different local names map to the same remote name for inputs.	
		list_first_item(t->input_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name) && strcmp(local_name, tf->payload)){
				fprintf(stderr, "Error: piece of input file %s conflicts with another input pointing to same remote name (%s).\n", local_name, remote_name);
				return 0;	
			}
		} 
		
		//check if there is an output file with the same remote name. 
		list_first_item(t->output_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name)){
				fprintf(stderr, "Error: piece of input file %s conflicts with an output pointing to same remote name (%s).\n", local_name, remote_name);
				return 0;	
			}
		}
	} else {
		files = t->output_files;
		
		//check if two different different remote names map to the same local name for outputs. 	
		list_first_item(files);
		while((tf = (struct work_queue_file*)list_next_item(files))) {
			if(!strcmp(local_name, tf->payload) && strcmp(remote_name, tf->remote_name)) {
				fprintf(stderr, "Error: piece of output file %s conflicts with another output pointing to same remote name (%s).\n", local_name, remote_name);
				return 0;       
			}
		}
		
		//check if there is an input file with the same remote name. 
		list_first_item(t->input_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name)){
				fprintf(stderr, "Error: piece of output file %s conflicts with an input pointing to same remote name (%s).\n", local_name, remote_name);
				return 0;	
			}
		}
	}
	
	tf = work_queue_file_create(remote_name, WORK_QUEUE_FILE_PIECE, flags);
	if(!tf) return 0;

	tf->length = strlen(local_name);
	tf->offset = start_byte;
	tf->piece_length = end_byte - start_byte + 1;
	tf->payload = xxstrdup(local_name);

	list_push_tail(files, tf);
	return 1;
}

int work_queue_task_specify_buffer(struct work_queue_task *t, const char *data, int length, const char *remote_name, int flags)
{
	struct work_queue_file *tf;
	if(!t || !remote_name) {
		fprintf(stderr, "Error: Null arguments for task and remote name not allowed in specify_buffer.\n");
		return 0;
	}

	// @param remote_name should not be an absolute path. @see
	// work_queue_task_specify_file
	if(remote_name[0] == '/') {
		fprintf(stderr, "Error: Remote name %s contains absolute path.\n", remote_name);
		return 0;
	}

	list_first_item(t->input_files);
	while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
		if(!strcmp(remote_name, tf->remote_name)) {	
			fprintf(stderr, "Error: buffer conflicts with another input pointing to same remote name (%s).\n", remote_name);
			return 0;	
		}
	}
	
	list_first_item(t->output_files);
	while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
		if(!strcmp(remote_name, tf->remote_name)) {	
			fprintf(stderr, "Error: buffer conflicts with an output pointing to same remote name (%s).\n", remote_name);
			return 0;	
		}
	}	
	
	tf = work_queue_file_create(remote_name, WORK_QUEUE_BUFFER, flags);
	if(!tf) return 0;
	
	tf->length = length;
	tf->payload = malloc(length);
	if(!tf->payload) {
		fprintf(stderr, "Error: failed to allocate memory for buffer with remote name %s and length %d bytes.\n", remote_name, length);
		return 0;	
	}
	
	memcpy(tf->payload, data, length);
	list_push_tail(t->input_files, tf);

	return 1;
}

int work_queue_task_specify_file_command(struct work_queue_task *t, const char *remote_name, const char *cmd, int type, int flags)
{
	struct list *files;
	struct work_queue_file *tf;
	if(!t || !remote_name || !cmd) {
		fprintf(stderr, "Error: Null arguments for task, remote name, and command not allowed in specify_file_command.\n");
		return 0;
	}

	// @param remote_name should not be an absolute path. @see
	// work_queue_task_specify_file
	if(remote_name[0] == '/') {
		fprintf(stderr, "Error: Remote name %s contains absolute path.\n", remote_name);
		return 0;
	}

	if(type == WORK_QUEUE_INPUT) {
		files = t->input_files;
		
		//check if two different local names map to the same remote name for inputs.	
		list_first_item(t->input_files);
		while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name) && strcmp(cmd, tf->payload)){
				fprintf(stderr, "Error: input file command %s conflicts with another input pointing to same remote name (%s).\n", cmd, remote_name);
				return 0;	
			}
		} 
		
		//check if there is an output file with the same remote name. 
		list_first_item(t->output_files);
	    while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name)) {
				fprintf(stderr, "Error: input file command %s conflicts with an output pointing to same remote name (%s).\n", cmd, remote_name);
				return 0;	
			}
		} 	
	} else {
		files = t->output_files;
		
		//check if two different different remote names map to the same local name for outputs. 	
		list_first_item(files);
		while((tf = (struct work_queue_file*)list_next_item(files))) {
			if(!strcmp(cmd, tf->payload) && strcmp(remote_name, tf->remote_name)) {
				fprintf(stderr, "Error: output file command %s conflicts with another output pointing to same remote name (%s).\n", cmd, remote_name);
				return 0;       
			}
		}
		
		//check if there is an input file with the same remote name. 
		list_first_item(t->input_files);
	    while((tf = (struct work_queue_file*)list_next_item(t->input_files))) {
			if(!strcmp(remote_name, tf->remote_name)){
				fprintf(stderr, "Error: output file command %s conflicts with an input pointing to same remote name (%s).\n", cmd, remote_name);
				return 0;	
			}
		}
	}
	
	tf = work_queue_file_create(remote_name, WORK_QUEUE_REMOTECMD, flags);
	if(!tf) return 0;

	tf->length = strlen(cmd);
	tf->payload = xxstrdup(cmd);

	list_push_tail(files, tf);
	
	return 1;
}

void work_queue_task_specify_algorithm(struct work_queue_task *t, int alg)
{
	t->worker_selection_algorithm = alg;
}

void work_queue_task_delete(struct work_queue_task *t)
{
	struct work_queue_file *tf;
	if(t) {
		if(t->command_line)
			free(t->command_line);
		if(t->tag)
			free(t->tag);
		if(t->output)
			free(t->output);
		if(t->input_files) {
			while((tf = list_pop_tail(t->input_files))) {
				if(tf->payload)
					free(tf->payload);
				if(tf->remote_name)
					free(tf->remote_name);
				free(tf);
			}
			list_delete(t->input_files);
		}
		if(t->output_files) {
			while((tf = list_pop_tail(t->output_files))) {
				if(tf->payload)
					free(tf->payload);
				if(tf->remote_name)
					free(tf->remote_name);
				free(tf);
			}
			list_delete(t->output_files);
		}
		if(t->hostname)
			free(t->hostname);
		if(t->host)
			free(t->host);
		free(t);
	}
}

/** DEPRECATED FUNCTIONS **/
int work_queue_task_specify_output_file(struct work_queue_task *t, const char *rname, const char *fname)
{
	return work_queue_task_specify_file(t, fname, rname, WORK_QUEUE_OUTPUT, WORK_QUEUE_CACHE);
}

int work_queue_task_specify_output_file_do_not_cache(struct work_queue_task *t, const char *rname, const char *fname)
{
	return work_queue_task_specify_file(t, fname, rname, WORK_QUEUE_OUTPUT, WORK_QUEUE_NOCACHE);
}

int work_queue_task_specify_input_buf(struct work_queue_task *t, const char *buf, int length, const char *rname)
{
	return work_queue_task_specify_buffer(t, buf, length, rname, WORK_QUEUE_NOCACHE);
}

int work_queue_task_specify_input_file(struct work_queue_task *t, const char *fname, const char *rname)
{
	return work_queue_task_specify_file(t, fname, rname, WORK_QUEUE_INPUT, WORK_QUEUE_CACHE);
}

int work_queue_task_specify_input_file_do_not_cache(struct work_queue_task *t, const char *fname, const char *rname)
{
	return work_queue_task_specify_file(t, fname, rname, WORK_QUEUE_INPUT, WORK_QUEUE_NOCACHE);
}



/******************************************************/
/********** work_queue public functions **********/
/******************************************************/

struct work_queue *work_queue_create(int port)
{
	struct work_queue *q = malloc(sizeof(*q));
	if(!q) {
		fprintf(stderr, "Error: failed to allocate memory for queue.\n");
		return 0;
	}
	char *envstring;

	random_init();

	memset(q, 0, sizeof(*q));

	if(port == 0) {
		envstring = getenv("WORK_QUEUE_PORT");
		if(envstring) {
			port = atoi(envstring);
		}
	}

	/* compatibility code */
	if (getenv("WORK_QUEUE_LOW_PORT"))
		setenv("TCP_LOW_PORT", getenv("WORK_QUEUE_LOW_PORT"), 0);
	if (getenv("WORK_QUEUE_HIGH_PORT"))
		setenv("TCP_HIGH_PORT", getenv("WORK_QUEUE_HIGH_PORT"), 0);

	q->master_link = link_serve(port);

	if(!q->master_link) {
		debug(D_NOTICE, "Could not create work_queue on port %i.", port);
		free(q);
		return 0;
	} else {
		char address[LINK_ADDRESS_MAX];
		link_address_local(q->master_link, address, &q->port);
	}

	getcwd(q->workingdir,PATH_MAX);

	q->ready_list = list_create();
	q->running_tasks = itable_create(0);
	q->finished_tasks = itable_create(0);
	q->complete_list = list_create();

	q->worker_table = hash_table_create(0, 0);
	q->worker_blacklist = hash_table_create(0, 0);
	q->worker_task_map = itable_create(0);
	
	q->workers_with_available_results = hash_table_create(0, 0);
	
	// The poll table is initially null, and will be created
	// (and resized) as needed by build_poll_table.
	q->poll_table_size = 8;

	q->fast_abort_multiplier = wq_option_fast_abort_multiplier;
	q->worker_selection_algorithm = wq_option_scheduler;
	q->task_ordering = WORK_QUEUE_TASK_ORDER_FIFO;
	q->process_pending_check = 0;
	q->workers_to_wait = 0;

	q->short_timeout = 5;
	q->long_timeout = 3600;

	q->start_time = timestamp_get();
	q->task_reports = list_create();

	q->catalog_host = 0;
	q->catalog_port = 0;

	q->keepalive_interval = WORK_QUEUE_DEFAULT_KEEPALIVE_INTERVAL;
	q->keepalive_timeout = WORK_QUEUE_DEFAULT_KEEPALIVE_TIMEOUT; 

	q->monitor_mode   =  0;
	q->password = 0;
	
	q->asynchrony_multiplier = 1.0;
	q->asynchrony_modifier = 0;

	q->minimum_transfer_timeout = 10;
	q->foreman_transfer_timeout = 3600;
	q->transfer_outlier_factor = 10;
	q->default_transfer_rate = 1*MEGABYTE;
	
	if( (envstring  = getenv("WORK_QUEUE_BANDWIDTH")) ) {
		q->bandwidth = string_metric_parse(envstring);
		if(q->bandwidth < 0) {
			q->bandwidth = 0;
		}
	}
	
	debug(D_WQ, "Work Queue is listening on port %d.", q->port);
	return q;
}

int work_queue_enable_monitoring(struct work_queue *q, char *monitor_summary_file)
{
  if(!q)
    return 0;

  if(q->monitor_mode)
  {
    debug(D_NOTICE, "Monitoring already enabled. Closing old logfile and opening (perhaps) new one.\n");
    if(close(q->monitor_fd))
      debug(D_NOTICE, "Error closing logfile: %s\n", strerror(errno));
  }

  q->monitor_mode = 0;

  q->monitor_exe = resource_monitor_copy_to_wd(NULL);
  if(!q->monitor_exe)
  {
    debug(D_NOTICE, "Could not find the resource monitor executable. Disabling monitor mode.\n");
    return 0;
  }

  if(monitor_summary_file)
    monitor_summary_file = xxstrdup(monitor_summary_file);
  else
    monitor_summary_file = string_format("wq-%d-resource-usage", getpid());

  q->monitor_fd = open(monitor_summary_file, O_CREAT | O_WRONLY | O_APPEND, 00666);
  free(monitor_summary_file);

  if(q->monitor_fd < 0)
  {
    debug(D_NOTICE, "Could not open monitor log file. Disabling monitor mode.\n");
    return 0;
  }

	q->monitor_mode = 1;

	return 1;
}

int work_queue_activate_fast_abort(struct work_queue *q, double multiplier)
{
	if(multiplier >= 1) {
		q->fast_abort_multiplier = multiplier;
		return 0;
	} else {
		q->fast_abort_multiplier = -1.0;
		return 1;
	}
}

int work_queue_port(struct work_queue *q)
{
	char addr[LINK_ADDRESS_MAX];
	int port;

	if(!q) return 0;

	if(link_address_local(q->master_link, addr, &port)) {
		return port;
	} else {
		return 0;
	}
}

void work_queue_activate_worker_waiting(struct work_queue *q, int value)
{
	q->workers_to_wait = value;
}

void work_queue_specify_estimate_capacity_on(struct work_queue *q, int value)
{
	// always on
}

void work_queue_specify_algorithm(struct work_queue *q, int alg)
{
	q->worker_selection_algorithm = alg;
}

void work_queue_specify_task_order(struct work_queue *q, int order)
{
	q->task_ordering = order;
}

void work_queue_specify_name(struct work_queue *q, const char *name)
{
	if(q->name) free(q->name);
	if(name) {
		q->name = xxstrdup(name);
		setenv("WORK_QUEUE_NAME", q->name, 1);
	} else {
		q->name = 0;
	}
}

const char *work_queue_name(struct work_queue *q)
{
	return q->name;
}

void work_queue_specify_priority(struct work_queue *q, int priority)
{
	q->priority = priority;
}

void work_queue_specify_master_mode(struct work_queue *q, int mode)
{
	// Deprecated: Report to the catalog iff a name is given.
}

void work_queue_specify_catalog_server(struct work_queue *q, const char *hostname, int port)
{
	if(hostname) {
		if(q->catalog_host) free(q->catalog_host);
		q->catalog_host = strdup(hostname);
		setenv("CATALOG_HOST", hostname, 1);
	}
	if(port > 0) {
		char portstr[DOMAIN_NAME_MAX];
		q->catalog_port = port;
		snprintf(portstr, DOMAIN_NAME_MAX, "%d", port);
		setenv("CATALOG_PORT", portstr, 1);
	}
}

void work_queue_specify_password( struct work_queue *q, const char *password )
{
	q->password = xxstrdup(password);
}

int work_queue_specify_password_file( struct work_queue *q, const char *file )
{
	return copy_file_to_buffer(file,&(q->password))>0;
}

void work_queue_delete(struct work_queue *q)
{
	if(q) {
		struct work_queue_worker *w;
		char *key;

		hash_table_firstkey(q->worker_table);
		while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
			release_worker(q, w);
		}
		if(q->name) {
			update_catalog(q, NULL, 1);
		}
		if(q->catalog_host) free(q->catalog_host);
		hash_table_delete(q->worker_table);
		hash_table_delete(q->worker_blacklist);
		itable_delete(q->worker_task_map);
		
		list_delete(q->ready_list);
		itable_delete(q->running_tasks);
		itable_delete(q->finished_tasks);
		list_delete(q->complete_list);

		hash_table_delete(q->workers_with_available_results);
		
		list_free(q->task_reports);
		list_delete(q->task_reports);
 
		free(q->poll_table);
		link_close(q->master_link);
		if(q->logfile) {
			fclose(q->logfile);
		}
		free(q);
	}
}

int work_queue_monitor_wrap(struct work_queue *q, struct work_queue_task *t)
{
	char *wrap_cmd; 
	char *template = string_format(RESOURCE_MONITOR_TASK_SUMMARY_NAME, getpid(), t->taskid);
	char *summary  = string_format("%s.summary", template);
	
	wrap_cmd = resource_monitor_rewrite_command(t->command_line, NULL, template, NULL, NULL, 1, 0, 0);

	//BUG: what if user changes current working directory?
	work_queue_task_specify_file(t, q->monitor_exe, q->monitor_exe, WORK_QUEUE_INPUT, WORK_QUEUE_CACHE);
	work_queue_task_specify_file(t, summary, summary, WORK_QUEUE_OUTPUT, WORK_QUEUE_NOCACHE);

	free(summary);
	free(template);
	free(t->command_line);

	t->command_line = wrap_cmd;

	return 0;
}

int work_queue_submit_internal(struct work_queue *q, struct work_queue_task *t)
{
	/* If the task has been used before, clear out accumlated state. */
	if(t->output) {
		free(t->output);
		t->output = 0;
	}
	if(t->hostname) {
		free(t->hostname);
		t->hostname = 0;
	}
	if(t->host) {
		free(t->host);
		t->host = 0;
	}
	t->total_bytes_received = 0;
	t->total_bytes_sent = 0;
	t->total_transfer_time = 0;
	t->cmd_execution_time = 0;
	t->result = 0;
	
	if(q->monitor_mode)
		work_queue_monitor_wrap(q, t);

	/* Then, add it to the ready list and mark it as submitted. */
	if (q->task_ordering == WORK_QUEUE_TASK_ORDER_LIFO){
		list_push_head(q->ready_list, t);
	}	
	else {
		list_push_tail(q->ready_list, t);
	}	
	t->time_task_submit = timestamp_get();
	q->total_tasks_submitted++;

	return (t->taskid);
}

int work_queue_submit(struct work_queue *q, struct work_queue_task *t)
{
	static int next_taskid = 1;

	t->taskid = next_taskid;

	//Increment taskid. So we get a unique taskid for every submit.
	next_taskid++;

	return work_queue_submit_internal(q, t);
}

void work_queue_blacklist_add(struct work_queue *q, const char *hostname)
{
	if (!hash_table_lookup(q->worker_blacklist, hostname)) {
		hash_table_insert(q->worker_blacklist, hostname, 0);
	}
}

void work_queue_blacklist_remove(struct work_queue *q, const char *hostname)
{
	hash_table_remove(q->worker_blacklist, hostname);
}

void work_queue_blacklist_clear(struct work_queue *q)
{
	hash_table_clear(q->worker_blacklist);
}

static void print_password_warning( struct work_queue *q )
{
	static int did_password_warning = 0;

	if(did_password_warning) return;

       	if(!q->password && q->name) {
       		fprintf(stderr,"warning: this work queue master is visible to the public.\n");
	       	fprintf(stderr,"warning: you should set a password with the --password option.\n");
		did_password_warning = 1;
	}
}

struct work_queue_task *work_queue_wait(struct work_queue *q, int timeout)
{
	return work_queue_wait_internal(q, timeout, NULL, NULL);
}

static int wait_loop_poll_links(struct work_queue *q, int stoptime, struct link *foreman_uplink, int *foreman_uplink_active, int last_tasks_transfered)
{
	int n = build_poll_table(q, foreman_uplink);

	static int busy_waiting = 0;

	// Wait no longer than the caller's patience.
	int msec;
	if(stoptime) {
		msec = MAX(0, (stoptime - time(0)) * 1000);
	} else {
		msec = 5000;
	}

	// If workers are available and tasks waiting to be dispatched, don't wait
	// on a message. However, take care of not busy waiting, if no available
	// worker can execute a task in the ready list.
	
	if((last_tasks_transfered || !busy_waiting) && available_workers(q) > 0 && list_size(q->ready_list) > 0) {
		msec = 0;
		busy_waiting = 1;        //Mark that we may be busy waiting, so that if no task are transfered, we force a wait next cycle.
	}
	else {
		busy_waiting = 0;
	}

	// Poll all links for activity.
	timestamp_t link_poll_start = timestamp_get();
	int result = link_poll(q->poll_table, n, msec);
	q->link_poll_end = timestamp_get();
	q->total_idle_time += q->link_poll_end - link_poll_start;


	// If the master link was awake, then accept as many workers as possible.
	if(q->poll_table[0].revents) {
		do {
			add_worker(q);
		} while(link_usleep(q->master_link, 0, 1, 0) && (stoptime > time(0)));
	}

	int i, j = 1;

	// Consider the foreman_uplink passed into the function and disregard if inactive.
	if(foreman_uplink) {
		if(q->poll_table[1].revents) {
			*foreman_uplink_active = 1; //signal that the master link saw activity
		} else {
			*foreman_uplink_active = 0;
		}
		j++;
	}

	// Then consider all existing active workers
	for(i = j; i < n; i++) {
		if(q->poll_table[i].revents) {
			handle_worker(q, q->poll_table[i].link);
		}
	}

	if(hash_table_size(q->workers_with_available_results) > 0) {
		char *key;
		struct work_queue_worker *w;
		hash_table_firstkey(q->workers_with_available_results);
		while(hash_table_nextkey(q->workers_with_available_results,&key,(void**)&w)) {
			process_available_results(q, w, -1);
			hash_table_remove(q->workers_with_available_results, key);
		}	
	}

	return result;
}

static int wait_loop_transfer_tasks(struct work_queue *q, time_t stoptime)
{
	int task_started;
	int tasks_received;

	do 
	{
		//Compute task_transfer_stoptime in some way...
		time_t task_transfer_stoptime = stoptime;

		//IF SOMETHING THEN
		task_started = start_tasks(q, task_transfer_stoptime);

		//IF SOMETHING THEN
		tasks_received = receive_tasks(q, task_transfer_stoptime);

		//ELSE (break here to mimic old wq behaviour. To modify with a better policy)
		break;

	}while ( (time(0) < stoptime) && (task_started > 0 || tasks_received > 0));

	return task_started + tasks_received;
}

struct work_queue_task *work_queue_wait_internal(struct work_queue *q, int timeout, struct link *foreman_uplink, int *foreman_uplink_active)
/*
      --------------------
     |  compute stoptime  |
      --------------------
               |
               v
         --------------
+------>|  poll links  |
|        --------------
|              |
|              v
|        -------------
|       |  send task  |<----------------+
|        -------------                  |
|              |                    yes |
|              v                        |
|     ------------------  yes   -----------------
|    | tasks remaining? |----->| time remaining? |
|     ------------------        -----------------
|           no |                     no |
|              v                        |
|     ------------------                |           
|    |   receive task   |<--------------+           
|     ------------------                |           
|              |                    yes |           
|              v                        |           
|     ------------------  yes   -----------------   
|    | tasks remaining? |----->| time remaining? |  
|     ------------------        -----------------   
|           no |                    no |            
|              v                       |
|     ------------------               |
|    |fast abort workers|              |
|     ------------------               |
|              |                       |
|              v                       |
| yes  -----------------               |
+-----| time remaining? |              |
       -----------------               |
            no |                       |
               |-----------------------+
               v
           ----------
          |  return  |
           ----------
*/
{
	struct work_queue_task *t;
	time_t stoptime;
	int    tasks_transfered = 0;

	static timestamp_t last_left_time = 0;
	if(last_left_time!=0) {
		q->total_app_time += timestamp_get() - last_left_time;
	}

	print_password_warning(q);

	if(timeout == WORK_QUEUE_WAITFORTASK) {
		stoptime = 0;
	} else {
		stoptime = time(0) + timeout;
	}

	while(1) {
		if(q->name) {
			update_catalog(q, foreman_uplink, 0);
		}
		
		remove_unresponsive_workers(q);	

		t = list_pop_head(q->complete_list);
		if(t) {
			last_left_time = timestamp_get();

			if( t->result != SUCCESS )
			{
				q->total_tasks_failed++;
			}
			return t;
		}
		
		if( q->process_pending_check && process_pending() )
			break;

		if(itable_size(q->running_tasks) == 0 && list_size(q->ready_list) == 0 && !(foreman_uplink))
			break;

		wait_loop_poll_links(q, stoptime, foreman_uplink, foreman_uplink_active, tasks_transfered);

		//We have the resources we have been waiting for; start task transfers
		int known = known_workers(q);
		if(known > 0 && known >= q->workers_to_wait) {
			tasks_transfered = wait_loop_transfer_tasks(q, stoptime);
			q->workers_to_wait = 0; //disable it after we started dipatching tasks
		}

		// If fast abort is enabled, kill off slow workers.
		if(q->fast_abort_multiplier > 0) {
			abort_slow_workers(q);
		}
		
		// If the foreman_uplink is active then break so the caller can handle it.
		if(foreman_uplink) {
			break;
		}
		
		// If nothing was awake, restart the loop or return without a task.
		if(stoptime && time(0) >= stoptime) {
			break;
		} else {
			continue;
		}
	}

	last_left_time = timestamp_get();

	return 0;
}

int work_queue_hungry(struct work_queue *q)
{
	if(q->total_tasks_submitted < 100)
		return (100 - q->total_tasks_submitted);

	//BUG: fix this so that it actually looks at the number of cores available.

	int i, j;

	//i = 1.1 * number of current workers
	//j = # of queued tasks.
	//i-j = # of tasks to queue to re-reach the status quo.
	i = (1.1 * hash_table_size(q->worker_table)); 
	j = list_size(q->ready_list);
	return MAX(i - j, 0);
}

int work_queue_shut_down_workers(struct work_queue *q, int n)
{
	struct work_queue_worker *w;
	char *key;
	int i = 0;

	if(!q)
		return -1;

	// send worker the "exit" msg
	hash_table_firstkey(q->worker_table);
	while(i < n && hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if(itable_size(w->current_tasks) == 0) {
			shut_down_worker(q, w);
			i++;
		}
	}

	return i;
}

/**
 * Cancel submitted task as long as it has not been retrieved through wait().
 * This is non-blocking and has a worst-case running time of O(n) where n is 
 * number of submitted tasks.
 * This returns the work_queue_task struct corresponding to specified task and 
 * null if the task is not found.
 */
struct work_queue_task *work_queue_cancel_by_taskid(struct work_queue *q, int taskid) {

	struct work_queue_task *matched_task;

	if (taskid > 0){
		//see if task is executing at a worker (in running_tasks or finished_tasks).
		if ((matched_task = find_running_task_by_id(q, taskid))) {
			if (cancel_running_task(q, matched_task)) {
				q->total_tasks_cancelled++;	
				return matched_task;
			}	
		} //if not, see if task is in ready list.
		else if ((matched_task = list_find(q->ready_list, taskid_comparator, &taskid))) {
			list_remove(q->ready_list, matched_task);
			debug(D_WQ, "Task with id %d is removed from ready list.", matched_task->taskid);
			q->total_tasks_cancelled++;	
			return matched_task;
		} //if not, see if task is in complete list.
		else if ((matched_task = list_find(q->complete_list, taskid_comparator, &taskid))) {
			list_remove(q->complete_list, matched_task);
			debug(D_WQ, "Task with id %d is removed from complete list.", matched_task->taskid);
			q->total_tasks_cancelled++;	
			return matched_task;
		} 
		else { 
			debug(D_WQ, "Task with id %d is not found in queue.", taskid);
		}	
	}
	
	return NULL;
}

struct work_queue_task *work_queue_cancel_by_tasktag(struct work_queue *q, const char* tasktag) {

	struct work_queue_task *matched_task;

	if (tasktag){
		//see if task is executing at a worker (in running_tasks or finished_tasks).
		if ((matched_task = find_running_task_by_tag(q, tasktag))) {
			if (cancel_running_task(q, matched_task)) {
				q->total_tasks_cancelled++;	
				return matched_task;
			}
		} //if not, see if task is in ready list.
		else if ((matched_task = list_find(q->ready_list, tasktag_comparator, tasktag))) {
			list_remove(q->ready_list, matched_task);
			debug(D_WQ, "Task with tag %s and id %d is removed from ready list.", matched_task->tag, matched_task->taskid);
			q->total_tasks_cancelled++;	
			return matched_task;
		} //if not, see if task is in complete list.
		else if ((matched_task = list_find(q->complete_list, tasktag_comparator, tasktag))) {
			list_remove(q->complete_list, matched_task);
			debug(D_WQ, "Task with tag %s and id %d is removed from complete list.", matched_task->tag, matched_task->taskid);
			q->total_tasks_cancelled++;	
			return matched_task;
		} 
		else { 
			debug(D_WQ, "Task with tag %s is not found in queue.", tasktag);
		}
	}
	
	return NULL;
}

struct list * work_queue_cancel_all_tasks(struct work_queue *q) {
	struct list *l = list_create();
	struct work_queue_task *t;
	struct work_queue_worker *w;
	uint64_t taskid;
	char *key;
	
	while( (t = list_pop_head(q->ready_list)) ) {
		list_push_tail(l, t);
		q->total_tasks_cancelled++;	
	}
	while( (t = list_pop_head(q->complete_list)) ) {
		list_push_tail(l, t);
		q->total_tasks_cancelled++;	
	}


	hash_table_firstkey(q->workers_with_available_results);
	while(hash_table_nextkey(q->workers_with_available_results, &key, (void **) &w)) {
		hash_table_remove(q->workers_with_available_results, key);
		hash_table_firstkey(q->workers_with_available_results);
	}	

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w)) {
		
		send_worker_msg(q,w,"kill -1\n");
		
		itable_firstkey(w->current_tasks);
		while(itable_nextkey(w->current_tasks, &taskid, (void**)&t)) {
			itable_remove(q->running_tasks, taskid);
			itable_remove(q->finished_tasks, taskid);
			itable_remove(q->worker_task_map, taskid);
			
			//Delete any input files that are not to be cached.
			delete_worker_files(q, w, t, t->input_files, WORK_QUEUE_CACHE | WORK_QUEUE_PREEXIST);

			//Delete all output files since they are not needed as the task was aborted.
			delete_worker_files(q, w, t, t->output_files, 0);
			
			w->cores_allocated -= t->cores;
			w->memory_allocated -= t->memory;
			w->disk_allocated -= t->disk;
			w->gpus_allocated -= t->gpus;
			
			itable_remove(w->current_tasks, taskid);
			
			list_push_tail(l, t);
			q->total_tasks_cancelled++;	
		}
	}
	return l;
}

void release_all_workers(struct work_queue *q) {
	struct work_queue_worker *w;
	char *key;
	
	if(!q) return;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table,&key,(void**)&w)) {
		release_worker(q, w);
	}
}

void work_queue_reset(struct work_queue *q, int flags) {
	struct work_queue_task *t;
	
	if(!q) return;

	release_all_workers(q); 

	if(flags == WORK_QUEUE_RESET_KEEP_TASKS) {
		return;
	}

	//CLEANUP: Why would the user need to set KEEP_TASKS flag? 
	
	//WORK_QUEUE_RESET_ALL (or any other flag value) will clear out all tasks.
	//tasks in running, finished, complete lists are deleted in release_worker().
	while((t = list_pop_head(q->ready_list))) {
		work_queue_task_delete(t);
	}
}

int work_queue_empty(struct work_queue *q)
{
	return ((list_size(q->ready_list) + itable_size(q->running_tasks) + itable_size(q->finished_tasks) + list_size(q->complete_list)) == 0);
}

void work_queue_specify_keepalive_interval(struct work_queue *q, int interval) 
{
	q->keepalive_interval = interval;
}

void work_queue_specify_keepalive_timeout(struct work_queue *q, int timeout) 
{
	q->keepalive_timeout = timeout;
}

int work_queue_tune(struct work_queue *q, const char *name, double value)
{
	
	if(!strcmp(name, "asynchrony-multiplier")) {
		q->asynchrony_multiplier = MAX(value, 1.0);
		
	} else if(!strcmp(name, "asynchrony-modifier")) {
		q->asynchrony_modifier = MAX(value, 0);
		
	} else if(!strcmp(name, "min-transfer-timeout")) {
		q->minimum_transfer_timeout = (int)value;
	
	} else if(!strcmp(name, "foreman-transfer-timeout")) {
		q->foreman_transfer_timeout = (int)value;
		
	} else if(!strcmp(name, "default-transfer-rate")) {
		q->default_transfer_rate = value;

	} else if(!strcmp(name, "transfer-outlier-factor")) {
		q->transfer_outlier_factor = value;
		
	} else if(!strcmp(name, "fast-abort-multiplier")) {
		work_queue_activate_fast_abort(q, value);

	} else if(!strcmp(name, "keepalive-interval")) {
		q->keepalive_interval = MAX(0, (int)value);
		
	} else if(!strcmp(name, "keepalive-timeout")) {
		q->keepalive_timeout = MAX(0, (int)value);

	} else if(!strcmp(name, "short-timeout")) {
		q->short_timeout = MAX(1, (int)value);
		
	} else {
		debug(D_NOTICE|D_WQ, "Warning: tuning parameter \"%s\" not recognized\n", name);
		return -1;
	}
	
	return 0;
}

void work_queue_enable_process_module(struct work_queue *q)
{
	q->process_pending_check = 1;
}

char * work_queue_get_worker_summary( struct work_queue *q )
{
	return strdup("n/a");
}

void work_queue_set_bandwidth_limit(struct work_queue *q, const char *bandwidth)
{
	q->bandwidth = string_metric_parse(bandwidth);
}

double work_queue_get_effective_bandwidth(struct work_queue *q)
{
	double queue_bandwidth = get_queue_transfer_rate(q, NULL)/MEGABYTE; //return in MB per second
	return queue_bandwidth; 
}

void work_queue_get_stats(struct work_queue *q, struct work_queue_stats *s)
{
	memset(s, 0, sizeof(*s));

	//info about workers 
	s->total_workers_connected = hash_table_size(q->worker_table);
	s->workers_init = hash_table_size(q->worker_table) - known_workers(q);
	s->workers_idle = known_workers(q) - workers_with_tasks(q); //returns workers that are not running any tasks.
	s->workers_busy = workers_with_tasks(q); 
	s->total_workers_joined = q->total_workers_joined;
	s->total_workers_removed = q->total_workers_removed;

	//info about tasks
	s->tasks_waiting = list_size(q->ready_list);
	s->tasks_running = itable_size(q->running_tasks) + itable_size(q->finished_tasks);
	s->tasks_complete = list_size(q->complete_list);
	s->total_tasks_dispatched = q->total_tasks_submitted;
	s->total_tasks_complete = q->total_tasks_complete;
	s->total_tasks_cancelled = q->total_tasks_cancelled;
	
	//info about queue
	s->start_time = q->start_time;
	s->total_send_time = q->total_send_time;
	s->total_receive_time = q->total_receive_time;
	s->total_bytes_sent = q->total_bytes_sent;
	s->total_bytes_received = q->total_bytes_received;
	s->total_execute_time = q->total_execute_time;
	s->total_good_execute_time = q->total_good_execute_time;
	timestamp_t wall_clock_time = timestamp_get() - q->start_time;
	if(wall_clock_time>0 && s->total_workers_connected>0) {
		s->efficiency = (double) (q->total_execute_time) / (wall_clock_time * s->total_workers_connected);
	}
	if(wall_clock_time>0) {
		s->idle_percentage = (double) q->total_idle_time / wall_clock_time;
	}
	s->capacity = compute_capacity(q);

	//info about resources
	s->bandwidth = work_queue_get_effective_bandwidth(q); 
	struct work_queue_resources r;
	aggregate_workers_resources(q,&r);
	s->total_cores = r.cores.total;
	s->total_memory = r.memory.total;
	s->total_disk = r.disk.total;
	s->total_gpus = r.gpus.total;
	s->min_cores = r.cores.smallest;
	s->max_cores = r.cores.largest;
	s->min_memory = r.memory.smallest;
	s->max_memory = r.memory.largest;
	s->min_disk = r.disk.smallest;
	s->max_disk = r.disk.largest;
	s->min_gpus = r.gpus.smallest;
	s->max_gpus = r.gpus.largest;

	//deprecated fields
	s->port = q->port;
	s->priority = q->priority;
	s->workers_ready = s->workers_idle; 
	s->workers_full = 0;
	s->total_worker_slots = s->tasks_running; 
	s->avg_capacity = s->capacity;
}

/* Unlike aggregate_workers_resources below, does not reset total */
void aggregate_committed_in_queue( struct work_queue *q, struct work_queue_resources *total )
{
	struct work_queue_task *t;

	list_first_item(q->ready_list);
	while((t = list_next_item(q->ready_list)))
	{
		if(t->unlabeled)
		{
			total->unlabeled.committed++;
		}
		else
		{
			total->cores.committed  += MAX(t->cores, 0);
			total->memory.committed += MAX(t->memory,0);
			total->disk.committed   += MAX(t->disk,  0);
			total->gpus.committed   += MAX(t->gpus,  0);
		}
	}
	list_first_item(q->ready_list);
}

/*
This function is a little roundabout, because work_queue_resources_add
updates the min and max of each value as it goes.  So, we set total
to the value of the first item, then use work_queue_resources_add.
If there are no items, we must manually return zero.
*/

void aggregate_workers_resources( struct work_queue *q, struct work_queue_resources *total )
{
	struct work_queue_worker *w;
	char *key;
	int first = 1;

	if(hash_table_size(q->worker_table)==0) {
		memset(total,0,sizeof(*total));
		return;
	}

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table,&key,(void**)&w)) {
		if(first) {
			*total = *w->resources;
			first = 0;
		} else {
			work_queue_resources_add(total,w->resources);
		}
	}

	aggregate_committed_in_queue(q, total);
}

int work_queue_specify_log(struct work_queue *q, const char *logfile)
{
	q->logfile = fopen(logfile, "a");
	if(q->logfile) {
		setvbuf(q->logfile, NULL, _IOLBF, 1024); // line buffered, we don't want incomplete lines
		fprintf(q->logfile,
				// start with a comment
				"#"
			// time:	
			"timestamp "
			//workers:
			"total_workers_connected workers_init workers_idle workers_busy total_workers_joined total_workers_removed "
			// tasks:
			"tasks_waiting tasks_running tasks_complete total_tasks_dispatched total_tasks_complete total_tasks_cancelled "
			// queue:
			"start_time total_send_time total_receive_time total_bytes_sent total_bytes_received efficiency idle_percentage capacity "
			// resource totals:
			"bandwidth total_cores total_memory total_disk total_gpus "
			//mins/maxs:
			"min_cores max_cores min_memory max_memory min_disk max_disk min_gpus max_gpus "
			//execute/good execute time
			"total_execute_time total_good_execute_time "
			//end with a newline
			"\n"
			);
		log_worker_stats(q);
		debug(D_WQ, "log enabled and is being written to %s\n", logfile);
		return 1;
	}
	else
	{
		debug(D_NOTICE | D_WQ, "couldn't open logfile %s: %s\n", logfile, strerror(errno)); 
		return 0;
	}
}



/* vim: set noexpandtab tabstop=4: */
