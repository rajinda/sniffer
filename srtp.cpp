#include <iostream>
#include <netinet/in.h>

#include "config.h"
#include "tools.h"

#include "srtp.h"


RTPsecure::RTPsecure(const char *crypto_suite, const char *sdes, eMode mode) {
	if(crypto_suite) {
		this->crypto_suite = crypto_suite;
	}
	this->sdes = sdes;
	#if HAVE_LIBSRTP
		this->mode = mode;
	#else
		this->mode = mode_native;
	#endif
	tag_len = 10;
	key_len = 128;
	cipher = GCRY_CIPHER_AES;
	md = GCRY_MD_SHA1;
	rtcp_index = 0;
	rtp_roc = 0;
	rtp_seq = 0;
	rtp_rcc = 1;
	error = err_na;
	rtcp_unencrypt_header = 8;
	rtcp_unencrypt_footer = 4;
	init();
	if(mode == mode_native) {
		init_native();
	} else {
		init_libsrtp();
	}
}

bool RTPsecure::gcrypt_lib_init = false;
bool RTPsecure::srtp_lib_init = false;

RTPsecure::~RTPsecure() {
}

bool RTPsecure::decrypt_rtp(u_char *data, unsigned *data_len, u_char *payload, unsigned *payload_len) {
	if(*payload_len <= tag_len) {
		return(false);
	}
	++rtp.counter_packets;
	if(mode == mode_native) {
		return(decrypt_rtp_native(data, data_len, payload, payload_len));
	} else {
		return(decrypt_rtp_libsrtp(data, data_len, payload, payload_len));
	}
}
 
bool RTPsecure::decrypt_rtp_native(u_char *data, unsigned *data_len, u_char *payload, unsigned *payload_len) {
	uint16_t seq = get_seq_rtp(data);
	uint32_t ssrc = get_ssrc_rtp(data);
	if(rtp.counter_packets == 1) {
		rtp_seq = seq;
	}
	uint32_t roc = compute_rtp_roc(seq);
	u_char *tag = rtp_digest(data, *data_len - tag_len, roc);
        if(memcmp(data + *data_len - tag_len, tag, tag_len)) {
		return(false);
	}
	if(!rtpDecrypt(payload, *payload_len - tag_len, seq, ssrc)) {
		return(false);
	}
	*data_len -= tag_len;
	*payload_len -= tag_len;
	//cout << rtp.counter_packets << endl;
	//hexdump(data, data_len - tag_len);
	return(true);
}

bool RTPsecure::decrypt_rtp_libsrtp(u_char *data, unsigned *data_len, u_char */*payload*/, unsigned *payload_len) {
	#if HAVE_LIBSRTP
	if(rtp.counter_packets == 1) {
		rtp.policy.ssrc.value = get_ssrc_rtp(data);
		srtp_create(&rtp.srtp_ctx, &rtp.policy);
	}
	int _data_len = *data_len;
	if(srtp_unprotect(rtp.srtp_ctx, data, &_data_len) == 0) {
		int diff_len = *data_len - _data_len;
		*data_len = _data_len;
		*payload_len -= diff_len;
	}
	#endif
	return(true);
}

bool RTPsecure::decrypt_rtcp(u_char *data, unsigned *data_len) {
	if(*data_len <= (tag_len + rtcp_unencrypt_header + rtcp_unencrypt_footer)) {
		return(false);
	}
	++rtcp.counter_packets;
	if(mode == mode_native) {
		return(decrypt_rtcp_native(data, data_len));
	} else {
		return(decrypt_rtcp_libsrtp(data, data_len));
	}
}

bool RTPsecure::decrypt_rtcp_native(u_char *data, unsigned *data_len) {
	u_char *tag = rtcp_digest(data, *data_len - tag_len);
	if(memcmp(data + *data_len - tag_len, tag, tag_len)) {
		return(false);
	}
	if(!rtcpDecrypt(data, *data_len - tag_len)) {
		return(false);
	}
	*data_len -= tag_len;
	return(true);
}

bool RTPsecure::decrypt_rtcp_libsrtp(u_char *data, unsigned *data_len) {
	#if HAVE_LIBSRTP
	if(rtcp.counter_packets == 1) {
		rtcp.policy.ssrc.value = get_ssrc_rtcp(data);
		srtp_create(&rtcp.srtp_ctx, &rtcp.policy);
	}
	int _data_len = *data_len;
	if(srtp_unprotect_rtcp(rtcp.srtp_ctx, data, &_data_len) == 0) {
		*data_len = _data_len;
	}
	#endif
	return(true);
}

void RTPsecure::setError(eError error) {
	this->error = error;
}

void RTPsecure::init() {
	static struct {
		string crypro_suite;
		int key_len;
		int tag_len;
	} srtp_crypto_suites[] = {
		{ "AES_CM_128_HMAC_SHA1_32", 128, 4 },
		{ "AES_CM_128_HMAC_SHA1_32", 128, 4 },
		{ "AES_CM_128_HMAC_SHA1_80", 128, 10 }
	};
	if(crypto_suite.length()) {
		for(unsigned i = 0; i < sizeof(srtp_crypto_suites) / sizeof(srtp_crypto_suites[0]); i++) {
			if(crypto_suite == srtp_crypto_suites[i].crypro_suite) {
				tag_len = srtp_crypto_suites[i].tag_len;
				key_len = srtp_crypto_suites[i].key_len;
				break;
			}
		}
	}
}

bool RTPsecure::init_native() {
	if(!decodeKey()) {
		return(false);
	}
	if(tag_len > gcry_md_get_algo_dlen(md)) {
		setError(err_bad_tag_len);
		return(false);
	}
	if(!gcrypt_lib_init) {
		if(gcry_check_version(GCRYPT_VERSION)) {
			gcrypt_lib_init = true;
		} else {
			setError(err_gcrypt_init);
			return(false);
		}
	}
	if(gcry_cipher_open(&rtp.cipher, cipher, GCRY_CIPHER_MODE_CTR, 0)) {
		setError(err_cipher_open);
		return(false);
	}
	if(gcry_md_open (&rtp.md, md, GCRY_MD_FLAG_HMAC)) {
		setError(err_md_open);
		return(false);
	}
	if(gcry_cipher_open(&rtcp.cipher, cipher, GCRY_CIPHER_MODE_CTR, 0)) {
		setError(err_cipher_open);
		return(false);
	}
	if(gcry_md_open (&rtcp.md, md, GCRY_MD_FLAG_HMAC)) {
		setError(err_md_open);
		return(false);
	}
	gcry_cipher_hd_t _cipher;
	if(gcry_cipher_open(&_cipher, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CTR, 0) ||
	   gcry_cipher_setkey(_cipher, key, sizeof(key))) {
		setError(err_set_key);
		return(false);
	}
	// SRTP key derivation
	u_char r[6];
	u_char keybuf[20];
        memset(r, 0, sizeof(r));
	memset(keybuf, 0, sizeof (keybuf));
	if(do_derive(_cipher, r, 6, SRTP_CRYPT, keybuf, 16) ||
	   gcry_cipher_setkey(rtp.cipher, keybuf, 16) ||
	   do_derive(_cipher, r, 6, SRTP_AUTH, keybuf, 20) ||
	   gcry_md_setkey(rtp.md, keybuf, 20) ||
	   do_derive (_cipher, r, 6, SRTP_SALT, (u_char*)rtp.salt, 14)) {
		setError(err_set_key);
		return(false);
	}
	
	// SRTP key derivation
	uint32_t _rtcp_index = htonl(this->rtcp_index);
	memcpy(r, &_rtcp_index, 4);
	if(do_derive(_cipher, r, 6, SRTCP_CRYPT, keybuf, 16) ||
	   gcry_cipher_setkey(rtcp.cipher, keybuf, 16) ||
	   do_derive(_cipher, r, 6, SRTCP_AUTH, keybuf, 20) ||
	   gcry_md_setkey (rtcp.md, keybuf, 20) ||
	   do_derive (_cipher, r, 6, SRTCP_SALT, (u_char*)rtcp.salt, 14)) {
		setError(err_set_key);
		return(false);
	}
	gcry_cipher_close(_cipher);
        return(true);
}

bool RTPsecure::init_libsrtp() {
	#if HAVE_LIBSRTP
	if(!decodeKey()) {
		return(false);
	}
	if(!srtp_lib_init) {
		srtp_init();
		srtp_lib_init = true;
	}
	for(int i = 0; i < 2; i++) {
		srtp_policy_t *policy = i == 0 ? &rtp.policy : &rtcp.policy;
		switch(key_len) {
		case 128:
			crypto_policy_set_rtp_default(&policy->rtp);
			crypto_policy_set_rtcp_default(&policy->rtcp);
			break;
		case 256:
			crypto_policy_set_aes_cm_256_hmac_sha1_80(&policy->rtp);
			crypto_policy_set_rtcp_default(&policy->rtcp);
			break;
		}
		policy->key = key_salt;
		policy->ssrc.type = ssrc_specific;
		policy->window_size = 128;
		policy->rtp.sec_serv = sec_serv_conf_and_auth;
		policy->rtp.auth_tag_len = tag_len;
		policy->rtcp.sec_serv = sec_serv_conf_and_auth;
		policy->rtcp.auth_tag_len = tag_len;
	}
	#endif
	return(true);
}

bool RTPsecure::decodeKey() {
	static char *b64chars = (char*)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	if(sdes.length() != 40) {
		setError(err_bad_sdes_length);
		return(false);
	}
	u_char sdes_raw[30];
	for(unsigned i = 0; i*4 < sdes.length(); i++) {
		u_char shifts[4];
		for (unsigned j = 0; j < 4; j++) {
			char *p = strchr(b64chars, sdes[4*i + j]);
			if(!p) {
				setError(err_bad_sdes_content);
				return(false);
			}
			shifts[j] = p-b64chars;
		}
		sdes_raw[3*i + 0] = (shifts[0]<<2)|(shifts[1]>>4);
		sdes_raw[3*i + 1] = (shifts[1]<<4)|(shifts[2]>>2);
		sdes_raw[3*i + 2] = (shifts[2]<<6)|shifts[3];
		
	}
	memcpy(key_salt, sdes_raw, 30);
	memcpy(key, sdes_raw, 16);
	memcpy(salt, sdes_raw + 16, 14);
	/*
	cout << "key / salt" << endl;
	hexdump(key, 16);
	hexdump(salt, 14);
	*/
	return(true);
}

bool RTPsecure::rtpDecrypt(u_char *payload, unsigned payload_len, uint16_t seq, uint32_t ssrc) {
	uint32_t roc = compute_rtp_roc(seq);
	// Updates ROC and sequence (it's safe now)
	int16_t diff = seq - this->rtp_seq;
	if(diff > 0) {
		// Sequence in the future, good
		rtp.window = rtp.window << diff;
		rtp.window |= 1;
		rtp_seq = seq;
		rtp_roc = roc;
	} else {
		// Sequence in the past/present, bad
		diff = -diff;
		if((diff >= 64) || ((rtp.window >> diff) & 1)) {
			return(false); // Replay attack
		}
		rtp.window |= 1 << diff;
	}
	if(rtp_decrypt(payload, payload_len, ssrc, roc, seq)) {
		return(false);
	}
	return(true);
}

bool RTPsecure::rtcpDecrypt(u_char *data, unsigned data_len) {
	uint32_t index;
	memcpy(&index, data + data_len - rtcp_unencrypt_footer, 4);
	index = ntohl (index);
	index &= ~(1 << 31); // clear E-bit for counter
	// Updates SRTCP index (safe here)
	int32_t diff = index - this->rtcp_index;
	if (diff > 0) {
		// Packet in the future, good
		rtcp.window = rtcp.window << diff;
		rtcp.window |= 1;
		rtcp_index = index;
	} else {
		// Packet in the past/present, bad
		diff = -diff;
		if ((diff >= 64) || ((rtcp.window >> diff) & 1)) {
			return(false); // replay attack!
		}
		rtp.window |= 1 << diff;
	}
	if(rtcp_decrypt(data + rtcp_unencrypt_header, data_len - rtcp_unencrypt_header - rtcp_unencrypt_footer, get_ssrc_rtcp(data), index)) {
		return(false);
	}
	return(true);
}

uint32_t RTPsecure::compute_rtp_roc(uint16_t seq) {
	uint32_t roc = this->rtp_roc;
	if(((seq - this->rtp_seq) & 0xffff) < 0x8000) {
		// Sequence is ahead, good
		if(seq < this->rtp_seq)
			roc++; // Sequence number wrap
	} else {
		// Sequence is late, bad
		if(seq > this->rtp_seq) {
			roc--; // Wrap back
		}
	}
	return(roc);
}

u_char *RTPsecure::rtp_digest(u_char *data, size_t data_len, uint32_t roc) {
	gcry_md_reset(rtp.md);
	gcry_md_write(rtp.md, data, data_len);
	roc = htonl(roc);
	gcry_md_write(rtp.md, &roc, 4);
	return(gcry_md_read(rtp.md, 0));
}

u_char *RTPsecure::rtcp_digest(u_char *data, size_t data_len) {
	gcry_md_reset(rtcp.md);
	gcry_md_write(rtcp.md, data, data_len);
	return(gcry_md_read(rtcp.md, 0));
}

int RTPsecure::rtp_decrypt(u_char *data, unsigned data_len, uint32_t ssrc, uint32_t roc, uint16_t seq) {
	// Determines cryptographic counter (IV)
	uint32_t counter[4];
	counter[0] = rtp.salt[0];
	counter[1] = rtp.salt[1] ^ htonl(ssrc);
	counter[2] = rtp.salt[2] ^ htonl(roc);
	counter[3] = rtp.salt[3] ^ htonl(seq << 16);
	// Decryption
	return(do_ctr_crypt(rtp.cipher, (u_char*)counter, data, data_len));
}

int RTPsecure::rtcp_decrypt(u_char *data, unsigned data_len, uint32_t ssrc, uint32_t index) {
	uint32_t counter[4];
	counter[0] = rtcp.salt[0];
	counter[1] = rtcp.salt[1] ^ htonl(ssrc);
	counter[2] = rtcp.salt[2] ^ htonl(index >> 16);
	counter[3] = rtcp.salt[3] ^ htonl((index & 0xffff) << 16);
	// Decryption
	return(do_ctr_crypt(rtcp.cipher, (u_char*)counter, data, data_len));
}

int RTPsecure::do_derive(gcry_cipher_hd_t cipher, u_char *r, unsigned rlen, uint8_t label, u_char *out, unsigned outlen) {
	u_char iv[16];
	memset(iv, 0, sizeof(iv));
	memcpy(iv, salt, sizeof(salt));
	iv[sizeof(salt) - 1 - rlen] ^= label;
	for(unsigned i = 0; i < rlen; i++) {
		iv[sizeof(iv) - rlen + i] ^= r[i];
	}
	memset(out, 0, outlen);
	return(do_ctr_crypt(cipher, iv, out, outlen));
}

int RTPsecure::do_ctr_crypt (gcry_cipher_hd_t cipher, u_char *ctr, u_char *data, unsigned len) {
	unsigned ctrlen = 16;
	div_t d = div((int)len, (int)ctrlen);
	if(gcry_cipher_setctr(cipher, ctr, ctrlen) ||
	   gcry_cipher_decrypt(cipher, data, d.quot * ctrlen, NULL, 0)) {
		return -1;
	}
	if(d.rem) {
		// Truncated last block */
		u_char dummy[ctrlen];
		data += d.quot * ctrlen;
		memcpy(dummy, data, d.rem);
		memset(dummy + d.rem, 0, ctrlen - d.rem);
		if(gcry_cipher_decrypt(cipher, dummy, ctrlen, data, ctrlen)) {
			return -1;
		}
		memcpy (data, dummy, d.rem);
	}
	return(0);
}
