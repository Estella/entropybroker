// SVN: $Id$
#include <string.h>
#include <arpa/inet.h>
#include <string>
#include <openssl/aes.h>

#include "error.h"
#include "random_source.h"
#include "utils.h"
#include "stirrer.h"
#include "stirrer_aes.h"

stirrer_aes::stirrer_aes(random_source_t rs_in) : stirrer(rs_in)
{
}

stirrer_aes::~stirrer_aes()
{
}

int stirrer_aes::get_stir_size() const
{
	return 256 / 8;
}

int stirrer_aes::get_ivec_size() const
{
	return AES_BLOCK_SIZE;
}

void stirrer_aes::do_stir(unsigned char *ivec, unsigned char *target, int target_size, unsigned char *data_in, int data_in_size, unsigned char *temp_buffer, bool direction)
{
	unsigned char temp_key[32];

	if (data_in_size > get_stir_size())
		error_exit("Invalid stir-size %d (expected: %d)", data_in_size, get_stir_size());

	memcpy(temp_key, data_in, data_in_size);

	// these generated bytes are not counted in the entropy
	// estimation
	if (data_in_size > 24 && data_in_size < 32)
	{
		get_random(rs, &temp_key[data_in_size], 32 - data_in_size);

		data_in_size = 32;
	}
	else if (data_in_size > 16 && data_in_size < 24)
	{
		get_random(rs, &temp_key[data_in_size], 24 - data_in_size);

		data_in_size = 24;
	}
	else if (data_in_size < 16)
	{
		get_random(rs, &temp_key[data_in_size], 16 - data_in_size);

		data_in_size = 16;
	}

	AES_KEY key;
	if (direction)
		AES_set_encrypt_key(temp_key, data_in_size * 8, &key);
	else
		AES_set_decrypt_key(temp_key, data_in_size * 8, &key);

	int ivec_offset = 0;
	AES_cfb128_encrypt(target, temp_buffer, target_size, &key, ivec, &ivec_offset, direction ? AES_ENCRYPT : AES_DECRYPT);
	memcpy(target, temp_buffer, target_size);
}
