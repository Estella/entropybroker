#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "error.h"
#include "pool.h"
#include "fips140.h"
#include "scc.h"
#include "config.h"
#include "handle_client.h"
#include "utils.h"
#include "log.h"
#include "handle_pool.h"
#include "signals.h"
#include "auth.h"

const char *pid_file = PID_DIR "/eb.pid";

void dump_pools(pool **pools, int n_pools)
{
	FILE *fh = fopen(CACHE, "wb");
	if (!fh)
		error_exit("Failed to create %s", CACHE);

	for(int index=0; index<n_pools; index++)
		pools[index] -> dump(fh);

	fclose(fh);
}

void seed(pool **pools, int n_pools)
{
	int n = 0, dummy;

	n += add_event(pools, n_pools, get_ts_ns(), NULL, 0); 

	dummy = getpid();
	n += add_event(pools, n_pools, get_ts_ns(), (unsigned char *)&dummy, sizeof(dummy));

	unsigned char buffer[16];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1)
		dolog(LOG_DEBUG, "Cannot open /dev/urandom");
	else
	{
		READ(fd, (char *)buffer, sizeof buffer);
		close(fd);

		n += add_event(pools, n_pools, get_ts_ns(), buffer, sizeof(buffer));
	}

	dolog(LOG_DEBUG, "added %d bits of startup-event-entropy to pool", n);

	unsigned char *buffer2 = NULL;
	long int dummy_2;
	fips140 *pf = new fips140();
	scc *ps = new scc();
	get_bits_from_pools(sizeof(dummy_2) * 8, pools, n_pools, &buffer2, 1, 1, pf, 1, ps);
	delete ps;
	delete pf;
	memcpy(&dummy_2, buffer2, sizeof dummy_2);
	srand48(dummy_2);
	free(buffer2);
}

void help(void)
{
	printf("-c file   config-file to read (default: " CONFIG "\n");
        printf("-l file   log to file 'file'\n");
        printf("-s        log to syslog\n");
	printf("-S        statistics-file to log to\n");
        printf("-n        do not fork\n");
	printf("-P file   write pid to file\n");
}

int main(int argc, char *argv[])
{
	pool **pools;
	int n_pools = 0;
	int c;
	char do_not_fork = 0, log_console = 0, log_syslog = 0;
	char *log_logfile = NULL;
	char *stats_file = NULL;
	fips140 *eb_output_fips140 = new fips140();
	scc *eb_output_scc = new scc();
	const char *config_file = CONFIG;
	config_t config;

	memset(&config, 0x00, sizeof(config));

	printf("eb v " VERSION ", (C) 2009-2012 by folkert@vanheusden.com\n");

	eb_output_fips140 -> set_user((char *)"output");
	eb_output_scc     -> set_user((char *)"output");

	while((c = getopt(argc, argv, "P:c:S:l:sn")) != -1)
	{
		switch(c)
		{
			case 'P':
				pid_file = optarg;
				break;

			case 'c':
				config_file = optarg;
				break;

			case 'S':
				stats_file = optarg;
				break;

			case 's':
				log_syslog = 1;
				break;

			case 'l':
				log_logfile = optarg;
				break;

			case 'n':
				do_not_fork = 1;
				log_console = 1;
				break;

			default:
				help();
				return 1;
		}
	}

	set_logging_parameters(log_console, log_logfile, log_syslog);

	load_config(config_file, &config);
	if (stats_file)
		config.stats_file = stats_file;

	if (config.auth_password == NULL)
	{
		fprintf(stderr, "No password set in configurationfile\n");
		return 1;
	}

	eb_output_scc -> set_threshold(config.scc_threshold);

	n_pools = config.number_of_pools;

	pools = (pool **)malloc(sizeof(pool *) * n_pools);
	FILE *fh = fopen(CACHE, "rb");
	if (!fh)
	{
		fprintf(stderr, "No cache-file found, continuing...\n");

		for(int loop=0; loop<n_pools; loop++)
			pools[loop] = new pool();
	}
	else
	{
		for(int loop=0; loop<n_pools; loop++)
			pools[loop] = new pool(loop + 1, fh);

		fclose(fh);
	}

	if (!do_not_fork)
	{
		if (daemon(-1, -1) == -1)
			error_exit("fork failed");
	}

	write_pid(pid_file);

	set_signal_handlers();

	seed(pools, n_pools);

	main_loop(pools, n_pools, &config, eb_output_fips140, eb_output_scc);

	dump_pools(pools, n_pools);

	unlink(pid_file);

	return 0;
}
