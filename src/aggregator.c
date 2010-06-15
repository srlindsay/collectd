

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

#include "buf.h"
#include "conn.h"

/* Folks without pthread will need to disable this plugin. */
#include <pthread.h>

#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <grp.h>

#include <event.h>
#include <glib.h>

#ifndef UNIX_PATH_MAX
# define UNIX_PATH_MAX sizeof (((struct sockaddr_un *)0)->sun_path)
#endif

#define DEFAULT_SOCK_FILE "/var/run/collectd-aggregator"
#define MAX_VALS 64

static void read_cb (conn_t *conn, void *data);

static pthread_t server_thread = (pthread_t ) 0;

static int     default_interval = 60;
static int     sock_perms = S_IRWXU | S_IRWXG | S_IRWXO;

static const char *config_keys[] = {
	"SocketFile",
	"SocketFileAvg",
	"DefaultInterval"
};

static int config_keys_num = STATIC_ARRAY_SIZE (config_keys);

typedef enum {
	AG_TYPE_SUM,
	AG_TYPE_AVG,
} ag_type_t;

typedef struct ag_st {
	ag_type_t type;
	struct event ev;
	GHashTable *keys; /* keys -> ag_key_entry_t */
} ag_t;

#define MAX_KEY_SIZE 1024

typedef struct ag_key_entry_st {
	ag_t *ag;
	char key[MAX_KEY_SIZE];
	struct timeval interval;
	double total;
	long count;

	value_list_t vl;
	struct event flush_ev;
} ag_key_entry_t;

typedef struct ag_client_st {
	ag_t *ag;
	struct event read_ev;
	int used;
	int size;
	char *pending;
} ag_client_t;

static void flush_cb (int fd, short ev_type, void *data) {
	ag_key_entry_t *entry = data;

	value_t values[1];

	switch (entry->ag->type) {
		case AG_TYPE_SUM:
			values[0].gauge = entry->total;
			break;
		case AG_TYPE_AVG:
			values[0].gauge = entry->count ? 
				(entry->total / (double) entry->count) : 0.0;
			break;
	}

	DEBUG("flush_cb called for key %s, val: %g", entry->key, values[0].gauge);

	entry->vl.values = values;
	entry->vl.values_len = 1;
	entry->vl.time = time(NULL);

	plugin_dispatch_values(&entry->vl);

	entry->total = 0.0;
	entry->count = 0;
	evtimer_add(&entry->flush_ev, &entry->interval);
}

static ag_key_entry_t *ag_key_entry_new(ag_t *ag, const char *key) {
	ag_key_entry_t *entry;

	entry = malloc(sizeof(ag_key_entry_t));
	entry->ag = ag;
	strncpy(entry->key, key, sizeof(entry->key));
	entry->interval.tv_sec = default_interval;
	entry->interval.tv_usec = 0;
	entry->total = 0.0;

	memset (&entry->vl, 0, sizeof(value_list_t));

	entry->vl.interval = entry->interval.tv_sec;
	entry->vl.meta = NULL;

	value_list_t *vl = &entry->vl;

	sstrncpy(vl->host, hostname_g, sizeof(vl->host));
	sstrncpy(vl->type, "gauge", sizeof(vl->type));

	sscanf(entry->key, "%[^-]-%[^-]-%s", vl->plugin, vl->plugin_instance, vl->type_instance);

	DEBUG("host: %s, plugin: %s, plugin-instance:%s, "
			"type: %s, type-instance:%s", 
			vl->host, vl->plugin, vl->plugin_instance, 
			vl->type, vl->type_instance);

	return entry;
}

static void build_key(char *buffer, int buf_sz, const char *plugin,
		const char *plugin_inst, const char *type_inst) {
	snprintf (buffer, buf_sz, "%s-%s-%s", plugin, plugin_inst, type_inst);
}

static void handle_vals(ag_t *ag, const char *plugin, const char *plugin_inst,
		const char *type_inst, double *vals, int n_vals) {
	char key[1024];
	build_key (key, 1024, plugin, plugin_inst, type_inst);
	ag_key_entry_t *entry;
	entry = g_hash_table_lookup(ag->keys, key);
	if (!entry) {
		DEBUG("creating entry for %s", key);
		entry = ag_key_entry_new(ag, key);
		g_hash_table_insert(ag->keys, entry->key, entry);
		evtimer_set(&entry->flush_ev, flush_cb, entry);
		evtimer_add(&entry->flush_ev, &entry->interval); 
	}
	DEBUG("handle_vals called");
	DEBUG("key: %s", key);
	int n;
	for (n=0; n < n_vals; n++) {
		DEBUG("val: %g", vals[n]);
		switch(ag->type) {
			case AG_TYPE_SUM:
				entry->total += vals[n];
				break;
			case AG_TYPE_AVG:
				entry->total += vals[n];
				entry->count++;
				break;
		}
	}
	DEBUG("total: %g", entry->total);
}

typedef enum {
	AG_START = 0,
	AG_PLUGIN,
	AG_PLUGIN_INST,
	AG_TYPE_INST,
	AG_VALUES
} state_t;

#define MAX_STR_SIZE 256

#define PARSE_FIELD(start_offset, buffer, next_start, next_state) \
	if (*c == ' ') {                                       \
		chain_slice_t s;                                   \
		s.start = (start_offset);                          \
		s.end.b = b;                                       \
		s.end.loc = c;                                     \
		str_len = chain_memcpy ((buffer), s, MAX_STR_SIZE);          \
		(buffer)[str_len] = '\0';                          \
		printf ("parsed out this: %s\n", (buffer));        \
		DEBUG("parsed: %s", (buffer));                     \
		next_start = chain_next(b, c);                     \
		state = (next_state);                              \
	}

static char* parse(ag_t *ag, conn_t *conn) {
	char *c;
	char *parsed = NULL;

	chain_offset_t plugin_start = {0},
				   plugin_inst_start = {0},
				   type_inst_start = {0},
				   val_start = {0};

	int str_len;

	char plugin[MAX_STR_SIZE];
	char plugin_inst[MAX_STR_SIZE];
	char type_inst[MAX_STR_SIZE];

	double vals[MAX_VALS];
	int n_vals = 0;

	state_t state = AG_START;
	buf_t *b;

	for (b = conn->in; b; b=b->next) {
		for (c = b->first; c < b->last; c++) {
			switch (state) {
				case AG_START:
					plugin_start.b = b;
					plugin_start.loc = c;
					state = AG_PLUGIN;
					break;
				case AG_PLUGIN:
					PARSE_FIELD(plugin_start, plugin, plugin_inst_start, AG_PLUGIN_INST);
					break;
				case AG_PLUGIN_INST:
					PARSE_FIELD(plugin_inst_start, plugin_inst, type_inst_start, AG_TYPE_INST);
					break;
				case AG_TYPE_INST:
					PARSE_FIELD(type_inst_start, type_inst, val_start, AG_VALUES);
					break;
				case AG_VALUES:
					switch (*c) {
						case ':':
							if (n_vals < MAX_VALS) {
								if (val_start.b == b) {
									vals[n_vals++] = strtod(val_start.loc, NULL);
								} else {
									char valbuf[256];
									chain_slice_t s;
									s.start = val_start;
									s.end.b = b;
									s.end.loc = c;
									chain_memcpy(valbuf, s, 256);
									vals[n_vals++] = strtod(valbuf, NULL);
								}
							} else {
								WARNING("aggregator plugin: more than 64 values "
										"in this packet.  Dropping the extra.");
							}
							val_start.b = b;
							val_start.loc = c+1;
							break;
						case '\n':
							if (n_vals < MAX_VALS) {
								if (val_start.b == b) {
									vals[n_vals++] = strtod(val_start.loc, NULL);
								} else {
									char valbuf[256];
									chain_slice_t s;
									s.start = val_start;
									s.end.b = b;
									s.end.loc = c;
									chain_memcpy(valbuf, s, 256);
									vals[n_vals++] = strtod(valbuf, NULL);
								}
							} else {
								WARNING("aggregator plugin: more than 64 values "
										"in this packet.  Dropping the extra.");
							}
							/* read a complete line, process these values */
							DEBUG("finished a complete line");
							handle_vals(ag, plugin, plugin_inst, type_inst, vals, n_vals);

							chain_offset_t end = {b, c};
							conn->in = chain_free_offset(conn->in, end);

							/* reset everything */
							state = AG_START;
							parsed = c+1;
							n_vals = 0;
							break;
					}
					break;
			}
		}
	}
	return parsed;
}

static void read_cb (conn_t *conn, void *data) {
	DEBUG("in read_cb");
	ag_t *ag = conn->data;
	buf_print_chain(conn->in);
	parse(ag, conn);
	buf_print_chain(conn->in);
}

static void err_cb (conn_t *conn, short ev_type, void *data) {
	DEBUG("in error_cb");
}

static void accept_cb (int fd, short ev_type, void *data) {
	ag_t *ag = data;

	DEBUG("aggregator plugin: accept_cb");
	int newfd = accept(fd, NULL, NULL);

	conn_t *conn = conn_new();
	conn->fd = newfd;
	conn->data = ag;

	conn->read_handler = read_cb;
	conn->error_handler = err_cb;

	conn->want_read = 1;

	conn_register_events(conn);
}

static int setup_socket (const char *path) {
	struct sockaddr_un sa;
	int status;

	unlink(path);

	int sock_fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		char errbuf[1024];
		ERROR ("aggregator plugin: socket failed: %s",
				sstrerror (errno, errbuf, sizeof (errbuf)));
		return (-1);
	}

	memset (&sa, '\0', sizeof (sa));
	sa.sun_family = AF_UNIX;
	sstrncpy (sa.sun_path, (path != NULL) ? path : DEFAULT_SOCK_FILE,
			sizeof (sa.sun_path));
	/* unlink (sa.sun_path); */

	DEBUG ("aggregator plugin: socket path = %s", sa.sun_path);

	status = bind (sock_fd, (struct sockaddr *) &sa, sizeof (sa));
	if (status != 0)
	{
		char errbuf[1024];
		sstrerror (errno, errbuf, sizeof (errbuf));
		ERROR ("aggregator plugin: bind failed: %s", errbuf);
		close (sock_fd);
		sock_fd = -1;
		return (-1);
	}

	chmod (sa.sun_path, sock_perms);

	status = listen (sock_fd, 8);
	if (status != 0)
	{
		char errbuf[1024];
		ERROR ("aggregator plugin: listen failed: %s",
				sstrerror (errno, errbuf, sizeof (errbuf)));
		close (sock_fd);
		sock_fd = -1;
		return (-1);
	}

	return sock_fd;
}

typedef struct pathtypes_st {
	const char *socket_path;
	ag_type_t type;
}pathtypes_t;

static pathtypes_t paths[] = {
	{"/var/run/collectd-aggregator", AG_TYPE_SUM},
	{"/var/run/collectd-aggregator-avg", AG_TYPE_AVG},
	{NULL, 0}
};


static void *ag_server_thread (void __attribute__((unused)) *arg) {
	struct event_base* ev_base = event_init();
	if (!ev_base) {
		ERROR("aggregator plugin: event_init() failed");
		return NULL;
	}

	pathtypes_t *path;
	for (path=paths; path->socket_path; path++) {
		ag_t *ag = malloc(sizeof(ag_t));
		ag->keys = g_hash_table_new(g_str_hash, g_str_equal);
		ag->type = path->type;

		int fd = setup_socket(path->socket_path);

		event_set(&ag->ev, fd, EV_READ | EV_PERSIST, accept_cb, ag);

		if (event_add(&ag->ev, NULL) < 0) {
			ERROR("aggregator plugin: failed to add listen event");
			return NULL;
		}
	}

	int rc = event_loop(0);

	if (rc == -1) {
		ERROR("aggregator plugin: event_loop exited with an error");
	} else if (rc == 1) {
		ERROR("aggregator plugin: event_loop exited due to no events registered");
	} else {
		DEBUG("aggregator plugin: event_loop exited normally");
	}

	return ((void*) 0);
}

static int ag_config(const char *key, const char *val) {
	DEBUG("aggregator plugin: ag_config");
	if (strcasecmp (key, "SocketFile") == 0)
	{
		char *new_sock_file = strdup (val);
		if (new_sock_file == NULL)
			return (1);

		pathtypes_t *p;
		for (p=paths; p->socket_path; p++) {
			if (p->type == AG_TYPE_SUM) {
				p->socket_path = new_sock_file;
				break;
			}
		}
	}
	else if (strcasecmp (key, "SocketFileAvg") == 0)
	{
		char *new_sock_file = strdup (val);
		if (new_sock_file == NULL)
			return (1);

		pathtypes_t *p;
		for (p=paths; p->socket_path; p++) {
			if (p->type == AG_TYPE_AVG) {
				p->socket_path = new_sock_file;
				break;
			}
		}
	}
	else if (strcasecmp (key, "DefaultInterval") == 0)
	{
		default_interval = (int) strtol(val, NULL, 10);
	}
	else
	{
		return (-1);
	}

	return (0);
}

static int ag_init(void) {
	static int have_init = 0;

	int status;

	/* Initialize only once. */
	if (have_init != 0)
		return (0);
	have_init = 1;

	status = pthread_create (&server_thread, NULL, ag_server_thread, NULL);
	if (status != 0)
	{
		char errbuf[1024];
		ERROR ("aggregator plugin: pthread_create failed: %s",
				sstrerror (errno, errbuf, sizeof (errbuf)));
		return (-1);
	}
	return 0;
}

static int ag_shutdown(void) {
	pathtypes_t *path;
	for (path=paths; path->socket_path; path++) {
		unlink(path->socket_path);
	}
	return 0;
}

void module_register(void)
{
	plugin_register_config("aggregator", ag_config, config_keys, config_keys_num);
	plugin_register_init("aggregator", ag_init);
	plugin_register_shutdown("aggregator", ag_shutdown);
}
