/*
 * contrib/pgcrypto/crypt-sha.c
 */
#include "postgres.h"

#include "px-crypt.h"
#include "px.h"

typedef enum {
  PGCRYPTO_SHA256CRYPT = 0,
  PGCRYPTO_SHA512CRYPT = 1,
  PGCRYPTO_SHA_UNKOWN
} PGCRYPTO_SHA_t;

static unsigned char _crypt_itoa64[64 + 1] =
		"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/*
 * Modern UNIX password, based on SHA crypt hashes
 */
char *
px_crypt_shacrypt(const char *pw, const char *salt, char *passwd, unsigned dstlen)
{
	static const char rounds_prefix[] = "rounds=";
	static char *magic_bytes[2] = { "$5$", "$6$" };
	static const char ascii_dollar[] = { 0x24, 0x00 };

	/* "$n$rounds=<N>$......salt......$...shahash(up to 86 chars)...\0" */
	char out_buf[PX_SHACRYPT_BUF_LEN]; /* resulting encrypted password buffer */

	PGCRYPTO_SHA_t type = PGCRYPTO_SHA_UNKOWN;
	PX_MD *digestA      = NULL;
	PX_MD *digestB      = NULL;
	int    err;

	const char *dec_salt_binary; /* pointer into the real salt string */

	unsigned char sha_buf[PX_SHACRYPT_DIGEST_MAX_LENGTH];
	unsigned char sha_buf_tmp[PX_SHACRYPT_DIGEST_MAX_LENGTH]; /* temporary buffer for digests */

	char rounds_custom = 0;
	char *p_bytes = NULL;
	char *s_bytes = NULL;
	char *cp = NULL;
	const char *ep;       /* holds pointer to the end of the salt string */

	size_t buf_size = 0;  /* buffer size for sha256crypt/sha512crypt */
	unsigned int block;   /* number of bytes processed */
	unsigned int rounds = PX_SHACRYPT_ROUNDS_DEFAULT;

	unsigned len, salt_len;

	/* Sanity checks */
	if (pw == NULL)
	{
		elog(ERROR, "null value for password rejected");
	}

	if (salt == NULL)
	{
		elog(ERROR, "null value for salt rejected");
	}

	/*
	 * Make sure result buffers are large enough.
	 */
	if (dstlen < PX_SHACRYPT_BUF_LEN)
	{
		elog(ERROR, "insufficient result buffer size to encrypt password");
	}

	/* Init contents of buffers properly */
	memset(&out_buf, '\0', sizeof(out_buf));
	memset(&sha_buf, '\0', sizeof(sha_buf));
	memset(&sha_buf_tmp, '\0', sizeof(sha_buf_tmp));

	/*
	 * Decode the salt string. We need to know how many rounds and which
	 * digest we have to use to hash the password.
	 */
	len = strlen(pw);
	dec_salt_binary = salt;

	/*
	 * Analyze and prepare the salt string
	 *
	 * The magic string should be specified in the first three bytes
	 * of the salt string. But do some sanity checks before.
	 */
	if (strlen(dec_salt_binary) < 3)
	{
		elog(ERROR, "invalid salt");
	}

	/*
	 * Check format of magic bytes. These should define either
	 * 5=sha256crypt or 6=sha512crypt in the second byte, enclosed by
	 * ascii dollar signs.
	 */
	if ((dec_salt_binary[0] != ascii_dollar[0])
	    && (dec_salt_binary[2] != ascii_dollar[0]))
	{
		elog(ERROR, "invalid format of salt");
	}

	/*
	 * Check magic byte for supported shacrypt digest.
	 */
	if (strncmp(dec_salt_binary, magic_bytes[0], strlen(magic_bytes[0])) == 0)
	{
		type = PGCRYPTO_SHA256CRYPT;
		dec_salt_binary += strlen(magic_bytes[0]);
	}

	if (strncmp(dec_salt_binary, magic_bytes[1], strlen(magic_bytes[1])) == 0)
	{
		type = PGCRYPTO_SHA512CRYPT;
		dec_salt_binary += strlen(magic_bytes[1]);
	}

	/*
	 * dec_salt_binary pointer is positioned after the magic bytes now
	 *
	 * We extract any options in the following code branch. The only optional
	 * setting we need to take care of is the "rounds" option. Note that
	 * the salt generator already checked for invalid settings before, but
	 * we need to do it here again to protect against injection of wrong values
	 * when called without the generator.
	 *
	 * Unknown magic byte is handled below
	 */
	if (strncmp(dec_salt_binary,
	            rounds_prefix, sizeof(rounds_prefix) - 1) == 0) {

		const char *num = dec_salt_binary + sizeof(rounds_prefix) - 1;
		char *endp;
		unsigned long int srounds = strtoul (num, &endp, 10);
		if (*endp == '$') {
			dec_salt_binary = endp + 1;
			if (srounds > PX_SHACRYPT_ROUNDS_MAX)
				rounds = PX_SHACRYPT_ROUNDS_MAX;
			else if (srounds < PX_SHACRYPT_ROUNDS_MIN)
				rounds = PX_SHACRYPT_ROUNDS_MIN;
			else
				rounds = (unsigned int)srounds;
			rounds_custom = 1;
		} else {
			elog(ERROR, "could not parse salt options");
		}

	}

	/*
	 * We need the real length of the decoded salt string, this is every
	 * character after the last '$' in the preamble. After this, dec_salt_binary
	 * is now positioned at the beginning of the salt string.
	 */
	for (ep = dec_salt_binary;
	     *ep && *ep != '$' && ep < (dec_salt_binary + PX_SHACRYPT_SALT_LEN_MAX);
	     ep++) continue;
	salt_len = ep - dec_salt_binary;

	elog(DEBUG1, "using rounds = %d", rounds);

	/*
	 * Choose the correct digest length and add the magic bytes to
	 * the result buffer. Also handle possible invalid magic byte we've
	 * extracted above.
	 */
	switch(type)
	{
		case PGCRYPTO_SHA256CRYPT:
		{
			/* Two PX_MD objects required */
			err = px_find_digest("sha256", &digestA);
			if (err)
				goto error;

			err = px_find_digest("sha256", &digestB);
			if (err)
				goto error;

			/* digest buffer length is 32 for sha256 */
			buf_size = 32;

			elog(DEBUG1,
			     "using sha256crypt as requested by magic byte in salt");
			strlcat(out_buf, magic_bytes[0], sizeof(out_buf));
			break;
		}

		case PGCRYPTO_SHA512CRYPT:
		{
			/* Two PX_MD objects required */
			err = px_find_digest("sha512", &digestA);
			if (err)
				goto error;

			err = px_find_digest("sha512", &digestB);
			if (err)
				goto error;

			buf_size = PX_SHACRYPT_DIGEST_MAX_LENGTH;

			elog(DEBUG1,
			     "using sha512crypt as requested by magic byte in salt");
			strlcat(out_buf, magic_bytes[1], sizeof(out_buf));
			break;
		}

		case PGCRYPTO_SHA_UNKOWN:
			elog(ERROR, "unknown crypt identifier \"%c\"", salt[1]);
	}

	if (rounds_custom > 0)
	{

		char tmp_buf[80]; /* "rounds=999999999" */

		snprintf(tmp_buf, sizeof(tmp_buf), "rounds=%u", rounds);
		strlcat(out_buf, tmp_buf, sizeof(out_buf));
		strlcat(out_buf, ascii_dollar, sizeof(out_buf));

	}

	strlcat(out_buf, dec_salt_binary, sizeof(out_buf));

	if (strlen(out_buf) > 3 + 17 * rounds_custom + salt_len)
	{
		elog(ERROR, "invalid salt string");
	}

	/*
	 * 1. Start digest A
	 * 2. Add the password string to digest A
	 * 3. Add the salt to digest A
	 */
	px_md_update(digestA, (const unsigned char *)pw, len);
	px_md_update(digestA, (const unsigned char *)dec_salt_binary, salt_len);

	/*
	 * 4. Create digest B
	 * 5. Add password to digest B
	 * 6. Add the salt string to digest B
	 * 7. Add the password again to digest B
	 * 8. Finalize digest B
	 */
	px_md_update(digestB, (const unsigned char *)pw, len);
	px_md_update(digestB, (const unsigned char *)dec_salt_binary, salt_len);
	px_md_update(digestB, (const unsigned char *)pw, len);
	px_md_finish(digestB, sha_buf);

	/*
	 * 9. For each block of (excluding the NULL byte), add
	 *    digest B to digest A.
	 */
	for (block = len; block > buf_size; block -= buf_size)
	{
		px_md_update(digestA, sha_buf, buf_size);
	}

	/* 10 For the remaining N bytes of the password string, add
	 * the first N bytes of digest B to A */
	px_md_update(digestA, sha_buf, block);

	/*
	 * 11 For each bit of the binary representation of the length of the
	 * password string up to and including the highest 1-digit, starting
	 * from to lowest bit position (numeric value 1)
	 *
	 * a) for a 1-digit add digest B (sha_buf) to digest A
	 * b) for a 0-digit add the password string
	 */

	block = len;
	while(block)
	{
		px_md_update(digestA,
					 (block & 1) ? sha_buf : (const unsigned char *)pw,
					 (block & 1) ? buf_size : len);

		/* right shift to next byte */
		block >>= 1;
	}

	/* 12 Finalize digest A */
	px_md_finish(digestA, sha_buf);

	/* 13 Start digest DP */
	px_md_reset(digestB);

	/*
	 * 14 Add every byte of the password string (excluding trailing NULL)
	 * to the digest DP
	 */
	for (block = len; block > 0; block--) {
		px_md_update(digestB, (const unsigned char *)pw, len);
	}

	/* 15 Finalize digest DP */
	px_md_finish(digestB, sha_buf_tmp);

	/*
	 * 16 produce byte sequence P with same length as password.
	 *
	 *     a) for each block of 32 or 64 bytes of length of the password
	 *        string the entire digest DP is used
	 *     b) for the remaining N (up to  31 or 63) bytes use the
	 *         first N bytes of digest DP
	 */
	if ((p_bytes = palloc0(len)) == NULL)
	{
		goto error;
	}

	/* N step of 16, copy over the bytes from password */
	for (cp = p_bytes, block = len; block > buf_size; block -= buf_size, cp += buf_size)
		memcpy(cp, sha_buf_tmp, buf_size);
	memcpy(cp, sha_buf_tmp, block);

	/*
	 * 17 Start digest DS
	 */
	px_md_reset(digestB);

	/*
	 * 18 Repeat the following 16+A[0] times, where A[0] represents the first
	 *    byte in digest A interpreted as an 8-bit unsigned value
	 *    add the salt to digest DS
	 */
	for (block = 16 + sha_buf[0]; block > 0; block--)
	{
		px_md_update(digestB, (const unsigned char *)dec_salt_binary, salt_len);
	}

	/*
	 * 19 Finalize digest DS
	 */
	px_md_finish(digestB, sha_buf_tmp);

	/*
	 * 20 Produce byte sequence S of the same length as the salt string where
	 *
	 * a) for each block of 32 or 64 bytes of length of the salt string the
	 *     entire digest DS is used
	 *
	 * b) for the remaining N (up to  31 or 63) bytes use the first N
	 *    bytes of digest DS
	 */
	if ((s_bytes = palloc0(salt_len)) == NULL)
		goto error;

	for (cp = s_bytes, block = salt_len; block > buf_size; block -= buf_size, cp += buf_size) {
		memcpy(cp, sha_buf_tmp, buf_size);
	}
	memcpy(cp, sha_buf_tmp, block);

	/*
	 * 21 Repeat a loop according to the number specified in the rounds=<N>
	 *    specification in the salt (or the default value if none is
	 *    present).  Each round is numbered, starting with 0 and up to N-1.
	 *
	 *    The loop uses a digest as input.  In the first round it is the
	 *    digest produced in step 12.  In the latter steps it is the digest
	 *    produced in step 21.h of the previous round.  The following text
	 *    uses the notation "digest A/B" to describe this behavior.
	 */
	for (block = 0; block < rounds; block++) {

		/* a) start digest B */
		px_md_reset(digestB);

		/*
		 * b) for odd round numbers add the byte sequense P to digest B
		 * c) for even round numbers add digest A/B
		 */
		px_md_update(digestB,
					 (block & 1) ? (const unsigned char *)p_bytes : sha_buf,
					 (block & 1) ? len : buf_size);

		/*  d) for all round numbers not divisible by 3 add the byte sequence S */
		if (block % 3) {
			px_md_update(digestB, (const unsigned char *)s_bytes, salt_len);
		}

		/* e) for all round numbers not divisible by 7 add the byte sequence P */
		if (block % 7) {
			px_md_update(digestB, (const unsigned char *)p_bytes, len);
		}

		/*
		 * f) for odd round numbers add digest A/C
		 * g) for even round numbers add the byte sequence P
		 */
		px_md_update(digestB,
					 (block & 1) ? sha_buf : (const unsigned char *)p_bytes,
					 (block & 1) ? buf_size : len);

		/* h) finish digest C. */
		px_md_finish(digestB, sha_buf);

	}

	px_md_free(digestA);
	px_md_free(digestB);

	digestA = NULL;
	digestB = NULL;

	pfree(s_bytes);
	pfree(p_bytes);

	s_bytes = NULL;
	p_bytes = NULL;

	/* prepare final result buffer */
	cp = out_buf + strlen(out_buf);
	*cp++ = ascii_dollar[0];

# define b64_from_24bit(B2, B1, B0, N)                                  \
    do {                                                                \
        unsigned int w = ((B2) << 16) | ((B1) << 8) | (B0);             \
        int i = (N);                                                    \
        while (i-- > 0)                                                 \
            {                                                           \
                *cp++ = _crypt_itoa64[w & 0x3f];                            \
                w >>= 6;                                                \
            }                                                           \
    } while (0)

	switch(type)
	{
		case PGCRYPTO_SHA256CRYPT:
		{
			b64_from_24bit (sha_buf[0], sha_buf[10], sha_buf[20], 4);
			b64_from_24bit (sha_buf[21], sha_buf[1], sha_buf[11], 4);
			b64_from_24bit (sha_buf[12], sha_buf[22], sha_buf[2], 4);
			b64_from_24bit (sha_buf[3], sha_buf[13], sha_buf[23], 4);
			b64_from_24bit (sha_buf[24], sha_buf[4], sha_buf[14], 4);
			b64_from_24bit (sha_buf[15], sha_buf[25], sha_buf[5], 4);
			b64_from_24bit (sha_buf[6], sha_buf[16], sha_buf[26], 4);
			b64_from_24bit (sha_buf[27], sha_buf[7], sha_buf[17], 4);
			b64_from_24bit (sha_buf[18], sha_buf[28], sha_buf[8], 4);
			b64_from_24bit (sha_buf[9], sha_buf[19], sha_buf[29], 4);
			b64_from_24bit (0, sha_buf[31], sha_buf[30], 3);

			break;
		}

		case PGCRYPTO_SHA512CRYPT:
		{
			b64_from_24bit (sha_buf[0], sha_buf[21], sha_buf[42], 4);
			b64_from_24bit (sha_buf[22], sha_buf[43], sha_buf[1], 4);
			b64_from_24bit (sha_buf[44], sha_buf[2], sha_buf[23], 4);
			b64_from_24bit (sha_buf[3], sha_buf[24], sha_buf[45], 4);
			b64_from_24bit (sha_buf[25], sha_buf[46], sha_buf[4], 4);
			b64_from_24bit (sha_buf[47], sha_buf[5], sha_buf[26], 4);
			b64_from_24bit (sha_buf[6], sha_buf[27], sha_buf[48], 4);
			b64_from_24bit (sha_buf[28], sha_buf[49], sha_buf[7], 4);
			b64_from_24bit (sha_buf[50], sha_buf[8], sha_buf[29], 4);
			b64_from_24bit (sha_buf[9], sha_buf[30], sha_buf[51], 4);
			b64_from_24bit (sha_buf[31], sha_buf[52], sha_buf[10], 4);
			b64_from_24bit (sha_buf[53], sha_buf[11], sha_buf[32], 4);
			b64_from_24bit (sha_buf[12], sha_buf[33], sha_buf[54], 4);
			b64_from_24bit (sha_buf[34], sha_buf[55], sha_buf[13], 4);
			b64_from_24bit (sha_buf[56], sha_buf[14], sha_buf[35], 4);
			b64_from_24bit (sha_buf[15], sha_buf[36], sha_buf[57], 4);
			b64_from_24bit (sha_buf[37], sha_buf[58], sha_buf[16], 4);
			b64_from_24bit (sha_buf[59], sha_buf[17], sha_buf[38], 4);
			b64_from_24bit (sha_buf[18], sha_buf[39], sha_buf[60], 4);
			b64_from_24bit (sha_buf[40], sha_buf[61], sha_buf[19], 4);
			b64_from_24bit (sha_buf[62], sha_buf[20], sha_buf[41], 4);
			b64_from_24bit (0, 0, sha_buf[63], 2);

			break;
		}

		default:
			goto error;
	}

	*cp = '\0';

	/* copy over result to specified buffer ... */
	memcpy(passwd, out_buf, dstlen);

	/* ...and we're done */
	return passwd;

error:
	if (digestA != NULL)
		px_md_free(digestA);

	if (digestB != NULL)
		px_md_free(digestB);

	elog(ERROR, "cannot create encrypted password");
}