#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>

#include "shared.h"
#include "st.h"

extern char *optarg;

char *default_vx32sdk_gcc_command[] = {
	"export LC_ALL=C; P=%s; gcc -m32 -Wall -O2 -g",
	" -Wl,-melf_i386,-Telf_i386_0x00048000.x",
	" -mno-tls-direct-seg-refs -fno-stack-protector",
	" -nostdlib -mfp-ret-in-387 $P/crt0.o -o %s %s",
	" -I$P/include -L$P -lst -lc -lgcc >%s 2>&1"};

struct storage_engines{
	char *name;
	storage_engine_create *create;
	storage_engine_destroy *destroy;
};

struct storage_engines engines[] = {
	{"fs", &storage_fs_create, &storage_fs_destroy},
	{"dumb", &storage_dumb_create, &storage_dumb_destroy},
	#ifdef CONFIG_USE_TOKYOCABINET
	{"tc", &storage_tc_create, &storage_tc_destroy},
	#endif
	#ifdef CONFIG_USE_BERKELEYDB
	{"bdb", &storage_bdb_create, &storage_bdb_destroy},
	#endif
	#ifdef CONFIG_USE_YDB
	{"ydb", &storage_ydb_create, &storage_ydb_destroy},
	#endif
};

char *flatten_argv(int argc, char **argv, char *joiner) {
	static char params[1024];
	params[0] = '\0';
	int i;
	for(i=0; i < argc; i++) {
		snprintf(params+strlen(params), sizeof(params)-strlen(params),
			"%s%s", argv[i], joiner);
	}
	if(strlen(params))
		params[strlen(params)-strlen(joiner)] = '\0';
	return(params);
}

char *flatten_argv_ext(int argc, char **argv, char *prefix, char *suffix) {
	static char params[1024];
	params[0] = '\0';
	int i;
	for(i=0; i < argc; i++) {
		snprintf(params+strlen(params), sizeof(params)-strlen(params),
			"%s%s%s", prefix, argv[i], suffix);
	}
	if(strlen(params))
		params[strlen(params)-strlen(suffix)] = '\0';
	return(&params[strlen(prefix)]);
}

char *flatten_engine_names() {
	char *names[NELEM(engines)];
	int i;
	for(i=0; i < NELEM(engines); i++)
		names[i] = engines[i].name;
	return flatten_argv(NELEM(engines), names, ", ");
}

void info_handler(void *arg) {
	struct config *config = (struct config*)arg;
	storage_sync(config->api);
	int pool_items, pool_bytes;
	get_pool_size(&pool_items, &pool_bytes);
	log_info("Memory stats: %iMB in %i items in pool. Pool freed.",
				pool_bytes/1024/1024, pool_items);
	pool_free();
}

void quit_handler(void *arg) {
	struct config *config = (struct config*)arg;
	log_info("Received signal, syncing database.");
	storage_sync(config->api);
	log_info("Synced.");
}

void print_help(struct server *server, struct config *config) {
printf(
"Usage: smalltable [OPTION]... -- [ENGINE PARAMETERS]...\n"
VERSION_STRING " - a light and fast key-value server\n"
"\n"
"Usefull options:\n"
"  -e, --engine=ENGINE       select storage engine for key-value data;\n"
"                            must be one of: [%s]\n"
"  -l, --listen=HOST         bind to specified hostname (default=%s)\n"
"  -p, --port=PORT           tcp/ip port number to listen on (default=%i)\n"
"\n"
"Useless options:\n"
"  -v, --verbose             print more useless debugging messages\n"
"  -x, --ping-parent         send SIGURG to parent pid after successfull binding\n"
"  -n, --no-vx32             disable vx32 engine\n"
"  -s, --vx32sdk=PATH        vx32sdk directory that contains includes and libs\n"
"                            (default=%s)\n"
"  -t, --tmpdir=PATH         temporary directory path; it's used as a storage\n"
"                            when foreign code is compiled (default=%s)\n"
"  -g, --gcc-command=CMD     gcc command line used to compile foreign vx32 code\n"
"                            default=%s"
"\n"
"Engine specific parameters:\n"
"   dumb:  --engine=dumb\n"
"   fs:    --engine=fs directory\n"
#ifdef CONFIG_USE_TOKYOCABINET
"   tc:    --engine=ydb tcfile\n"
#endif
#ifdef CONFIG_USE_BERKELEYDB
"   bdb:   --engine=bdb bdbfile\n"
#endif 
#ifdef CONFIG_USE_YDB
"   ydb:   --engine=ydb directory\n"
#endif
"\n"
""
"",
	flatten_engine_names(),
	server->host,
	server->port,
	config->vx32sdk_path,
	config->tmpdir,
	flatten_argv_ext(NELEM(default_vx32sdk_gcc_command), default_vx32sdk_gcc_command, "\t\t\t\t", "\t\\\n")
);
}

int main(int argc, char *argv[]) {
	static struct option long_options[] = {
		{"listen", required_argument, 0, 'l'},
		{"port", required_argument, 0, 'p'},
		{"help", no_argument, 0, 'h'},
		{"verbose", no_argument, 0, 'v'},
		{"ping-parent", no_argument, 0, 'x'},
		{"gcc-command", required_argument, 0, 'g'},
		{"vx32sdk", required_argument, 0, 's'},
		{"tmpdir", required_argument, 0, 't'},
		{"engine", required_argument, 0, 'e'},
		{"no-vx32", no_argument, 0, 'n'},
		{0, 0, 0, 0}
	};
	
	struct server *server = (struct server*)st_calloc(1, sizeof(struct server));
	server->info_handler = &info_handler;
	server->quit_handler = &quit_handler;
	server->process_multi= &process_multi;
	INIT_LIST_HEAD(&server->root);
	
	server->host = "127.0.0.1";
	server->port = 22122;
	
	struct config *config = (struct config*)st_calloc(1, sizeof(struct config));
	server->userdata = config;
	config->tmpdir = "/tmp";
	config->vx32sdk_path = "./untrusted/";
	config->vx32sdk_gcc_command = strdup(flatten_argv(NELEM(default_vx32sdk_gcc_command), default_vx32sdk_gcc_command, " "));
	config->syscall_limit = 4; /* 4 syscalls per request allowed */
	
	int option_index;
	int arg;
	char *engine_name = NULL;
	while((arg = getopt_long_only(argc, argv, "hxvnl:p:g:s:t:e:", long_options, &option_index)) != EOF) {
		switch(arg) {
		case 'h':
			print_help(server, config);
			exit(-1);
			break;
		case 'v':
			server->trace = 1;
			break;
		case 'x':
			server->ping_parent = 1;
			break;
		case 'l':
			server->host = optarg;
			break;
		case 'p':
			server->port = atoi(optarg);
			if(server->port < 0 || server->port > 65536)
				fatal("Port number broken: %i", server->port);
			break;
		case 'g':
			free(config->vx32sdk_gcc_command);
			config->vx32sdk_gcc_command = strdup(optarg);
			break;
		case 's':
			config->vx32sdk_path = optarg;
			break;
		case 't':
			config->tmpdir = optarg;
			break;
		case 'e':
			engine_name = optarg;
			break;
		case 'n':
			config->vx32_disabled = 1;
			break;
		case 0:
		default:
			fatal("\nUnknown option: \"%s\"\n", argv[optind-1]);
		}
	}
	
	int i;
	storage_engine_create *engine_create = NULL;
	storage_engine_destroy *engine_destroy = NULL;
	for(i=0; i<NELEM(engines); i++) {
		if(engine_name && 0 == strcmp(engine_name, engines[i].name)) {
			engine_create = engines[i].create;
			engine_destroy = engines[i].destroy;
		}
	}
	if(NULL == engine_create)
		fatal("\nYou must specify a storage engine:"
		" --engine=[%s]\n", flatten_engine_names() );

	log_info("Process pid %i", getpid());
	signal(SIGPIPE, SIG_IGN);
	
	commands_initialize();
	process_initialize(config);
	char *params = flatten_argv(argc-optind, &argv[optind], ", ");
	log_info("Loading database engine \"%s\" with parameters \"%s\"", engine_name, params);
	
	config->api = engine_create(argc-optind, &argv[optind]);
	
	do_event_loop(server);
	
	log_info("Quit");
	process_destroy();
	commands_destroy();
	pool_free();
	
	engine_destroy(config->api);
	free(config->vx32sdk_gcc_command);
	free(config);
	free(server);
	
	exit(0);
	return(0);
}
