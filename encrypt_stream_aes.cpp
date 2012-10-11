// SVN: $Revision$
#include <string>
#include <string.h>
#include <stdio.h>

#include "error.h"
#include "encrypt_stream.h"
#include "encrypt_stream_aes.h"
#include "utils.h"

//pthread_mutex_t lock_aes = PTHREAD_MUTEX_INITIALIZER;

encrypt_stream_aes::encrypt_stream_aes()
{
	enc = NULL;
	dec = NULL;
}

encrypt_stream_aes::~encrypt_stream_aes()
{
	delete enc;
	delete dec;
}

int encrypt_stream_aes::get_ivec_size()
{
	return CryptoPP::AES::BLOCKSIZE;
}

int encrypt_stream_aes::get_key_size()
{
	return CryptoPP::AES::DEFAULT_KEYLENGTH;
}

bool encrypt_stream_aes::init(unsigned char *key_in, int key_len, unsigned char *ivec_in, bool force)
{
	my_assert(key_len > 0);

#ifdef CRYPTO_DEBUG
	printf("KEY: "); hexdump(key_in, key_len);
#endif

	unsigned char key_use[CryptoPP::AES::DEFAULT_KEYLENGTH] = { 0 };
	memcpy(key_use, key_in, mymin(CryptoPP::AES::DEFAULT_KEYLENGTH, key_len));

//	pthread_check(pthread_mutex_lock(&lock_aes), "pthread_mutex_lock");
	if (enc)
		delete enc;
	if (dec)
		delete dec;
	enc = new CryptoPP::CFB_Mode<CryptoPP::AES>::Encryption(key_use, CryptoPP::AES::DEFAULT_KEYLENGTH, ivec_in);
	dec = new CryptoPP::CFB_Mode<CryptoPP::AES>::Decryption(key_use, CryptoPP::AES::DEFAULT_KEYLENGTH, ivec_in);
//	pthread_check(pthread_mutex_unlock(&lock_aes), "pthread_mutex_lock");

	return true;
}

std::string encrypt_stream_aes::get_name()
{
	return "aes";
}

void encrypt_stream_aes::encrypt(unsigned char *p, int len, unsigned char *p_out)
{
	my_assert(len > 0);

#ifdef CRYPTO_DEBUG
	printf("ORG: "); hexdump(p, len);
	printf("EIV %d before: ", ivec_offset); hexdump(ivec, 8);
#endif

//	pthread_check(pthread_mutex_lock(&lock_aes), "pthread_mutex_lock");
	enc -> ProcessData(p_out, p, len);
//	pthread_check(pthread_mutex_unlock(&lock_aes), "pthread_mutex_lock");

#ifdef CRYPTO_DEBUG
	printf("EIV %d after: ", ivec_offset); hexdump(ivec, 8);
	printf("ENC: "); hexdump(p_out, len);
#endif
}

void encrypt_stream_aes::decrypt(unsigned char *p, int len, unsigned char *p_out)
{
	my_assert(len > 0);

#ifdef CRYPTO_DEBUG
	printf("DEC: "); hexdump(p, len);
	printf("EIV %d before: ", ivec_offset); hexdump(ivec, 8);
#endif

//	pthread_check(pthread_mutex_lock(&lock_aes), "pthread_mutex_lock");
	dec -> ProcessData(p_out, p, len);
//	pthread_check(pthread_mutex_unlock(&lock_aes), "pthread_mutex_lock");

#ifdef CRYPTO_DEBUG
	printf("EIV %d after: ", ivec_offset); hexdump(ivec, 8);
	printf("ORG: "); hexdump(p_out, len);
#endif
}
