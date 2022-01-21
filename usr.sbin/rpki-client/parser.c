/*	$OpenBSD: parser.c,v 1.49 2022/01/21 18:49:44 tb Exp $ */
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
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "extern.h"

static void		 build_chain(const struct auth *, STACK_OF(X509) **);
static struct crl	*get_crl(const struct auth *);
static void		 build_crls(const struct crl *, STACK_OF(X509_CRL) **);

static X509_STORE_CTX	*ctx;
static struct auth_tree  auths = RB_INITIALIZER(&auths);
static struct crl_tree	 crlt = RB_INITIALIZER(&crlt);

struct parse_repo {
	RB_ENTRY(parse_repo)	 entry;
	char			*path;
	char			*validpath;
	unsigned int		 id;
};

static RB_HEAD(repo_tree, parse_repo)	repos = RB_INITIALIZER(&repos);

static inline int
repocmp(struct parse_repo *a, struct parse_repo *b)
{
	return a->id - b->id;
}

RB_GENERATE_STATIC(repo_tree, parse_repo, entry, repocmp);

static struct parse_repo *
repo_get(unsigned int id)
{
	struct parse_repo needle = { .id = id };

	return RB_FIND(repo_tree, &repos, &needle);
}

static void
repo_add(unsigned int id, char *path, char *validpath)
{
	struct parse_repo *rp;

	if ((rp = calloc(1, sizeof(*rp))) == NULL)
		err(1, NULL);
	rp->id = id;
	if (path != NULL)
		if ((rp->path = strdup(path)) == NULL)
			err(1, NULL);
	if (validpath != NULL)
		if ((rp->validpath = strdup(validpath)) == NULL)
			err(1, NULL);

	if (RB_INSERT(repo_tree, &repos, rp) != NULL)
		errx(1, "repository already added: id %d, %s", id, path);
}

/*
 * Build access path to file based on repoid, path and file values.
 * If wantalt == 1 the function can return NULL, if wantalt == 0 it
 * can not fail.
 */
static char *
parse_filepath(unsigned int repoid, const char *path, const char *file,
    int wantalt)
{
	struct parse_repo	*rp;
	char			*fn, *repopath;

	/* build file path based on repoid, entity path and filename */
	rp = repo_get(repoid);
	if (rp == NULL) {
		/* no repo so no alternative path. */
		if (wantalt)
			return NULL;

		if (path == NULL) {
			if ((fn = strdup(file)) == NULL)
				err(1, NULL);
		} else {
			if (asprintf(&fn, "%s/%s", path, file) == -1)
				err(1, NULL);
		}
	} else {
		if (wantalt || rp->path == NULL)
			repopath = rp->validpath;
		else
			repopath = rp->path;

		if (repopath == NULL)
			return NULL;

		if (path == NULL) {
			if (asprintf(&fn, "%s/%s", repopath, file) == -1)
				err(1, NULL);
		} else {
			if (asprintf(&fn, "%s/%s/%s", repopath, path,
			    file) == -1)
				err(1, NULL);
		}
	}
	return fn;
}

/*
 * Callback for X509_verify_cert() to handle critical extensions in old
 * LibreSSL libraries or OpenSSL libs without RFC3779 support.
 */
static int
verify_cb(int ok, X509_STORE_CTX *store_ctx)
{
	X509				*cert;
	const STACK_OF(X509_EXTENSION)	*exts;
	X509_EXTENSION			*ext;
	ASN1_OBJECT			*obj;
	char				*file;
	int				 depth, error, i, nid;

	error = X509_STORE_CTX_get_error(store_ctx);
	depth = X509_STORE_CTX_get_error_depth(store_ctx);

	if (error != X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION)
		return ok;

	if ((file = X509_STORE_CTX_get_app_data(store_ctx)) == NULL)
		cryptoerrx("X509_STORE_CTX_get_app_data");

	if ((cert = X509_STORE_CTX_get_current_cert(store_ctx)) == NULL) {
		warnx("%s: got no current cert", file);
		return 0;
	}
	if ((exts = X509_get0_extensions(cert)) == NULL) {
		warnx("%s: got no cert extensions", file);
		return 0;
	}

	for (i = 0; i < sk_X509_EXTENSION_num(exts); i++) {
		ext = sk_X509_EXTENSION_value(exts, i);

		/* skip over non-critical and known extensions */
		if (!X509_EXTENSION_get_critical(ext))
			continue;
		if (X509_supported_extension(ext))
			continue;

		if ((obj = X509_EXTENSION_get_object(ext)) == NULL) {
			warnx("%s: got no extension object", file);
			return 0;
		}

		nid = OBJ_obj2nid(obj);
		switch (nid) {
		case NID_sbgp_ipAddrBlock:
		case NID_sbgp_autonomousSysNum:
			continue;
		default:
			warnx("%s: depth %d: unknown extension: nid %d",
			    file, depth, nid);
			return 0;
		}
	}

	return 1;
}

/*
 * Validate the X509 certificate.  If crl is NULL don't check CRL.
 * Returns 1 for valid certificates, returns 0 if there is a verify error
 */
static int
valid_x509(char *file, X509 *x509, struct auth *a, struct crl *crl,
    unsigned long flags)
{
	STACK_OF(X509)		*chain;
	STACK_OF(X509_CRL)	*crls = NULL;
	int			 c;

	build_chain(a, &chain);
	build_crls(crl, &crls);

	assert(x509 != NULL);
	if (!X509_STORE_CTX_init(ctx, NULL, x509, NULL))
		cryptoerrx("X509_STORE_CTX_init");

	X509_STORE_CTX_set_verify_cb(ctx, verify_cb);
	if (!X509_STORE_CTX_set_app_data(ctx, file))
		cryptoerrx("X509_STORE_CTX_set_app_data");
	if (flags != 0)
		X509_STORE_CTX_set_flags(ctx, flags);
	X509_STORE_CTX_set_depth(ctx, MAX_CERT_DEPTH);
	X509_STORE_CTX_set0_trusted_stack(ctx, chain);
	X509_STORE_CTX_set0_crls(ctx, crls);

	if (X509_verify_cert(ctx) <= 0) {
		c = X509_STORE_CTX_get_error(ctx);
		warnx("%s: %s", file, X509_verify_cert_error_string(c));
		X509_STORE_CTX_cleanup(ctx);
		sk_X509_free(chain);
		sk_X509_CRL_free(crls);
		return 0;
	}

	X509_STORE_CTX_cleanup(ctx);
	sk_X509_free(chain);
	sk_X509_CRL_free(crls);
	return 1;
}

/*
 * Parse and validate a ROA.
 * This is standard stuff.
 * Returns the roa on success, NULL on failure.
 */
static struct roa *
proc_parser_roa(char *file, const unsigned char *der, size_t len)
{
	struct roa		*roa;
	struct crl		*crl;
	struct auth		*a;
	X509			*x509;

	if ((roa = roa_parse(&x509, file, der, len)) == NULL)
		return NULL;

	a = valid_ski_aki(file, &auths, roa->ski, roa->aki);
	crl = get_crl(a);

	if (!valid_x509(file, x509, a, crl, X509_V_FLAG_CRL_CHECK)) {
		X509_free(x509);
		roa_free(roa);
		return NULL;
	}
	X509_free(x509);

	/*
	 * Check CRL to figure out the soonest transitive expiry moment
	 */
	if (crl != NULL && roa->expires > crl->expires)
		roa->expires = crl->expires;

	/*
	 * Scan the cert tree to figure out the soonest transitive
	 * expiry moment
	 */
	for (; a != NULL; a = a->parent) {
		if (roa->expires > a->cert->expires)
			roa->expires = a->cert->expires;
	}

	/*
	 * If the ROA isn't valid, we accept it anyway and depend upon
	 * the code around roa_read() to check the "valid" field itself.
	 */

	if (valid_roa(file, &auths, roa))
		roa->valid = 1;

	return roa;
}

/*
 * Check all files and their hashes in a MFT structure.
 * Return zero on failure, non-zero on success.
 */
static int
proc_parser_mft_check(const char *fn, struct mft *p)
{
	size_t	i;
	int	fd, try, rc = 1;
	char	*h, *path;

	for (i = 0; i < p->filesz; i++) {
		const struct mftfile *m = &p->files[i];
		if (!valid_filename(m->file)) {
			if (base64_encode(m->hash, sizeof(m->hash), &h) == -1)
				errx(1, "base64_encode failed in %s", __func__);
			warnx("%s: unsupported filename for %s", fn, h);
			free(h);
			continue;
		}

		fd = -1;
		try = 0;
		path = NULL;
		do {
			free(path);
			if ((path = parse_filepath(p->repoid, p->path, m->file,
			    try++)) == NULL)
				break;
			fd = open(path, O_RDONLY);
		} while (fd == -1 && try < 2);

		free(path);

		if (!valid_filehash(fd, m->hash, sizeof(m->hash))) {
			warnx("%s: bad message digest for %s", fn, m->file);
			rc = 0;
		}
	}

	return rc;
}

/*
 * Parse and validate a manifest file.
 * Here we *don't* validate against the list of CRLs, because the
 * certificate used to sign the manifest may specify a CRL that the root
 * certificate didn't, and we haven't scanned for it yet.
 * This chicken-and-egg isn't important, however, because we'll catch
 * the revocation list by the time we scan for any contained resources
 * (ROA, CER) and will see it then.
 * Return the mft on success or NULL on failure.
 */
static struct mft *
proc_parser_mft(char *file, const unsigned char *der, size_t len,
    const char *path, unsigned int repoid)
{
	struct mft		*mft;
	X509			*x509;
	struct auth		*a;

	if ((mft = mft_parse(&x509, file, der, len)) == NULL)
		return NULL;

	a = valid_ski_aki(file, &auths, mft->ski, mft->aki);

	/* CRL checks disabled here because CRL is referenced from mft */
	if (!valid_x509(file, x509, a, NULL, 0)) {
		mft_free(mft);
		X509_free(x509);
		return NULL;
	}
	X509_free(x509);

	mft->repoid = repoid;
	if (path != NULL)
		if ((mft->path = strdup(path)) == NULL)
			err(1, NULL);

	if (!mft->stale)
		if (!proc_parser_mft_check(file, mft)) {
			mft_free(mft);
			return NULL;
		}

	return mft;
}

/*
 * Validate a certificate, if invalid free the resouces and return NULL.
 */
static struct cert *
proc_parser_cert_validate(char *file, struct cert *cert)
{
	struct auth	*a;
	struct crl	*crl;

	a = valid_ski_aki(file, &auths, cert->ski, cert->aki);
	crl = get_crl(a);

	if (!valid_x509(file, cert->x509, a, crl, X509_V_FLAG_CRL_CHECK)) {
		cert_free(cert);
		return NULL;
	}

	cert->talid = a->cert->talid;

	/* Validate the cert to get the parent */
	if (!valid_cert(file, &auths, cert)) {
		cert_free(cert);
		return NULL;
	}

	/*
	 * Add validated CA certs to the RPKI auth tree.
	 */
	if (cert->purpose == CERT_PURPOSE_CA)
		auth_insert(&auths, cert, a);

	return cert;
}

/*
 * Certificates are from manifests (has a digest and is signed with
 * another certificate) Parse the certificate, make sure its
 * signatures are valid (with CRLs), then validate the RPKI content.
 * This returns a certificate (which must not be freed) or NULL on
 * parse failure.
 */
static struct cert *
proc_parser_cert(char *file, const unsigned char *der, size_t len)
{
	struct cert	*cert;

	/* Extract certificate data. */

	cert = cert_parse(file, der, len);
	if (cert == NULL)
		return NULL;

	cert = proc_parser_cert_validate(file, cert);
	return cert;
}

/*
 * Root certificates come from TALs (has a pkey and is self-signed).
 * Parse the certificate, ensure that it's public key matches the
 * known public key from the TAL, and then validate the RPKI
 * content.
 *
 * This returns a certificate (which must not be freed) or NULL on
 * parse failure.
 */
static struct cert *
proc_parser_root_cert(char *file, const unsigned char *der, size_t len,
    unsigned char *pkey, size_t pkeysz, int talid)
{
	struct cert		*cert;

	/* Extract certificate data. */

	cert = ta_parse(file, der, len, pkey, pkeysz);
	if (cert == NULL)
		return NULL;

	if (!valid_ta(file, &auths, cert)) {
		warnx("%s: certificate not a valid ta", file);
		cert_free(cert);
		return NULL;
	}

	cert->talid = talid;

	/*
	 * Add valid roots to the RPKI auth tree.
	 */
	auth_insert(&auths, cert, NULL);

	return cert;
}

/*
 * Parse a certificate revocation list
 * This simply parses the CRL content itself, optionally validating it
 * within the digest if it comes from a manifest, then adds it to the
 * CRL tree.
 */
static void
proc_parser_crl(char *file, const unsigned char *der, size_t len)
{
	X509_CRL		*x509_crl;
	struct crl		*crl;
	const ASN1_TIME		*at;
	struct tm		 expires_tm;

	if ((x509_crl = crl_parse(file, der, len)) != NULL) {
		if ((crl = malloc(sizeof(*crl))) == NULL)
			err(1, NULL);
		if ((crl->aki = x509_crl_get_aki(x509_crl, file)) ==
		    NULL) {
			warnx("x509_crl_get_aki failed");
			goto err;
		}

		crl->x509_crl = x509_crl;

		/* extract expire time for later use */
		at = X509_CRL_get0_nextUpdate(x509_crl);
		if (at == NULL) {
			warnx("%s: X509_CRL_get0_nextUpdate failed", file);
			goto err;
		}
		memset(&expires_tm, 0, sizeof(expires_tm));
		if (ASN1_time_parse(at->data, at->length, &expires_tm,
		    0) == -1) {
			warnx("%s: ASN1_time_parse failed", file);
			goto err;
		}
		if ((crl->expires = mktime(&expires_tm)) == -1)
			errx(1, "%s: mktime failed", file);

		if (RB_INSERT(crl_tree, &crlt, crl) != NULL) {
			warnx("%s: duplicate AKI %s", file, crl->aki);
			goto err;
		}
	}
	return;
 err:
	free_crl(crl);
}

/*
 * Parse a ghostbuster record
 */
static void
proc_parser_gbr(char *file, const unsigned char *der, size_t len)
{
	struct gbr		*gbr;
	X509			*x509;
	struct auth		*a;
	struct crl		*crl;

	if ((gbr = gbr_parse(&x509, file, der, len)) == NULL)
		return;

	a = valid_ski_aki(file, &auths, gbr->ski, gbr->aki);
	crl = get_crl(a);

	/* return value can be ignored since nothing happens here */
	valid_x509(file, x509, a, crl, X509_V_FLAG_CRL_CHECK);

	X509_free(x509);
	gbr_free(gbr);
}

/*
 * Walk the certificate tree to the root and build a certificate
 * chain from cert->x509. All certs in the tree are validated and
 * can be loaded as trusted stack into the validator.
 */
static void
build_chain(const struct auth *a, STACK_OF(X509) **chain)
{
	*chain = NULL;

	if (a == NULL)
		return;

	if ((*chain = sk_X509_new_null()) == NULL)
		err(1, "sk_X509_new_null");
	for (; a != NULL; a = a->parent) {
		assert(a->cert->x509 != NULL);
		if (!sk_X509_push(*chain, a->cert->x509))
			errx(1, "sk_X509_push");
	}
}

/*
 * Find a CRL based on the auth SKI value.
 */
static struct crl *
get_crl(const struct auth *a)
{
	struct crl	find;

	if (a == NULL)
		return NULL;

	find.aki = a->cert->ski;
	return RB_FIND(crl_tree, &crlt, &find);
}

/*
 * Add the CRL based on the certs SKI value.
 * No need to insert any other CRL since those were already checked.
 */
static void
build_crls(const struct crl *crl, STACK_OF(X509_CRL) **crls)
{
	*crls = NULL;

	if (crl == NULL)
		return;

	if ((*crls = sk_X509_CRL_new_null()) == NULL)
		errx(1, "sk_X509_CRL_new_null");

	if (!sk_X509_CRL_push(*crls, crl->x509_crl))
		err(1, "sk_X509_CRL_push");
}

static char *
parse_load_file(struct entity *entp, unsigned char **f, size_t *flen)
{
	char *file, *nfile;

	file = parse_filepath(entp->repoid, entp->path, entp->file, 0);

	/* TAL files include the data already */
	if (entp->type == RTYPE_TAL) {
		*f = NULL;
		*flen = 0;
		return file;
	}

	*f = load_file(file, flen);
	if (*f != NULL)
		return file;

	if (errno != ENOENT)
		goto fail;

	/* try alternate file location */
	nfile = parse_filepath(entp->repoid, entp->path, entp->file, 1);
	if (nfile == NULL)
		goto fail;

	free(file);
	file = nfile;

	*f = load_file(file, flen);
	if (*f != NULL)
		return file;

fail:
	warn("parse file %s", file);
	return file;
}

/*
 * Process an entity and responing to parent process.
 */
static void
parse_entity(struct entityq *q, struct msgbuf *msgq)
{
	struct entity	*entp;
	struct tal	*tal;
	struct cert	*cert;
	struct mft	*mft;
	struct roa	*roa;
	struct ibuf	*b;
	unsigned char	*f;
	size_t		 flen;
	char		*file;
	int		 c;

	while ((entp = TAILQ_FIRST(q)) != NULL) {
		TAILQ_REMOVE(q, entp, entries);

		/* handle RTYPE_REPO first */
		if (entp->type == RTYPE_REPO) {
			repo_add(entp->repoid, entp->path, entp->file);
			entity_free(entp);
			continue;
		}

		file = parse_load_file(entp, &f, &flen);

		/* pass back at least type, repoid and filename */
		b = io_new_buffer();
		io_simple_buffer(b, &entp->type, sizeof(entp->type));
		io_str_buffer(b, file);

		switch (entp->type) {
		case RTYPE_TAL:
			if ((tal = tal_parse(entp->file, entp->data,
			    entp->datasz)) == NULL)
				errx(1, "%s: could not parse tal file",
				    entp->file);
			tal->id = entp->talid;
			tal_buffer(b, tal);
			tal_free(tal);
			break;
		case RTYPE_CER:
			if (entp->data != NULL)
				cert = proc_parser_root_cert(file,
				    f, flen, entp->data, entp->datasz,
				    entp->talid);
			else
				cert = proc_parser_cert(file, f, flen);
			c = (cert != NULL);
			io_simple_buffer(b, &c, sizeof(int));
			if (cert != NULL)
				cert_buffer(b, cert);
			/*
			 * The parsed certificate data "cert" is now
			 * managed in the "auths" table, so don't free
			 * it here (see the loop after "out").
			 */
			break;
		case RTYPE_CRL:
			proc_parser_crl(file, f, flen);
			break;
		case RTYPE_MFT:
			mft = proc_parser_mft(file, f, flen,
			    entp->path, entp->repoid);
			c = (mft != NULL);
			io_simple_buffer(b, &c, sizeof(int));
			if (mft != NULL)
				mft_buffer(b, mft);
			mft_free(mft);
			break;
		case RTYPE_ROA:
			roa = proc_parser_roa(file, f, flen);
			c = (roa != NULL);
			io_simple_buffer(b, &c, sizeof(int));
			if (roa != NULL)
				roa_buffer(b, roa);
			roa_free(roa);
			break;
		case RTYPE_GBR:
			proc_parser_gbr(file, f, flen);
			break;
		default:
			errx(1, "unhandled entity type %d", entp->type);
		}

		free(f);
		free(file);
		io_close_buffer(msgq, b);
		entity_free(entp);
	}
}

/*
 * Use the X509 CRL Distribution Points to locate the CRL needed for
 * verification.
 */
static void
parse_load_crl(const char *uri)
{
	char *nfile, *f;
	size_t flen;

	if (uri == NULL)
		return;
	if (strncmp(uri, "rsync://", strlen("rsync://")) != 0) {
		warnx("bad CRL distribution point URI %s", uri);
		return;
	}
	uri += strlen("rsync://");

	if (asprintf(&nfile, "valid/%s", uri) == -1)
		err(1, NULL);

	f = load_file(nfile, &flen);
	if (f == NULL) {
		warn("parse file %s", nfile);
		goto done;
	}

	proc_parser_crl(nfile, f, flen);

done:
	free(nfile);
	free(f);
}

/*
 * Parse the cert pointed at by the AIA URI while doing that also load
 * the CRL of this cert. While the CRL is validated the returned cert
 * is not. The caller needs to make sure it is validated once all
 * necessary certs were loaded. Returns NULL on failure.
 */
static struct cert *
parse_load_cert(const char *uri)
{
	struct cert *cert = NULL;
	char *nfile, *f;
	size_t flen;

	if (uri == NULL)
		return NULL;

	if (strncmp(uri, "rsync://", strlen("rsync://")) != 0) {
		warnx("bad authority information access URI %s", uri);
		return NULL;
	}
	uri += strlen("rsync://");

	if (asprintf(&nfile, "valid/%s", uri) == -1)
		err(1, NULL);

	f = load_file(nfile, &flen);
	if (f == NULL) {
		warn("parse file %s", nfile);
		goto done;
	}

	cert = cert_parse(nfile, f, flen);
	free(f);

	if (cert == NULL)
		goto done;
	if (cert->purpose != CERT_PURPOSE_CA) {
		warnx("AIA reference to bgpsec cert %s", nfile);
		goto done;
	}
	/* try to load the CRL of this cert */
	parse_load_crl(cert->crl);

	free(nfile);
	return cert;

done:
	cert_free(cert);
	free(nfile);
	return NULL;
}

/*
 * Build the certificate chain by using the Authority Information Access.
 * This requires that the TA are already validated and added to the auths
 * tree. Once the TA is located in the chain the chain is validated in
 * reverse order.
 */
static void
parse_load_certchain(char *uri)
{
	struct cert *stack[MAX_CERT_DEPTH];
	char *filestack[MAX_CERT_DEPTH];
	struct cert *cert;
	int i, failed;

	for (i = 0; i < MAX_CERT_DEPTH; i++) {
		cert = parse_load_cert(uri);
		if (cert == NULL) {
			warnx("failed to build authority chain");
			return;
		}
		stack[i] = cert;
		filestack[i] = uri;
		if (auth_find(&auths, cert->aki) != NULL)
			break;	/* found the TA */
		uri = cert->aia;
	}

	if (i >= MAX_CERT_DEPTH) {
		warnx("authority chain exceeds max depth of %d",
		    MAX_CERT_DEPTH);
		for (i = 0; i < MAX_CERT_DEPTH; i++)
			cert_free(stack[i]);
		return;
	}

	/* TA found play back the stack and add all certs */
	for (failed = 0; i >= 0; i--) {
		cert = stack[i];
		uri = filestack[i];

		if (failed)
			cert_free(cert);
		else if (proc_parser_cert_validate(uri, cert) == NULL)
			failed = 1;
	}
}

static void
parse_load_ta(struct tal *tal)
{
	const char *file;
	char *nfile, *f;
	size_t flen;

	/* does not matter which URI, all end with same filename */
	file = strrchr(tal->uri[0], '/');
	assert(file);

	if (asprintf(&nfile, "ta/%s%s", tal->descr, file) == -1)
		err(1, NULL);

	f = load_file(nfile, &flen);
	if (f == NULL) {
		warn("parse file %s", nfile);
		free(nfile);
		return;
	}

	/* if TA is valid it was added as a root which is all we need */
	proc_parser_root_cert(nfile, f, flen, tal->pkey, tal->pkeysz, tal->id);
	free(nfile);
	free(f);
}

/*
 * Parse file passed with -f option.
 */
static void
proc_parser_file(char *file, unsigned char *buf, size_t len)
{
	X509 *x509 = NULL;
	struct cert *cert = NULL;
	struct mft *mft = NULL;
	struct roa *roa = NULL;
	struct gbr *gbr = NULL;
	struct tal *tal = NULL;
	enum rtype type;
	char *aia = NULL, *aki = NULL, *ski = NULL;
	unsigned long verify_flags = X509_V_FLAG_CRL_CHECK;

	if ((type = rtype_from_file_extension(file)) == RTYPE_INVALID)
		errx(1, "%s: unsupported file type", file);

	switch (type) {
	case RTYPE_CER:
		cert = cert_parse(file, buf, len);
		if (cert == NULL)
			break;
		cert_print(cert);
		aia = cert->aia;
		aki = cert->aki;
		ski = cert->ski;
		x509 = cert->x509;
		if (X509_up_ref(x509) == 0)
			errx(1, "%s: X509_up_ref failed", __func__);
		break;
	case RTYPE_MFT:
		mft = mft_parse(&x509, file, buf, len);
		if (mft == NULL)
			break;
		mft_print(mft);
		aia = mft->aia;
		aki = mft->aki;
		ski = mft->ski;
		verify_flags = 0;
		break;
	case RTYPE_ROA:
		roa = roa_parse(&x509, file, buf, len);
		if (roa == NULL)
			break;
		roa_print(roa);
		aia = roa->aia;
		aki = roa->aki;
		ski = roa->ski;
		break;
	case RTYPE_GBR:
		gbr = gbr_parse(&x509, file, buf, len);
		if (gbr == NULL)
			break;
		gbr_print(gbr);
		aia = gbr->aia;
		aki = gbr->aki;
		ski = gbr->ski;
		break;
	case RTYPE_TAL:
		tal = tal_parse(file, buf, len);
		if (tal == NULL)
			break;
		tal_print(tal);
		break;
	case RTYPE_CRL: /* XXX no printer yet */
	default:
		break;
	}

	if (aia != NULL) {
		struct auth *a;
		struct crl *crl;
		char *c;

		c = x509_get_crl(x509, file);
		parse_load_crl(c);
		free(c);
		parse_load_certchain(aia);
		a = valid_ski_aki(file, &auths, ski, aki);
		crl = get_crl(a);

		if (valid_x509(file, x509, a, crl, verify_flags))
			printf("Validation: OK\n");
		else
			printf("Validation: Failed\n");
	}

	X509_free(x509);
	cert_free(cert);
	mft_free(mft);
	roa_free(roa);
	gbr_free(gbr);
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
			parse_load_ta(tal);
			tal_free(tal);
			break;
		default:
			errx(1, "unhandled entity type %d", entp->type);
		}

		b = io_new_buffer();
		io_simple_buffer(b, &entp->type, sizeof(entp->type));
		io_str_buffer(b, entp->file);
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
proc_parser(int fd)
{
	struct entityq	 q;
	struct msgbuf	 msgq;
	struct pollfd	 pfd;
	struct entity	*entp;
	struct ibuf	*b, *inbuf = NULL;

	ERR_load_crypto_strings();
	OpenSSL_add_all_ciphers();
	OpenSSL_add_all_digests();
	x509_init_oid();

	if ((ctx = X509_STORE_CTX_new()) == NULL)
		cryptoerrx("X509_STORE_CTX_new");

	TAILQ_INIT(&q);

	msgbuf_init(&msgq);
	msgq.fd = fd;

	pfd.fd = fd;

	for (;;) {
		pfd.events = POLLIN;
		if (msgq.queued)
			pfd.events |= POLLOUT;

		if (poll(&pfd, 1, INFTIM) == -1)
			err(1, "poll");
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

		if (!filemode)
			parse_entity(&q, &msgq);
		else
			parse_file(&q, &msgq);
	}

	while ((entp = TAILQ_FIRST(&q)) != NULL) {
		TAILQ_REMOVE(&q, entp, entries);
		entity_free(entp);
	}

	/* XXX free auths and crl tree */

	X509_STORE_CTX_free(ctx);
	msgbuf_clear(&msgq);

	exit(0);
}
