/*	$OpenBSD: filemode.c,v 1.46 2024/06/12 10:03:09 tb Exp $ */
/*
 * Copyright (c) 2019 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/types.h>

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <imsg.h>

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "extern.h"
#include "json.h"

static X509_STORE_CTX	*ctx;
static struct auth_tree	 auths = RB_INITIALIZER(&auths);
static struct crl_tree	 crlt = RB_INITIALIZER(&crlt);

struct tal		*talobj[TALSZ_MAX];

struct uripath {
	RB_ENTRY(uripath)	 entry;
	const char		*uri;
	struct cert		*cert;
};

static RB_HEAD(uripath_tree, uripath) uritree;

static inline int
uripathcmp(const struct uripath *a, const struct uripath *b)
{
	return strcmp(a->uri, b->uri);
}

RB_PROTOTYPE(uripath_tree, uripath, entry, uripathcmp);

static void
uripath_add(const char *uri, struct cert *cert)
{
	struct uripath *up;

	if ((up = calloc(1, sizeof(*up))) == NULL)
		err(1, NULL);
	if ((up->uri = strdup(uri)) == NULL)
		err(1, NULL);
	up->cert = cert;
	if (RB_INSERT(uripath_tree, &uritree, up) != NULL)
		errx(1, "corrupt AIA lookup tree");
}

static struct cert *
uripath_lookup(const char *uri)
{
	struct uripath needle = { .uri = uri };
	struct uripath *up;

	up = RB_FIND(uripath_tree, &uritree, &needle);
	if (up == NULL)
		return NULL;
	return up->cert;
}

RB_GENERATE(uripath_tree, uripath, entry, uripathcmp);

/*
 * Use the X509 CRL Distribution Points to locate the CRL needed for
 * verification.
 */
static void
parse_load_crl(char *uri)
{
	struct crl *crl;
	char *f;
	size_t flen;

	if (uri == NULL)
		return;
	if (strncmp(uri, RSYNC_PROTO, RSYNC_PROTO_LEN) != 0) {
		warnx("bad CRL distribution point URI %s", uri);
		return;
	}
	uri += RSYNC_PROTO_LEN;

	f = load_file(uri, &flen);
	if (f == NULL) {
		warn("parse file %s", uri);
		return;
	}

	crl = crl_parse(uri, f, flen);
	if (crl != NULL && !crl_insert(&crlt, crl))
		crl_free(crl);

	free(f);
}

/*
 * Parse the cert pointed at by the AIA URI while doing that also load
 * the CRL of this cert. While the CRL is validated the returned cert
 * is not. The caller needs to make sure it is validated once all
 * necessary certs were loaded. Returns NULL on failure.
 */
static struct cert *
parse_load_cert(char *uri)
{
	struct cert *cert = NULL;
	char *f;
	size_t flen;

	if (uri == NULL)
		return NULL;

	if (strncmp(uri, RSYNC_PROTO, RSYNC_PROTO_LEN) != 0) {
		warnx("bad authority information access URI %s", uri);
		return NULL;
	}
	uri += RSYNC_PROTO_LEN;

	f = load_file(uri, &flen);
	if (f == NULL) {
		warn("parse file %s", uri);
		goto done;
	}

	cert = cert_parse_pre(uri, f, flen);
	free(f);

	if (cert == NULL)
		goto done;
	if (cert->purpose != CERT_PURPOSE_CA) {
		warnx("AIA reference to %s in %s",
		    purpose2str(cert->purpose), uri);
		goto done;
	}
	/* try to load the CRL of this cert */
	parse_load_crl(cert->crl);

	return cert;

 done:
	cert_free(cert);
	return NULL;
}

/*
 * Build the certificate chain by using the Authority Information Access.
 * This requires that the TA are already validated and added to the auths
 * tree. Once the TA is located in the chain the chain is validated in
 * reverse order.
 */
static struct auth *
parse_load_certchain(char *uri)
{
	struct cert *stack[MAX_CERT_DEPTH] = { 0 };
	char *filestack[MAX_CERT_DEPTH];
	struct cert *cert;
	struct crl *crl;
	struct auth *a;
	const char *errstr;
	int i;

	for (i = 0; i < MAX_CERT_DEPTH; i++) {
		if ((cert = uripath_lookup(uri)) != NULL) {
			a = auth_find(&auths, cert->certid);
			if (a == NULL) {
				warnx("failed to find issuer for %s", uri);
				goto fail;
			}
			break;
		}
		filestack[i] = uri;
		stack[i] = cert = parse_load_cert(uri);
		if (cert == NULL || cert->purpose != CERT_PURPOSE_CA) {
			warnx("failed to build authority chain: %s", uri);
			goto fail;
		}
		uri = cert->aia;
	}

	if (i >= MAX_CERT_DEPTH) {
		warnx("authority chain exceeds max depth of %d",
		    MAX_CERT_DEPTH);
		goto fail;
	}

	/* TA found play back the stack and add all certs */
	for (; i > 0; i--) {
		cert = stack[i - 1];
		uri = filestack[i - 1];

		crl = crl_get(&crlt, a);
		if (!valid_x509(uri, ctx, cert->x509, a, crl, &errstr) ||
		    !valid_cert(uri, a, cert)) {
			if (errstr != NULL)
				warnx("%s: %s", uri, errstr);
			goto fail;
		}
		cert->talid = a->cert->talid;
		a = auth_insert(uri, &auths, cert, a);
		uripath_add(uri, cert);
		stack[i] = NULL;
	}

	return a;
fail:
	for (i = 0; i < MAX_CERT_DEPTH; i++)
		cert_free(stack[i]);
	return NULL;
}

static void
parse_load_ta(struct tal *tal)
{
	const char *filename;
	struct cert *cert;
	unsigned char *f = NULL;
	char *file;
	size_t flen, i;

	/* does not matter which URI, all end with same filename */
	filename = strrchr(tal->uri[0], '/');
	assert(filename);

	if (asprintf(&file, "ta/%s%s", tal->descr, filename) == -1)
		err(1, NULL);

	f = load_file(file, &flen);
	if (f == NULL) {
		warn("parse file %s", file);
		goto out;
	}

	/* Extract certificate data. */
	cert = cert_parse_pre(file, f, flen);
	cert = ta_parse(file, cert, tal->pkey, tal->pkeysz);
	if (cert == NULL)
		goto out;

	cert->talid = tal->id;
	auth_insert(file, &auths, cert, NULL);
	for (i = 0; i < tal->urisz; i++) {
		if (strncasecmp(tal->uri[i], RSYNC_PROTO, RSYNC_PROTO_LEN) != 0)
			continue;
		/* Add all rsync uri since any of them could be used as AIA. */
		uripath_add(tal->uri[i], cert);
	}

out:
	free(file);
	free(f);
}

static struct tal *
find_tal(struct cert *cert)
{
	EVP_PKEY	*pk, *opk;
	struct tal	*tal;
	int		 i;

	if ((opk = X509_get0_pubkey(cert->x509)) == NULL)
		return NULL;

	for (i = 0; i < TALSZ_MAX; i++) {
		const unsigned char *pkey;

		if (talobj[i] == NULL)
			break;
		tal = talobj[i];
		pkey = tal->pkey;
		pk = d2i_PUBKEY(NULL, &pkey, tal->pkeysz);
		if (pk == NULL)
			continue;
		if (EVP_PKEY_cmp(pk, opk) == 1) {
			EVP_PKEY_free(pk);
			return tal;
		}
		EVP_PKEY_free(pk);
	}
	return NULL;
}

static void
print_signature_path(const char *crl, const char *aia, const struct auth *a)
{
	if (crl != NULL)
		printf("Signature path:           %s\n", crl);
	if (a->cert->mft != NULL)
		printf("                          %s\n", a->cert->mft);
	if (aia != NULL)
		printf("                          %s\n", aia);

	for (; a != NULL; a = a->issuer) {
		if (a->cert->crl != NULL)
			printf("                          %s\n", a->cert->crl);
		if (a->issuer != NULL && a->issuer->cert != NULL &&
		    a->issuer->cert->mft != NULL)
			printf("                          %s\n",
			    a->issuer->cert->mft);
		if (a->cert->aia != NULL)
			printf("                          %s\n", a->cert->aia);
	}
}

/*
 * Parse file passed with -f option.
 */
static void
proc_parser_file(char *file, unsigned char *buf, size_t len)
{
	static int num;
	X509 *x509 = NULL;
	struct aspa *aspa = NULL;
	struct cert *cert = NULL;
	struct crl *crl = NULL;
	struct gbr *gbr = NULL;
	struct geofeed *geofeed = NULL;
	struct mft *mft = NULL;
	struct roa *roa = NULL;
	struct rsc *rsc = NULL;
	struct spl *spl = NULL;
	struct tak *tak = NULL;
	struct tal *tal = NULL;
	char *aia = NULL;
	char *crl_uri = NULL;
	time_t *expires = NULL, *notafter = NULL;
	struct auth *a;
	struct crl *c;
	const char *errstr = NULL, *valid;
	int status = 0;
	char filehash[SHA256_DIGEST_LENGTH];
	char *hash;
	enum rtype type;
	int is_ta = 0;

	if (outformats & FORMAT_JSON) {
		json_do_start(stdout);
	} else {
		if (num++ > 0)
			printf("--\n");
	}

	if (strncmp(file, RSYNC_PROTO, RSYNC_PROTO_LEN) == 0) {
		file += RSYNC_PROTO_LEN;
		buf = load_file(file, &len);
		if (buf == NULL) {
			warn("parse file %s", file);
			return;
		}
	}

	if (!EVP_Digest(buf, len, filehash, NULL, EVP_sha256(), NULL))
		errx(1, "EVP_Digest failed in %s", __func__);

	if (base64_encode(filehash, sizeof(filehash), &hash) == -1)
		errx(1, "base64_encode failed in %s", __func__);

	if (outformats & FORMAT_JSON) {
		json_do_string("file", file);
		json_do_string("hash_id", hash);
	} else {
		printf("File:                     %s\n", file);
		printf("Hash identifier:          %s\n", hash);
	}

	free(hash);

	type = rtype_from_file_extension(file);

	switch (type) {
	case RTYPE_ASPA:
		aspa = aspa_parse(&x509, file, -1, buf, len);
		if (aspa == NULL)
			break;
		aia = aspa->aia;
		expires = &aspa->expires;
		notafter = &aspa->notafter;
		break;
	case RTYPE_CER:
		cert = cert_parse_pre(file, buf, len);
		if (cert == NULL)
			break;
		is_ta = (cert->purpose == CERT_PURPOSE_TA);
		if (!is_ta)
			cert = cert_parse(file, cert);
		if (cert == NULL)
			break;
		aia = cert->aia;
		x509 = cert->x509;
		if (X509_up_ref(x509) == 0)
			errx(1, "%s: X509_up_ref failed", __func__);
		expires = &cert->expires;
		notafter = &cert->notafter;
		break;
	case RTYPE_CRL:
		crl = crl_parse(file, buf, len);
		if (crl == NULL)
			break;
		crl_print(crl);
		break;
	case RTYPE_MFT:
		mft = mft_parse(&x509, file, -1, buf, len);
		if (mft == NULL)
			break;
		aia = mft->aia;
		expires = &mft->expires;
		notafter = &mft->nextupdate;
		break;
	case RTYPE_GBR:
		gbr = gbr_parse(&x509, file, -1, buf, len);
		if (gbr == NULL)
			break;
		aia = gbr->aia;
		expires = &gbr->expires;
		notafter = &gbr->notafter;
		break;
	case RTYPE_GEOFEED:
		geofeed = geofeed_parse(&x509, file, -1, buf, len);
		if (geofeed == NULL)
			break;
		aia = geofeed->aia;
		expires = &geofeed->expires;
		notafter = &geofeed->notafter;
		break;
	case RTYPE_ROA:
		roa = roa_parse(&x509, file, -1, buf, len);
		if (roa == NULL)
			break;
		aia = roa->aia;
		expires = &roa->expires;
		notafter = &roa->notafter;
		break;
	case RTYPE_RSC:
		rsc = rsc_parse(&x509, file, -1, buf, len);
		if (rsc == NULL)
			break;
		aia = rsc->aia;
		expires = &rsc->expires;
		notafter = &rsc->notafter;
		break;
	case RTYPE_SPL:
		spl = spl_parse(&x509, file, -1, buf, len);
		if (spl == NULL)
			break;
		aia = spl->aia;
		expires = &spl->expires;
		notafter = &spl->notafter;
		break;
	case RTYPE_TAK:
		tak = tak_parse(&x509, file, -1, buf, len);
		if (tak == NULL)
			break;
		aia = tak->aia;
		expires = &tak->expires;
		notafter = &tak->notafter;
		break;
	case RTYPE_TAL:
		tal = tal_parse(file, buf, len);
		if (tal == NULL)
			break;
		tal_print(tal);
		break;
	default:
		printf("%s: unsupported file type\n", file);
		break;
	}

	if (aia != NULL) {
		x509_get_crl(x509, file, &crl_uri);
		parse_load_crl(crl_uri);
		a = parse_load_certchain(aia);
		c = crl_get(&crlt, a);

		if ((status = valid_x509(file, ctx, x509, a, c, &errstr))) {
			switch (type) {
			case RTYPE_ASPA:
				status = aspa->valid;
				break;
			case RTYPE_GEOFEED:
				status = geofeed->valid;
				break;
			case RTYPE_ROA:
				status = roa->valid;
				break;
			case RTYPE_RSC:
				status = rsc->valid;
				break;
			case RTYPE_SPL:
				status = spl->valid;
			default:
				break;
			}
		}
		if (status && cert == NULL) {
			struct cert *eecert;

			eecert = cert_parse_ee_cert(file, a->cert->talid, x509);
			if (eecert == NULL)
				status = 0;
			cert_free(eecert);
		} else if (status) {
			cert->talid = a->cert->talid;
			constraints_validate(file, cert);
		}
	} else if (is_ta) {
		expires = NULL;
		notafter = NULL;
		if ((tal = find_tal(cert)) != NULL) {
			cert = ta_parse(file, cert, tal->pkey, tal->pkeysz);
			status = (cert != NULL);
			if (status) {
				expires = &cert->expires;
				notafter = &cert->notafter;
			}
			if (outformats & FORMAT_JSON)
				json_do_string("tal", tal->descr);
			else
				printf("TAL:                      %s\n",
				    tal->descr);
			tal = NULL;
		} else {
			cert_free(cert);
			cert = NULL;
			status = 0;
		}
	}

	if (expires != NULL) {
		if (status && aia != NULL)
			*expires = x509_find_expires(*notafter, a, &crlt);

		switch (type) {
		case RTYPE_ASPA:
			aspa_print(x509, aspa);
			break;
		case RTYPE_CER:
			cert_print(cert);
			break;
		case RTYPE_GBR:
			gbr_print(x509, gbr);
			break;
		case RTYPE_GEOFEED:
			geofeed_print(x509, geofeed);
			break;
		case RTYPE_MFT:
			mft_print(x509, mft);
			break;
		case RTYPE_ROA:
			roa_print(x509, roa);
			break;
		case RTYPE_RSC:
			rsc_print(x509, rsc);
			break;
		case RTYPE_SPL:
			spl_print(x509, spl);
			break;
		case RTYPE_TAK:
			tak_print(x509, tak);
			break;
		default:
			break;
		}
	}

	if (status)
		valid = "OK";
	else if (aia == NULL)
		valid = "N/A";
	else
		valid = "Failed";

	if (outformats & FORMAT_JSON) {
		json_do_string("validation", valid);
		if (errstr != NULL)
			json_do_string("error", errstr);
	} else {
		printf("Validation:               %s", valid);
		if (errstr != NULL)
			printf(", %s", errstr);
	}

	if (outformats & FORMAT_JSON)
		json_do_finish();
	else {
		printf("\n");

		if (status && aia != NULL) {
			print_signature_path(crl_uri, aia, a);
			if (expires != NULL)
				printf("Signature path expires:   %s\n",
				    time2str(*expires));
		}

		if (x509 == NULL)
			goto out;
		if (type == RTYPE_TAL || type == RTYPE_CRL)
			goto out;

		if (verbose) {
			if (!X509_print_fp(stdout, x509))
				errx(1, "X509_print_fp");
		}

		if (verbose > 1) {
			if (!PEM_write_X509(stdout, x509))
				errx(1, "PEM_write_X509");
		}
	}

 out:
	free(crl_uri);
	X509_free(x509);
	aspa_free(aspa);
	cert_free(cert);
	crl_free(crl);
	gbr_free(gbr);
	geofeed_free(geofeed);
	mft_free(mft);
	roa_free(roa);
	rsc_free(rsc);
	tak_free(tak);
	tal_free(tal);
}

/*
 * Process a file request, in general don't send anything back.
 */
static void
parse_file(struct entityq *q, struct msgbuf *msgq)
{
	struct entity	*entp;
	struct ibuf	*b;
	struct tal	*tal;
	time_t		 dummy = 0;

	while ((entp = TAILQ_FIRST(q)) != NULL) {
		TAILQ_REMOVE(q, entp, entries);

		switch (entp->type) {
		case RTYPE_FILE:
			proc_parser_file(entp->file, entp->data, entp->datasz);
			break;
		case RTYPE_TAL:
			if ((tal = tal_parse(entp->file, entp->data,
			    entp->datasz)) == NULL)
				errx(1, "%s: could not parse tal file",
				    entp->file);
			tal->id = entp->talid;
			talobj[tal->id] = tal;
			parse_load_ta(tal);
			break;
		default:
			errx(1, "unhandled entity type %d", entp->type);
		}

		b = io_new_buffer();
		io_simple_buffer(b, &entp->type, sizeof(entp->type));
		io_simple_buffer(b, &entp->repoid, sizeof(entp->repoid));
		io_simple_buffer(b, &entp->talid, sizeof(entp->talid));
		io_str_buffer(b, entp->file);
		io_simple_buffer(b, &dummy, sizeof(dummy));
		io_close_buffer(msgq, b);
		entity_free(entp);
	}
}

/*
 * Process responsible for parsing and validating content.
 * All this process does is wait to be told about a file to parse, then
 * it parses it and makes sure that the data being returned is fully
 * validated and verified.
 * The process will exit cleanly only when fd is closed.
 */
void
proc_filemode(int fd)
{
	struct entityq	 q;
	struct msgbuf	 msgq;
	struct pollfd	 pfd;
	struct entity	*entp;
	struct ibuf	*b, *inbuf = NULL;

	/* Only allow access to the cache directory. */
	if (unveil(".", "r") == -1)
		err(1, "unveil cachedir");
	if (pledge("stdio rpath", NULL) == -1)
		err(1, "pledge");

	ERR_load_crypto_strings();
	OpenSSL_add_all_ciphers();
	OpenSSL_add_all_digests();
	x509_init_oid();
	constraints_parse();

	if ((ctx = X509_STORE_CTX_new()) == NULL)
		err(1, "X509_STORE_CTX_new");
	TAILQ_INIT(&q);

	msgbuf_init(&msgq);
	msgq.fd = fd;

	pfd.fd = fd;

	for (;;) {
		pfd.events = POLLIN;
		if (msgq.queued)
			pfd.events |= POLLOUT;

		if (poll(&pfd, 1, INFTIM) == -1) {
			if (errno == EINTR)
				continue;
			err(1, "poll");
		}
		if ((pfd.revents & (POLLERR|POLLNVAL)))
			errx(1, "poll: bad descriptor");

		/* If the parent closes, return immediately. */

		if ((pfd.revents & POLLHUP))
			break;

		if ((pfd.revents & POLLIN)) {
			b = io_buf_read(fd, &inbuf);
			if (b != NULL) {
				entp = calloc(1, sizeof(struct entity));
				if (entp == NULL)
					err(1, NULL);
				entity_read_req(b, entp);
				TAILQ_INSERT_TAIL(&q, entp, entries);
				ibuf_free(b);
			}
		}

		if (pfd.revents & POLLOUT) {
			switch (msgbuf_write(&msgq)) {
			case 0:
				errx(1, "write: connection closed");
			case -1:
				err(1, "write");
			}
		}

		parse_file(&q, &msgq);
	}

	msgbuf_clear(&msgq);
	while ((entp = TAILQ_FIRST(&q)) != NULL) {
		TAILQ_REMOVE(&q, entp, entries);
		entity_free(entp);
	}

	auth_tree_free(&auths);
	crl_tree_free(&crlt);

	X509_STORE_CTX_free(ctx);
	ibuf_free(inbuf);

	exit(0);
}
