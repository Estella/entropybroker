#include <vector>
#include <string>
#include <map>
#include <sys/time.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <libgen.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <openssl/blowfish.h>
#include <openssl/sha.h>

#include "error.h"
#include "utils.h"
#include "log.h"
#include "math.h"
#include "protocol.h"
#include "users.h"
#include "auth.h"
#include "kernel_prng_io.h"

#define KNUTH_SIZE 8192
#define A_B_SIZE 8192
#define KNUTH_FILE "lookup.knuthm"

const char *pid_file = PID_DIR "/proxy_knuth_m.pid";
const char *client_type = "proxy_knuth_m v" VERSION;

volatile bool sig_quit = false;

typedef struct
{
	int fd;
	std::string host, type;

        unsigned char ivec[8]; // used for data encryption
        int ivec_offset;
        long long unsigned int challenge;
        long long unsigned int ivec_counter; // required for CFB

        BF_KEY key;
} proxy_client_t;

typedef struct
{
	unsigned short *table;
	int t_size, t_offset;

	unsigned short *A;
	int n_A;
	unsigned short *B;
	int n_B;

	pthread_mutex_t lock;
} lookup_t;

int read_value(FILE *fh)
{
	unsigned char buffer[4];

	if (fread(buffer, 1, 4, fh) != 4)
		error_exit("short read on file, please delete cache file and retry");

	return (buffer[3] << 24) + (buffer[2] << 16) + (buffer[1] << 8) + buffer[0];
}

void write_value(FILE *fh, int value)
{
	unsigned char buffer[4];

	buffer[3] = (value >> 24) & 255;
	buffer[2] = (value >> 16) & 255;
	buffer[1] = (value >>  8) & 255;
	buffer[0] = (value      ) & 255;

	if (fwrite(buffer, 1, 4, fh) != 4)
		error_exit("short write on file, please delete cache file");
}

void load_knuth_file(std::string file, lookup_t *lt)
{
	FILE *fh = fopen(file.c_str(), "rb");
	if (!fh)
	{
		if (errno != ENOENT)
			error_exit("Error accessing file %s", file.c_str());

		dolog(LOG_INFO, "No cache file '%s' exists, starting with fresh buffers", file.c_str());

		lt -> t_size = KNUTH_SIZE;
		lt -> table = (unsigned short *)malloc(lt -> t_size * sizeof(unsigned short));

		lt -> A = (unsigned short *)malloc(A_B_SIZE * sizeof(unsigned short));
		lt -> B = (unsigned short *)malloc(A_B_SIZE * sizeof(unsigned short));
	}
	else
	{
		dolog(LOG_INFO, "Reading cached data from %s", file.c_str());

		lt -> t_size = read_value(fh);
		if (lt -> t_size > 67108864)
			error_exit("%s is probably corrupt (size > 128MB)", file.c_str());
		lt -> t_offset = read_value(fh);
		lt -> table = (unsigned short *)malloc(lt -> t_size * sizeof(unsigned short));
		size_t bytes = lt -> t_size * sizeof(unsigned short);
		if (fread(lt -> table, 1, bytes, fh) != bytes)
			error_exit("Short read in file, please delete %s and retry", file.c_str());

		int dummy = read_value(fh);
		if (dummy != A_B_SIZE)
			error_exit("Unexpected A/B size %d (expecting %d), please delete %s and retry", dummy, A_B_SIZE, file.c_str());

		bytes = A_B_SIZE * sizeof(unsigned short);
		lt -> A = (unsigned short *)malloc(A_B_SIZE * sizeof(unsigned short));
		if (fread(lt -> A, 1, bytes, fh) != bytes)
			error_exit("Short read in file, please delete %s and retry", file.c_str());
		lt -> B = (unsigned short *)malloc(A_B_SIZE * sizeof(unsigned short));
		if (fread(lt -> B, 1, bytes, fh) != bytes)
			error_exit("Short read in file, please delete %s and retry", file.c_str());

		fclose(fh);
	}
}

void write_knuth_file(std::string file, lookup_t *lt)
{
	dolog(LOG_INFO, "Writing cached data to %s", file.c_str());

	FILE *fh = fopen(file.c_str(), "wb");
	if (!fh)
		error_exit("Failed to create file %s", file.c_str());

	write_value(fh, lt -> t_size);
	write_value(fh, lt -> t_offset);
	size_t bytes = lt -> t_size * sizeof(unsigned short);
	if (fwrite(lt -> table, 1, bytes, fh) != bytes)
		error_exit("Error writing to file %s", file.c_str());

	write_value(fh, A_B_SIZE);
	bytes = A_B_SIZE * sizeof(unsigned short);
	if (fwrite(lt -> A, 1, bytes, fh) != bytes)
		error_exit("Error writing to file %s", file.c_str());
	if (fwrite(lt -> B, 1, bytes, fh) != bytes)
		error_exit("Error writing to file %s", file.c_str());

	fclose(fh);
}

int put_data(proxy_client_t *client, lookup_t *lt, bool is_A)
{
	char bit_cnt[4 + 1] = { 0 };

	// receive number of bits
	if (READ_TO(client -> fd, bit_cnt, 4, DEFAULT_COMM_TO) != 4)
		return -1;

	int cur_n_bits = atoi(bit_cnt);
	if (cur_n_bits > 9992)
		return -1;

	bool full = false;
	if (is_A && lt -> t_offset == lt -> t_size && lt -> n_A == A_B_SIZE)
		full = true;
	else if (!is_A && lt -> n_B == A_B_SIZE)
		full = true;

	char reply[4 + 4 + 1] = { 0 };
	make_msg(reply, 1, cur_n_bits);
	// make_msg(reply, full ? 9003 : 1, cur_n_bits);
	if (WRITE_TO(client -> fd, reply, 8, DEFAULT_COMM_TO) != 8)
		return -1;

	int cur_n_bytes = (cur_n_bits + 7) / 8;

	int in_len = cur_n_bytes + DATA_HASH_LEN;
	unsigned char *buffer_in = (unsigned char *)malloc(in_len);
	if (!buffer_in)
		error_exit("%s error allocating %d bytes of memory", client -> host.c_str(), in_len);

	if (READ_TO(client -> fd, (char *)buffer_in, in_len, DEFAULT_COMM_TO) != in_len)
	{
		dolog(LOG_INFO, "put|%s short read while retrieving entropy data", client -> host.c_str());

		free(buffer_in);

		return -1;
	}

	unsigned char *buffer_out = (unsigned char *)malloc(in_len);
	if (!buffer_out)
		error_exit("%s error allocating %d bytes of memory", client -> host.c_str(), cur_n_bytes);
	// FIXME lock_mem(buffer_out, cur_n_bytes);

	// decrypt data
	BF_cfb64_encrypt(buffer_in, buffer_out, in_len, &client -> key, client -> ivec, &client -> ivec_offset, BF_DECRYPT);

	unsigned char *entropy_data = &buffer_out[DATA_HASH_LEN];
	int entropy_data_len = cur_n_bytes;
	unsigned char hash[DATA_HASH_LEN];
	DATA_HASH_FUNC(entropy_data, entropy_data_len, hash);

	if (memcmp(hash, buffer_out, DATA_HASH_LEN) != 0)
	{
		dolog(LOG_WARNING, "Hash mismatch in retrieved entropy data!");

		free(buffer_out);
		free(buffer_in);

		return -1;
	}

	pthread_mutex_lock(&lt -> lock);

	bool use = false;
	if (is_A)
	{
		if (lt -> t_offset < lt -> t_size)
		{
			int n = lt -> t_size - lt -> t_offset;

			int do_n_bytes = min(n * sizeof(unsigned short), cur_n_bytes);
			memcpy(lt -> table, buffer_out, do_n_bytes);

			lt -> t_offset += do_n_bytes / sizeof(unsigned short);
			if (lt -> t_offset == lt -> t_size)
				dolog(LOG_INFO, "look-up table is filled");
			else
				dolog(LOG_DEBUG, "Look-up table fill: %.2f%%", double(lt -> t_offset * 100) / double(lt -> t_size));

			use = true;
		}
		else
		{
			int n = A_B_SIZE - lt -> n_A;

			int do_n_bytes = min(n * sizeof(unsigned short), cur_n_bytes);
			if (do_n_bytes > 0)
			{
				memcpy(&lt -> A[lt -> n_A], buffer_out, do_n_bytes);
				use = true;
			}

			lt -> n_A += do_n_bytes / sizeof(unsigned short);
		}
	}
	else
	{
		int n = A_B_SIZE - lt -> n_B;

		int do_n_bytes = min(n * sizeof(unsigned short), cur_n_bytes);
		if (do_n_bytes > 0)
		{
			memcpy(&lt -> B[lt -> n_B], buffer_out, do_n_bytes);
			use = true;
		}

		lt -> n_B += do_n_bytes / sizeof(unsigned short);
	}

	if (use)
		dolog(LOG_DEBUG, "storing %d bits from %s", cur_n_bits, client -> host.c_str());

	pthread_mutex_unlock(&lt -> lock);

	// memset
	// unlock_mem
	free(buffer_out);

	free(buffer_in);

	return 0;
}

int handle_client(proxy_client_t *client, lookup_t *lt, bool is_A)
{
	char cmd[4 + 1] = { 0 };

	if (READ_TO(client -> fd, cmd, 4, DEFAULT_COMM_TO) != 4)
	{
		dolog(LOG_INFO, "Short read on fd %d / %s", client -> fd, client -> host.c_str());
		return -1;
	}

	if (memcmp(cmd, "0003", 4) == 0) // server info msg
	{
		char *info = NULL;
		int info_len = 0;
		if (recv_length_data(client -> fd, &info, &info_len, DEFAULT_COMM_TO) == -1)
			return -1;

		client -> type = std::string(info);
		dolog(LOG_WARNING, "Client %s is: %s", client -> host.c_str(), info);

		free(info);
	}
	else if (memcmp(cmd, "0006", 4) == 0) // client info msg
	{
		char *info = NULL;
		int info_len = 0;
		if (recv_length_data(client -> fd, &info, &info_len, DEFAULT_COMM_TO) == -1)
			return -1;

		dolog(LOG_WARNING, "Clients (%s/%s) connecting to this proxy not supported!", client -> host.c_str(), info);
		free(info);

		return -1;
	}
	else if (memcmp(cmd, "0002", 4) == 0) // put data
	{
		return put_data(client, lt, is_A);
	}
	else
	{
		dolog(LOG_WARNING, "Unknown / unexpected message %s received", cmd);
		return -1;
	}

	return 0;
}

typedef struct
{
	protocol *p;
	lookup_t *lt;
}
thread_pars_t;

void * thread(void *pars)
{
	thread_pars_t *p = (thread_pars_t *)pars;

	for(;!sig_quit;)
	{
		pthread_mutex_lock(&p -> lt -> lock);

		// transmit to broker
		bool send = false;
		if (p -> lt -> n_A > 0 && p -> lt -> n_B > 0)
		{
			send = true;
			dolog(LOG_DEBUG, "buffered data: %d %d", p -> lt -> n_A, p -> lt -> n_B);
		}

		while (p -> lt -> n_A > 0 && p -> lt -> n_B > 0 && !sig_quit)
		{
			int n_short = 1249 / sizeof(unsigned short);

			int n = min(n_short, min(p -> lt -> n_A, p -> lt -> n_B));

			int n_bytes = n * sizeof(unsigned short);
			unsigned short *out = (unsigned short *)malloc(n_bytes);

			dolog(LOG_DEBUG, "Processing %d shorts", n);
			for(int loop=0; loop<n; loop++)
			{
				int A = p -> lt -> A[p -> lt -> n_A - 1];
				p -> lt -> n_A--;
				int B = p -> lt -> B[p -> lt -> n_B - 1];
				p -> lt -> n_B--;

				int j = B % p -> lt -> t_size;
				int v = p -> lt -> table[j];
				p -> lt -> table[j] = A;

				out[loop] = v;
			}

			pthread_mutex_unlock(&p -> lt -> lock);
			(void)p -> p -> message_transmit_entropy_data((unsigned char *)out, n_bytes);
			free(out);
			pthread_mutex_lock(&p -> lt -> lock);
		}
		if (send)
			dolog(LOG_DEBUG, "Finished transmitting");

		pthread_mutex_unlock(&p -> lt -> lock);

		usleep(250000); // FIXME condwait or so
	}

	return NULL;
}

void sig_handler(int sig)
{
	dolog(LOG_INFO, "Got signal %d", sig);

	if (sig == SIGTERM || sig == SIGINT || sig == SIGQUIT)
		sig_quit = true;
	else
	{
		fprintf(stderr, "Exit due to signal %d\n", sig);
		unlink(pid_file);
		exit(0);
	}
}

void help()
{
        printf("-I host   entropy_broker host to connect to\n");
        printf("          e.g. host\n");
        printf("               host:port\n");
        printf("               [ipv6 literal]:port\n");
        printf("          you can have multiple entries of this\n");
	printf("-j adapter  adapter to listen on\n");
	printf("-p port   port to listen on (default: %d)\n", DEFAULT_PROXY_LISTEN_PORT);
	printf("-l file   log to file 'file'\n");
	printf("-s        log to syslog\n");
	printf("-n        do not fork\n");
	printf("-V file   store buffers to this file (default: %s/%s)\n", CACHE_DIR, KNUTH_FILE);
	printf("-P file   write pid to file\n");
	printf("-X file   read username+password from file (to authenticate to broker)\n");
	printf("-U file   read u/p for clients from file (to authenticate local clients)\n");
}

int main(int argc, char *argv[])
{
	const char *listen_adapter = "0.0.0.0";
	int listen_port = DEFAULT_PROXY_LISTEN_PORT;
	int c;
	bool do_not_fork = false, log_console = false, log_syslog = false;
	char *log_logfile = NULL;
	std::string username, password;
	std::string clients_auths;
	std::string knuth_file = CACHE_DIR + std::string("/") + KNUTH_FILE;
	std::vector<std::string> hosts;

	printf("proxy_knuth_m, (C) 2009-2012 by folkert@vanheusden.com\n");

	while((c = getopt(argc, argv, "V:j:p:U:hf:X:P:I:l:sn")) != -1)
	{
		switch(c)
		{
			case 'V':
				knuth_file = optarg;
				break;

			case 'j':
				listen_adapter = optarg;
				break;

			case 'p':
				listen_port = atoi(optarg);
				break;

			case 'U':
				clients_auths = optarg;
				break;

			case 'X':
				get_auth_from_file(optarg, username, password);
				break;

			case 'P':
				pid_file = optarg;
				break;

			case 'I':
				hosts.push_back(optarg);
				break;

			case 's':
				log_syslog = true;
				break;

			case 'l':
				log_logfile = optarg;
				break;

			case 'n':
				do_not_fork = true;
				log_console = true;
				break;

			case 'h':
				help();
				return 0;

			default:
				fprintf(stderr, "-%c not known\n", c);
				help();
				return 1;
		}
	}

	if (username.length() == 0 || password.length() == 0)
		error_exit("password + username cannot be empty");

	if (clients_auths.length() == 0)
		error_exit("No file with usernames + passwords selected for client authentication");

	if (hosts.empty())
		error_exit("No host to connect to selected");

	(void)umask(0177);
	set_logging_parameters(log_console, log_logfile, log_syslog);

	users *user_map = new users(clients_auths);

	protocol *p = new protocol(&hosts, username, password, true, client_type);

	no_core();

	if (!do_not_fork)
	{
		if (daemon(0, 0) == -1)
			error_exit("fork failed");
	}

	write_pid(pid_file);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGTERM, sig_handler);
	signal(SIGINT , sig_handler);
	signal(SIGQUIT, sig_handler);

	proxy_client_t *clients[2] = { new proxy_client_t, new proxy_client_t };

	int listen_socket_fd = start_listen(listen_adapter, listen_port, 5);

	clients[0] -> fd = -1;
	clients[1] -> fd = -1;

	lookup_t lt;
	memset(&lt, 0x00, sizeof lt);

	load_knuth_file(knuth_file, &lt);

	pthread_mutex_init(&lt.lock, NULL);

	thread_pars_t tp = { p, &lt };
	pthread_t th;
	pthread_create(&th, NULL, thread, &tp);

	for(;!sig_quit;)
	{
		fd_set rfds;
		FD_ZERO(&rfds);

		int max_fd = -1;

		FD_SET(listen_socket_fd, &rfds);
		max_fd = max(max_fd, listen_socket_fd);

		for(int client_index=0; client_index<2; client_index++)
		{
			if (clients[client_index] -> fd != -1)
			{
				FD_SET(clients[client_index] -> fd, &rfds);
				max_fd = max(max_fd, clients[client_index] -> fd);
			}
		}

		sigset_t sig_set;

		if (sigemptyset(&sig_set) == -1)
			error_exit("sigemptyset");

		if (pselect(max_fd + 1, &rfds, NULL, NULL, NULL, &sig_set) == -1)
		{
			if (errno != EINTR)
				error_exit("select failed");

			continue;
		}

		for(int client_index=0; client_index<2; client_index++)
		{
			if (clients[client_index] -> fd != -1 && FD_ISSET(clients[client_index] -> fd, &rfds))
			{
				if (handle_client(clients[client_index], &lt, client_index == 0) == -1)
				{
					close(clients[client_index] -> fd);

					clients[client_index] -> fd = -1;
				}
			}
		}

		if (!sig_quit && FD_ISSET(listen_socket_fd, &rfds))
		{
			int new_socket_fd = accept(listen_socket_fd, NULL, NULL);
			if (new_socket_fd != -1)
			{
				std::string host = get_endpoint_name(new_socket_fd);

				dolog(LOG_INFO, "new client: %s (fd: %d)", host.c_str(), new_socket_fd);

				std::string client_password;
				long long unsigned int challenge = 1;
				if (auth_eb(new_socket_fd, DEFAULT_COMM_TO, user_map, client_password, &challenge) == 0)
				{
					if (clients[0] -> fd != -1 && clients[1] -> fd != -1)
					{
						dolog(LOG_WARNING, "New connection with 2 clients connected: dropping all previous connections");

						close(clients[0] -> fd);
						close(clients[1] -> fd);
						clients[0] -> fd = -1;
						clients[1] -> fd = -1;
					}

					proxy_client_t *pcp = NULL;

					if (clients[0] -> fd == -1)
						pcp = clients[0];
					else if (clients[1] -> fd == -1)
						pcp = clients[1];

					pcp -> fd = new_socket_fd;
					pcp -> challenge = challenge;

					char dummy_str[256];
					snprintf(dummy_str, sizeof dummy_str, "%s", host.c_str());
					pcp -> host = dummy_str;

					pcp -> challenge = challenge;
					pcp -> ivec_counter = 0;
					pcp -> ivec_offset = 0;
					calc_ivec((char *)client_password.c_str(), pcp -> challenge, pcp -> ivec_counter, pcp -> ivec);

					BF_set_key(&pcp -> key, client_password.length(), (unsigned char *)client_password.c_str());
				}
				else
				{
					close(new_socket_fd);
				}
			}
		}
	}

	dolog(LOG_INFO, "Terminating...");

	pthread_join(th, NULL);

	delete p;

	write_knuth_file(knuth_file, &lt);

	unlink(pid_file);

	dolog(LOG_INFO, "Finished");

	return 0;
}