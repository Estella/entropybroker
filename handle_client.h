typedef struct
{
	int to_thread[2], to_main[2];
	pthread_t th;

	int socket_fd;
	char host[128], type[128];
	bool is_server;
	int bits_sent, bits_recv;
	int max_bits_per_interval;
	bool allow_prng;
	bool ignore_rngtest_fips140, ignore_rngtest_scc;
	double last_message, last_put_message;
	double connected_since;
	char *password;

	fips140 *pfips140;
	scc *pscc;

	unsigned char ivec[8]; // used for data encryption
	int ivec_offset;
	long long unsigned int challenge;
	long long unsigned int ivec_counter; // required for CFB

	BF_KEY key;

	int ping_nr;

	// globals
	users *pu;
	config_t *config;
	pools *ppools;
} client_t;

typedef struct
{
	int bps, bps_cur;

	long long int total_recv, total_sent;
	int total_recv_requests, total_sent_requests;
	int n_times_empty, n_times_not_allowed, n_times_full, n_times_quota;

	int disconnects, timeouts;
} statistics_t;

void main_loop(pools *ppools, config_t *config, fips140 *eb_output_fips140, scc *eb_output_scc);
