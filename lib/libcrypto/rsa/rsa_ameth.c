/* $OpenBSD: rsa_ameth.c,v 1.31 2023/08/10 09:36:37 tb Exp $ */
/* Written by Dr Stephen N Henson (steve@openssl.org) for the OpenSSL
 * project 2006.
 */
/* ====================================================================
 * Copyright (c) 2006 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <stdio.h>

#include <openssl/opensslconf.h>

#include <openssl/asn1t.h>
#include <openssl/bn.h>
#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "asn1_local.h"
#include "bn_local.h"
#include "cryptlib.h"
#include "evp_local.h"
#include "rsa_local.h"

#ifndef OPENSSL_NO_CMS
static int rsa_cms_sign(CMS_SignerInfo *si);
static int rsa_cms_verify(CMS_SignerInfo *si);
static int rsa_cms_decrypt(CMS_RecipientInfo *ri);
static int rsa_cms_encrypt(CMS_RecipientInfo *ri);
#endif

static RSA_PSS_PARAMS *rsa_pss_decode(const X509_ALGOR *alg);

/* Set any parameters associated with pkey */
static int
rsa_param_encode(const EVP_PKEY *pkey, ASN1_STRING **pstr, int *pstrtype)
{
	const RSA *rsa = pkey->pkey.rsa;

	*pstr = NULL;

	/* If RSA it's just NULL type */
	if (pkey->ameth->pkey_id != EVP_PKEY_RSA_PSS) {
		*pstrtype = V_ASN1_NULL;
		return 1;
	}

	/* If no PSS parameters we omit parameters entirely */
	if (rsa->pss == NULL) {
		*pstrtype = V_ASN1_UNDEF;
		return 1;
	}

	/* Encode PSS parameters */
	if (ASN1_item_pack(rsa->pss, &RSA_PSS_PARAMS_it, pstr) == NULL)
		return 0;

	*pstrtype = V_ASN1_SEQUENCE;
	return 1;
}

/* Decode any parameters and set them in RSA structure */
static int
rsa_param_decode(RSA *rsa, const X509_ALGOR *alg)
{
	const ASN1_OBJECT *algoid;
	const void *algp;
	int algptype;

	X509_ALGOR_get0(&algoid, &algptype, &algp, alg);
	if (OBJ_obj2nid(algoid) != EVP_PKEY_RSA_PSS)
		return 1;
	if (algptype == V_ASN1_UNDEF)
		return 1;
	if (algptype != V_ASN1_SEQUENCE) {
		RSAerror(RSA_R_INVALID_PSS_PARAMETERS);
		return 0;
	}
	rsa->pss = rsa_pss_decode(alg);
	if (rsa->pss == NULL)
		return 0;
	return 1;
}

static int
rsa_pub_encode(X509_PUBKEY *pk, const EVP_PKEY *pkey)
{
	unsigned char *penc = NULL;
	int penclen;
	ASN1_STRING *str;
	int strtype;

	if (!rsa_param_encode(pkey, &str, &strtype))
		return 0;
	penclen = i2d_RSAPublicKey(pkey->pkey.rsa, &penc);
	if (penclen <= 0)
		return 0;
	if (X509_PUBKEY_set0_param(pk, OBJ_nid2obj(pkey->ameth->pkey_id),
	    strtype, str, penc, penclen))
		return 1;

	free(penc);

	return 0;
}

static int
rsa_pub_decode(EVP_PKEY *pkey, X509_PUBKEY *pubkey)
{
	const unsigned char *p;
	int pklen;
	X509_ALGOR *alg;
	RSA *rsa = NULL;

	if (!X509_PUBKEY_get0_param(NULL, &p, &pklen, &alg, pubkey))
		return 0;
	if ((rsa = d2i_RSAPublicKey(NULL, &p, pklen)) == NULL) {
		RSAerror(ERR_R_RSA_LIB);
		return 0;
	}
	if (!rsa_param_decode(rsa, alg)) {
		RSA_free(rsa);
		return 0;
	}
	if (!EVP_PKEY_assign(pkey, pkey->ameth->pkey_id, rsa)) {
		RSA_free(rsa);
		return 0;
	}
	return 1;
}

static int
rsa_pub_cmp(const EVP_PKEY *a, const EVP_PKEY *b)
{
	if (BN_cmp(b->pkey.rsa->n, a->pkey.rsa->n) != 0 ||
	    BN_cmp(b->pkey.rsa->e, a->pkey.rsa->e) != 0)
		return 0;

	return 1;
}

static int
old_rsa_priv_decode(EVP_PKEY *pkey, const unsigned char **pder, int derlen)
{
	RSA *rsa;

	if ((rsa = d2i_RSAPrivateKey(NULL, pder, derlen)) == NULL) {
		RSAerror(ERR_R_RSA_LIB);
		return 0;
	}
	EVP_PKEY_assign(pkey, pkey->ameth->pkey_id, rsa);
	return 1;
}

static int
old_rsa_priv_encode(const EVP_PKEY *pkey, unsigned char **pder)
{
	return i2d_RSAPrivateKey(pkey->pkey.rsa, pder);
}

static int
rsa_priv_encode(PKCS8_PRIV_KEY_INFO *p8, const EVP_PKEY *pkey)
{
	ASN1_STRING *str = NULL;
	ASN1_OBJECT *aobj;
	int strtype;
	unsigned char *rk = NULL;
	int rklen = 0;

	if (!rsa_param_encode(pkey, &str, &strtype))
		goto err;
	if ((rklen = i2d_RSAPrivateKey(pkey->pkey.rsa, &rk)) <= 0) {
		RSAerror(ERR_R_MALLOC_FAILURE);
		rklen = 0;
		goto err;
	}
	if ((aobj = OBJ_nid2obj(pkey->ameth->pkey_id)) == NULL)
		goto err;
	if (!PKCS8_pkey_set0(p8, aobj, 0, strtype, str, rk, rklen)) {
		RSAerror(ERR_R_MALLOC_FAILURE);
		goto err;
	}

	return 1;

 err:
	ASN1_STRING_free(str);
	freezero(rk, rklen);

	return 0;
}

static int
rsa_priv_decode(EVP_PKEY *pkey, const PKCS8_PRIV_KEY_INFO *p8)
{
	const unsigned char *p;
	RSA *rsa;
	int pklen;
	const X509_ALGOR *alg;

	if (!PKCS8_pkey_get0(NULL, &p, &pklen, &alg, p8))
		return 0;
	rsa = d2i_RSAPrivateKey(NULL, &p, pklen);
	if (rsa == NULL) {
		RSAerror(ERR_R_RSA_LIB);
		return 0;
	}
	if (!rsa_param_decode(rsa, alg)) {
		RSA_free(rsa);
		return 0;
	}
	EVP_PKEY_assign(pkey, pkey->ameth->pkey_id, rsa);

	return 1;
}

static int
int_rsa_size(const EVP_PKEY *pkey)
{
	return RSA_size(pkey->pkey.rsa);
}

static int
rsa_bits(const EVP_PKEY *pkey)
{
	return BN_num_bits(pkey->pkey.rsa->n);
}

static int
rsa_security_bits(const EVP_PKEY *pkey)
{
	return RSA_security_bits(pkey->pkey.rsa);
}

static void
int_rsa_free(EVP_PKEY *pkey)
{
	RSA_free(pkey->pkey.rsa);
}

static X509_ALGOR *
rsa_mgf1_decode(X509_ALGOR *alg)
{
	if (OBJ_obj2nid(alg->algorithm) != NID_mgf1)
		return NULL;

	return ASN1_TYPE_unpack_sequence(&X509_ALGOR_it, alg->parameter);
}

static RSA_PSS_PARAMS *
rsa_pss_decode(const X509_ALGOR *alg)
{
	RSA_PSS_PARAMS *pss;

	pss = ASN1_TYPE_unpack_sequence(&RSA_PSS_PARAMS_it, alg->parameter);
	if (pss == NULL)
		return NULL;

	if (pss->maskGenAlgorithm != NULL) {
		pss->maskHash = rsa_mgf1_decode(pss->maskGenAlgorithm);
		if (pss->maskHash == NULL) {
			RSA_PSS_PARAMS_free(pss);
			return NULL;
		}
	}

	return pss;
}

static int
rsa_pss_param_print(BIO *bp, int pss_key, RSA_PSS_PARAMS *pss, int indent)
{
	int rv = 0;
	X509_ALGOR *maskHash = NULL;

	if (!BIO_indent(bp, indent, 128))
		goto err;
	if (pss_key) {
		if (pss == NULL) {
			if (BIO_puts(bp, "No PSS parameter restrictions\n") <= 0)
				return 0;
			return 1;
		} else {
			if (BIO_puts(bp, "PSS parameter restrictions:") <= 0)
				return 0;
		}
	} else if (pss == NULL) {
		if (BIO_puts(bp,"(INVALID PSS PARAMETERS)\n") <= 0)
			return 0;
		return 1;
	}
	if (BIO_puts(bp, "\n") <= 0)
		goto err;
	if (pss_key)
		indent += 2;
	if (!BIO_indent(bp, indent, 128))
		goto err;
	if (BIO_puts(bp, "Hash Algorithm: ") <= 0)
		goto err;

	if (pss->hashAlgorithm) {
		if (i2a_ASN1_OBJECT(bp, pss->hashAlgorithm->algorithm) <= 0)
			goto err;
	} else if (BIO_puts(bp, "sha1 (default)") <= 0) {
		goto err;
	}

	if (BIO_puts(bp, "\n") <= 0)
		goto err;

	if (!BIO_indent(bp, indent, 128))
		goto err;

	if (BIO_puts(bp, "Mask Algorithm: ") <= 0)
		goto err;
	if (pss->maskGenAlgorithm) {
		if (i2a_ASN1_OBJECT(bp, pss->maskGenAlgorithm->algorithm) <= 0)
			goto err;
		if (BIO_puts(bp, " with ") <= 0)
			goto err;
		maskHash = rsa_mgf1_decode(pss->maskGenAlgorithm);
		if (maskHash != NULL) {
			if (i2a_ASN1_OBJECT(bp, maskHash->algorithm) <= 0)
				goto err;
		} else if (BIO_puts(bp, "INVALID") <= 0) {
			goto err;
		}
	} else if (BIO_puts(bp, "mgf1 with sha1 (default)") <= 0) {
		goto err;
	}
	BIO_puts(bp, "\n");

	if (!BIO_indent(bp, indent, 128))
		goto err;
	if (BIO_printf(bp, "%s Salt Length: 0x", pss_key ? "Minimum" : "") <= 0)
		goto err;
	if (pss->saltLength) {
		if (i2a_ASN1_INTEGER(bp, pss->saltLength) <= 0)
			goto err;
	} else if (BIO_puts(bp, "14 (default)") <= 0) {
		goto err;
	}
	BIO_puts(bp, "\n");

	if (!BIO_indent(bp, indent, 128))
		goto err;
	if (BIO_puts(bp, "Trailer Field: 0x") <= 0)
		goto err;
	if (pss->trailerField) {
		if (i2a_ASN1_INTEGER(bp, pss->trailerField) <= 0)
			goto err;
	} else if (BIO_puts(bp, "BC (default)") <= 0) {
		goto err;
	}
	BIO_puts(bp, "\n");

	rv = 1;

 err:
	X509_ALGOR_free(maskHash);
	return rv;

}

static int
pkey_rsa_print(BIO *bp, const EVP_PKEY *pkey, int off, int priv)
{
	const RSA *x = pkey->pkey.rsa;
	char *str;
	const char *s;
	int ret = 0, mod_len = 0;

	if (x->n != NULL)
		mod_len = BN_num_bits(x->n);

	if (!BIO_indent(bp, off, 128))
		goto err;

	if (BIO_printf(bp, "%s ", pkey_is_pss(pkey) ?  "RSA-PSS" : "RSA") <= 0)
		goto err;

	if (priv && x->d != NULL) {
		if (BIO_printf(bp, "Private-Key: (%d bit)\n", mod_len) <= 0)
			goto err;
		str = "modulus:";
		s = "publicExponent:";
	} else {
		if (BIO_printf(bp, "Public-Key: (%d bit)\n", mod_len) <= 0)
			goto err;
		str = "Modulus:";
		s = "Exponent:";
	}
	if (!bn_printf(bp, x->n, off, "%s", str))
		goto err;
	if (!bn_printf(bp, x->e, off, "%s", s))
		goto err;
	if (priv) {
		if (!bn_printf(bp, x->d, off, "privateExponent:"))
			goto err;
		if (!bn_printf(bp, x->p, off, "prime1:"))
			goto err;
		if (!bn_printf(bp, x->q, off, "prime2:"))
			goto err;
		if (!bn_printf(bp, x->dmp1, off, "exponent1:"))
			goto err;
		if (!bn_printf(bp, x->dmq1, off, "exponent2:"))
			goto err;
		if (!bn_printf(bp, x->iqmp, off, "coefficient:"))
			goto err;
	}
	if (pkey_is_pss(pkey) && !rsa_pss_param_print(bp, 1, x->pss, off))
		goto err;
	ret = 1;
 err:
	return ret;
}

static int
rsa_pub_print(BIO *bp, const EVP_PKEY *pkey, int indent, ASN1_PCTX *ctx)
{
	return pkey_rsa_print(bp, pkey, indent, 0);
}

static int
rsa_priv_print(BIO *bp, const EVP_PKEY *pkey, int indent, ASN1_PCTX *ctx)
{
	return pkey_rsa_print(bp, pkey, indent, 1);
}

static int
rsa_sig_print(BIO *bp, const X509_ALGOR *sigalg, const ASN1_STRING *sig,
    int indent, ASN1_PCTX *pctx)
{
	if (OBJ_obj2nid(sigalg->algorithm) == EVP_PKEY_RSA_PSS) {
		int rv;
		RSA_PSS_PARAMS *pss = rsa_pss_decode(sigalg);

		rv = rsa_pss_param_print(bp, 0, pss, indent);
		RSA_PSS_PARAMS_free(pss);
		if (!rv)
			return 0;
	} else if (!sig && BIO_puts(bp, "\n") <= 0) {
		return 0;
	}
	if (sig)
		return X509_signature_dump(bp, sig, indent);
	return 1;
}

static int
rsa_pkey_ctrl(EVP_PKEY *pkey, int op, long arg1, void *arg2)
{
	X509_ALGOR *alg = NULL;
	const EVP_MD *md;
	const EVP_MD *mgf1md;
	int min_saltlen;

	switch (op) {
	case ASN1_PKEY_CTRL_PKCS7_SIGN:
		if (arg1 == 0)
			PKCS7_SIGNER_INFO_get0_algs(arg2, NULL, NULL, &alg);
		break;

	case ASN1_PKEY_CTRL_PKCS7_ENCRYPT:
		if (pkey_is_pss(pkey))
			return -2;
		if (arg1 == 0)
			PKCS7_RECIP_INFO_get0_alg(arg2, &alg);
		break;
#ifndef OPENSSL_NO_CMS
	case ASN1_PKEY_CTRL_CMS_SIGN:
		if (arg1 == 0)
			return rsa_cms_sign(arg2);
		else if (arg1 == 1)
			return rsa_cms_verify(arg2);
		break;

	case ASN1_PKEY_CTRL_CMS_ENVELOPE:
		if (pkey_is_pss(pkey))
			return -2;
		if (arg1 == 0)
			return rsa_cms_encrypt(arg2);
		else if (arg1 == 1)
			return rsa_cms_decrypt(arg2);
		break;

	case ASN1_PKEY_CTRL_CMS_RI_TYPE:
		if (pkey_is_pss(pkey))
			return -2;
		*(int *)arg2 = CMS_RECIPINFO_TRANS;
		return 1;
#endif

	case ASN1_PKEY_CTRL_DEFAULT_MD_NID:
		if (pkey->pkey.rsa->pss != NULL) {
			if (!rsa_pss_get_param(pkey->pkey.rsa->pss, &md, &mgf1md,
			    &min_saltlen)) {
				RSAerror(ERR_R_INTERNAL_ERROR);
				return 0;
			}
			*(int *)arg2 = EVP_MD_type(md);
			/* Return of 2 indicates this MD is mandatory */
			return 2;
		}
		*(int *)arg2 = NID_sha256;
		return 1;

	default:
		return -2;
	}

	if (alg)
		X509_ALGOR_set0(alg, OBJ_nid2obj(NID_rsaEncryption),
		    V_ASN1_NULL, 0);

	return 1;
}

/* Allocate and set algorithm ID from EVP_MD, defaults to SHA1. */
static int
rsa_md_to_algor(X509_ALGOR **palg, const EVP_MD *md)
{
	if (md == NULL || EVP_MD_type(md) == NID_sha1)
		return 1;
	*palg = X509_ALGOR_new();
	if (*palg == NULL)
		return 0;
	X509_ALGOR_set_md(*palg, md);
	return 1;
}

/* Allocate and set MGF1 algorithm ID from EVP_MD. */
static int
rsa_md_to_mgf1(X509_ALGOR **palg, const EVP_MD *mgf1md)
{
	X509_ALGOR *algtmp = NULL;
	ASN1_STRING *stmp = NULL;

	*palg = NULL;
	if (mgf1md == NULL || EVP_MD_type(mgf1md) == NID_sha1)
		return 1;
	/* need to embed algorithm ID inside another */
	if (!rsa_md_to_algor(&algtmp, mgf1md))
		goto err;
	if (ASN1_item_pack(algtmp, &X509_ALGOR_it, &stmp) == NULL)
		 goto err;
	*palg = X509_ALGOR_new();
	if (*palg == NULL)
		goto err;
	X509_ALGOR_set0(*palg, OBJ_nid2obj(NID_mgf1), V_ASN1_SEQUENCE, stmp);
	stmp = NULL;
 err:
	ASN1_STRING_free(stmp);
	X509_ALGOR_free(algtmp);
	if (*palg)
		return 1;
	return 0;
}

/* Convert algorithm ID to EVP_MD, defaults to SHA1. */
static const EVP_MD *
rsa_algor_to_md(X509_ALGOR *alg)
{
	const EVP_MD *md;

	if (!alg)
		return EVP_sha1();
	md = EVP_get_digestbyobj(alg->algorithm);
	if (md == NULL)
		RSAerror(RSA_R_UNKNOWN_DIGEST);
	return md;
}

/*
 * Convert EVP_PKEY_CTX in PSS mode into corresponding algorithm parameter,
 * suitable for setting an AlgorithmIdentifier.
 */
static RSA_PSS_PARAMS *
rsa_ctx_to_pss(EVP_PKEY_CTX *pkctx)
{
	const EVP_MD *sigmd, *mgf1md;
	EVP_PKEY *pk = EVP_PKEY_CTX_get0_pkey(pkctx);
	int saltlen;

	if (EVP_PKEY_CTX_get_signature_md(pkctx, &sigmd) <= 0)
		return NULL;
	if (EVP_PKEY_CTX_get_rsa_mgf1_md(pkctx, &mgf1md) <= 0)
		return NULL;
	if (!EVP_PKEY_CTX_get_rsa_pss_saltlen(pkctx, &saltlen))
		return NULL;
	if (saltlen == -1) {
		saltlen = EVP_MD_size(sigmd);
	} else if (saltlen == -2 || saltlen == -3) {
		saltlen = EVP_PKEY_size(pk) - EVP_MD_size(sigmd) - 2;
		if ((EVP_PKEY_bits(pk) & 0x7) == 1)
			saltlen--;
		if (saltlen < 0)
			return NULL;
	}

	return rsa_pss_params_create(sigmd, mgf1md, saltlen);
}

RSA_PSS_PARAMS *
rsa_pss_params_create(const EVP_MD *sigmd, const EVP_MD *mgf1md, int saltlen)
{
	RSA_PSS_PARAMS *pss = RSA_PSS_PARAMS_new();

	if (pss == NULL)
		goto err;
	if (saltlen != 20) {
		pss->saltLength = ASN1_INTEGER_new();
		if (pss->saltLength == NULL)
			goto err;
		if (!ASN1_INTEGER_set(pss->saltLength, saltlen))
			goto err;
	}
	if (!rsa_md_to_algor(&pss->hashAlgorithm, sigmd))
		goto err;
	if (mgf1md == NULL)
		mgf1md = sigmd;
	if (!rsa_md_to_mgf1(&pss->maskGenAlgorithm, mgf1md))
		goto err;
	if (!rsa_md_to_algor(&pss->maskHash, mgf1md))
		goto err;
	return pss;
 err:
	RSA_PSS_PARAMS_free(pss);
	return NULL;
}

static ASN1_STRING *
rsa_ctx_to_pss_string(EVP_PKEY_CTX *pkctx)
{
	RSA_PSS_PARAMS *pss = rsa_ctx_to_pss(pkctx);
	ASN1_STRING *os;

	if (pss == NULL)
		return NULL;

	os = ASN1_item_pack(pss, &RSA_PSS_PARAMS_it, NULL);
	RSA_PSS_PARAMS_free(pss);
	return os;
}

/*
 * From PSS AlgorithmIdentifier set public key parameters. If pkey isn't NULL
 * then the EVP_MD_CTX is setup and initialised. If it is NULL parameters are
 * passed to pkctx instead.
 */

static int
rsa_pss_to_ctx(EVP_MD_CTX *ctx, EVP_PKEY_CTX *pkctx,
    X509_ALGOR *sigalg, EVP_PKEY *pkey)
{
	int rv = -1;
	int saltlen;
	const EVP_MD *mgf1md = NULL, *md = NULL;
	RSA_PSS_PARAMS *pss;

	/* Sanity check: make sure it is PSS */
	if (OBJ_obj2nid(sigalg->algorithm) != EVP_PKEY_RSA_PSS) {
		RSAerror(RSA_R_UNSUPPORTED_SIGNATURE_TYPE);
		return -1;
	}
	/* Decode PSS parameters */
	pss = rsa_pss_decode(sigalg);

	if (!rsa_pss_get_param(pss, &md, &mgf1md, &saltlen)) {
		RSAerror(RSA_R_INVALID_PSS_PARAMETERS);
		goto err;
	}

	/* We have all parameters now set up context */
	if (pkey) {
		if (!EVP_DigestVerifyInit(ctx, &pkctx, md, NULL, pkey))
			goto err;
	} else {
		const EVP_MD *checkmd;
		if (EVP_PKEY_CTX_get_signature_md(pkctx, &checkmd) <= 0)
			goto err;
		if (EVP_MD_type(md) != EVP_MD_type(checkmd)) {
			RSAerror(RSA_R_DIGEST_DOES_NOT_MATCH);
			goto err;
		}
	}

	if (EVP_PKEY_CTX_set_rsa_padding(pkctx, RSA_PKCS1_PSS_PADDING) <= 0)
		goto err;

	if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pkctx, saltlen) <= 0)
		goto err;

	if (EVP_PKEY_CTX_set_rsa_mgf1_md(pkctx, mgf1md) <= 0)
		goto err;
	/* Carry on */
	rv = 1;

 err:
	RSA_PSS_PARAMS_free(pss);
	return rv;
}

int
rsa_pss_get_param(const RSA_PSS_PARAMS *pss, const EVP_MD **pmd,
    const EVP_MD **pmgf1md, int *psaltlen)
{
	if (pss == NULL)
		return 0;
	*pmd = rsa_algor_to_md(pss->hashAlgorithm);
	if (*pmd == NULL)
		return 0;
	*pmgf1md = rsa_algor_to_md(pss->maskHash);
	if (*pmgf1md == NULL)
		return 0;
	if (pss->saltLength) {
		*psaltlen = ASN1_INTEGER_get(pss->saltLength);
		if (*psaltlen < 0) {
			RSAerror(RSA_R_INVALID_SALT_LENGTH);
			return 0;
		}
	} else {
		*psaltlen = 20;
	}

	/*
	 * low-level routines support only trailer field 0xbc (value 1) and
	 * PKCS#1 says we should reject any other value anyway.
	 */
	if (pss->trailerField && ASN1_INTEGER_get(pss->trailerField) != 1) {
		RSAerror(RSA_R_INVALID_TRAILER);
		return 0;
	}

	return 1;
}

#ifndef OPENSSL_NO_CMS
static int
rsa_cms_verify(CMS_SignerInfo *si)
{
	int nid, nid2;
	X509_ALGOR *alg;
	EVP_PKEY_CTX *pkctx = CMS_SignerInfo_get0_pkey_ctx(si);

	CMS_SignerInfo_get0_algs(si, NULL, NULL, NULL, &alg);
	nid = OBJ_obj2nid(alg->algorithm);
	if (nid == EVP_PKEY_RSA_PSS)
		return rsa_pss_to_ctx(NULL, pkctx, alg, NULL);
	/* Only PSS allowed for PSS keys */
	if (pkey_ctx_is_pss(pkctx)) {
		RSAerror(RSA_R_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE);
		return 0;
	}
	if (nid == NID_rsaEncryption)
		return 1;
	/* Workaround for some implementation that use a signature OID */
	if (OBJ_find_sigid_algs(nid, NULL, &nid2)) {
		if (nid2 == NID_rsaEncryption)
			return 1;
	}
	return 0;
}
#endif

/*
 * Customised RSA item verification routine. This is called when a signature
 * is encountered requiring special handling. We currently only handle PSS.
 */
static int
rsa_item_verify(EVP_MD_CTX *ctx, const ASN1_ITEM *it, void *asn,
    X509_ALGOR *sigalg, ASN1_BIT_STRING *sig, EVP_PKEY *pkey)
{
	/* Sanity check: make sure it is PSS */
	if (OBJ_obj2nid(sigalg->algorithm) != EVP_PKEY_RSA_PSS) {
		RSAerror(RSA_R_UNSUPPORTED_SIGNATURE_TYPE);
		return -1;
	}
	if (rsa_pss_to_ctx(ctx, NULL, sigalg, pkey) > 0) {
		/* Carry on */
		return 2;
	}
	return -1;
}

#ifndef OPENSSL_NO_CMS
static int
rsa_cms_sign(CMS_SignerInfo *si)
{
	int pad_mode = RSA_PKCS1_PADDING;
	X509_ALGOR *alg;
	EVP_PKEY_CTX *pkctx = CMS_SignerInfo_get0_pkey_ctx(si);
	ASN1_STRING *os = NULL;

	CMS_SignerInfo_get0_algs(si, NULL, NULL, NULL, &alg);
	if (pkctx) {
		if (EVP_PKEY_CTX_get_rsa_padding(pkctx, &pad_mode) <= 0)
			return 0;
	}
	if (pad_mode == RSA_PKCS1_PADDING) {
		X509_ALGOR_set0(alg, OBJ_nid2obj(NID_rsaEncryption), V_ASN1_NULL, 0);
		return 1;
	}
	/* We don't support it */
	if (pad_mode != RSA_PKCS1_PSS_PADDING)
		return 0;
	os = rsa_ctx_to_pss_string(pkctx);
	if (!os)
		return 0;
	X509_ALGOR_set0(alg, OBJ_nid2obj(EVP_PKEY_RSA_PSS), V_ASN1_SEQUENCE, os);
	return 1;
}
#endif

static int
rsa_item_sign(EVP_MD_CTX *ctx, const ASN1_ITEM *it, void *asn,
    X509_ALGOR *alg1, X509_ALGOR *alg2, ASN1_BIT_STRING *sig)
{
	EVP_PKEY_CTX *pkctx = ctx->pctx;
	int pad_mode;

	if (EVP_PKEY_CTX_get_rsa_padding(pkctx, &pad_mode) <= 0)
		return 0;
	if (pad_mode == RSA_PKCS1_PADDING)
		return 2;
	if (pad_mode == RSA_PKCS1_PSS_PADDING) {
		ASN1_STRING *os1 = NULL;
		os1 = rsa_ctx_to_pss_string(pkctx);
		if (!os1)
			return 0;
		/* Duplicate parameters if we have to */
		if (alg2) {
			ASN1_STRING *os2 = ASN1_STRING_dup(os1);
			if (!os2) {
				ASN1_STRING_free(os1);
				return 0;
			}
			X509_ALGOR_set0(alg2, OBJ_nid2obj(EVP_PKEY_RSA_PSS),
			    V_ASN1_SEQUENCE, os2);
		}
		X509_ALGOR_set0(alg1, OBJ_nid2obj(EVP_PKEY_RSA_PSS),
		    V_ASN1_SEQUENCE, os1);
		return 3;
	}
	return 2;
}

static int
rsa_pkey_check(const EVP_PKEY *pkey)
{
	return RSA_check_key(pkey->pkey.rsa);
}

#ifndef OPENSSL_NO_CMS
static RSA_OAEP_PARAMS *
rsa_oaep_decode(const X509_ALGOR *alg)
{
	RSA_OAEP_PARAMS *oaep;

	oaep = ASN1_TYPE_unpack_sequence(&RSA_OAEP_PARAMS_it, alg->parameter);
	if (oaep == NULL)
		return NULL;

	if (oaep->maskGenFunc != NULL) {
		oaep->maskHash = rsa_mgf1_decode(oaep->maskGenFunc);
		if (oaep->maskHash == NULL) {
			RSA_OAEP_PARAMS_free(oaep);
			return NULL;
		}
	}
	return oaep;
}

static int
rsa_cms_decrypt(CMS_RecipientInfo *ri)
{
	EVP_PKEY_CTX *pkctx;
	X509_ALGOR *cmsalg;
	int nid;
	int rv = -1;
	unsigned char *label = NULL;
	int labellen = 0;
	const EVP_MD *mgf1md = NULL, *md = NULL;
	RSA_OAEP_PARAMS *oaep;

	pkctx = CMS_RecipientInfo_get0_pkey_ctx(ri);
	if (pkctx == NULL)
		return 0;
	if (!CMS_RecipientInfo_ktri_get0_algs(ri, NULL, NULL, &cmsalg))
		return -1;
	nid = OBJ_obj2nid(cmsalg->algorithm);
	if (nid == NID_rsaEncryption)
		return 1;
	if (nid != NID_rsaesOaep) {
		RSAerror(RSA_R_UNSUPPORTED_ENCRYPTION_TYPE);
		return -1;
	}
	/* Decode OAEP parameters */
	oaep = rsa_oaep_decode(cmsalg);

	if (oaep == NULL) {
		RSAerror(RSA_R_INVALID_OAEP_PARAMETERS);
		goto err;
	}

	mgf1md = rsa_algor_to_md(oaep->maskHash);
	if (mgf1md == NULL)
		goto err;
	md = rsa_algor_to_md(oaep->hashFunc);
	if (md == NULL)
		goto err;

	if (oaep->pSourceFunc != NULL) {
		X509_ALGOR *plab = oaep->pSourceFunc;

		if (OBJ_obj2nid(plab->algorithm) != NID_pSpecified) {
			RSAerror(RSA_R_UNSUPPORTED_LABEL_SOURCE);
			goto err;
		}
		if (plab->parameter->type != V_ASN1_OCTET_STRING) {
			RSAerror(RSA_R_INVALID_LABEL);
			goto err;
		}

		label = plab->parameter->value.octet_string->data;

		/* Stop label being freed when OAEP parameters are freed */
		/* XXX - this leaks label on error... */
		plab->parameter->value.octet_string->data = NULL;
		labellen = plab->parameter->value.octet_string->length;
	}

	if (EVP_PKEY_CTX_set_rsa_padding(pkctx, RSA_PKCS1_OAEP_PADDING) <= 0)
		goto err;
	if (EVP_PKEY_CTX_set_rsa_oaep_md(pkctx, md) <= 0)
		goto err;
	if (EVP_PKEY_CTX_set_rsa_mgf1_md(pkctx, mgf1md) <= 0)
		goto err;
	if (EVP_PKEY_CTX_set0_rsa_oaep_label(pkctx, label, labellen) <= 0)
		goto err;

	rv = 1;

 err:
	RSA_OAEP_PARAMS_free(oaep);
	return rv;
}

static int
rsa_cms_encrypt(CMS_RecipientInfo *ri)
{
	const EVP_MD *md, *mgf1md;
	RSA_OAEP_PARAMS *oaep = NULL;
	ASN1_STRING *os = NULL;
	X509_ALGOR *alg;
	EVP_PKEY_CTX *pkctx = CMS_RecipientInfo_get0_pkey_ctx(ri);
	int pad_mode = RSA_PKCS1_PADDING, rv = 0, labellen;
	unsigned char *label;

	if (CMS_RecipientInfo_ktri_get0_algs(ri, NULL, NULL, &alg) <= 0)
		return 0;
	if (pkctx) {
		if (EVP_PKEY_CTX_get_rsa_padding(pkctx, &pad_mode) <= 0)
			return 0;
	}
	if (pad_mode == RSA_PKCS1_PADDING) {
		X509_ALGOR_set0(alg, OBJ_nid2obj(NID_rsaEncryption), V_ASN1_NULL, 0);
		return 1;
	}
	/* Not supported */
	if (pad_mode != RSA_PKCS1_OAEP_PADDING)
		return 0;
	if (EVP_PKEY_CTX_get_rsa_oaep_md(pkctx, &md) <= 0)
		goto err;
	if (EVP_PKEY_CTX_get_rsa_mgf1_md(pkctx, &mgf1md) <= 0)
		goto err;
	labellen = EVP_PKEY_CTX_get0_rsa_oaep_label(pkctx, &label);
	if (labellen < 0)
		goto err;
	oaep = RSA_OAEP_PARAMS_new();
	if (oaep == NULL)
		goto err;
	if (!rsa_md_to_algor(&oaep->hashFunc, md))
		goto err;
	if (!rsa_md_to_mgf1(&oaep->maskGenFunc, mgf1md))
		goto err;
	if (labellen > 0) {
		ASN1_OCTET_STRING *los;
		oaep->pSourceFunc = X509_ALGOR_new();
		if (oaep->pSourceFunc == NULL)
			goto err;
		los = ASN1_OCTET_STRING_new();
		if (los == NULL)
			goto err;
		if (!ASN1_OCTET_STRING_set(los, label, labellen)) {
			ASN1_OCTET_STRING_free(los);
			goto err;
		}
		X509_ALGOR_set0(oaep->pSourceFunc, OBJ_nid2obj(NID_pSpecified),
		    V_ASN1_OCTET_STRING, los);
	}
	/* create string with pss parameter encoding. */
	if (!ASN1_item_pack(oaep, &RSA_OAEP_PARAMS_it, &os))
		 goto err;
	X509_ALGOR_set0(alg, OBJ_nid2obj(NID_rsaesOaep), V_ASN1_SEQUENCE, os);
	os = NULL;
	rv = 1;
 err:
	RSA_OAEP_PARAMS_free(oaep);
	ASN1_STRING_free(os);
	return rv;
}
#endif

const EVP_PKEY_ASN1_METHOD rsa_asn1_meths[] = {
	{
		.pkey_id = EVP_PKEY_RSA,
		.pkey_base_id = EVP_PKEY_RSA,
		.pkey_flags = ASN1_PKEY_SIGPARAM_NULL,

		.pem_str = "RSA",
		.info = "OpenSSL RSA method",

		.pub_decode = rsa_pub_decode,
		.pub_encode = rsa_pub_encode,
		.pub_cmp = rsa_pub_cmp,
		.pub_print = rsa_pub_print,

		.priv_decode = rsa_priv_decode,
		.priv_encode = rsa_priv_encode,
		.priv_print = rsa_priv_print,

		.pkey_size = int_rsa_size,
		.pkey_bits = rsa_bits,
		.pkey_security_bits = rsa_security_bits,

		.sig_print = rsa_sig_print,

		.pkey_free = int_rsa_free,
		.pkey_ctrl = rsa_pkey_ctrl,
		.old_priv_decode = old_rsa_priv_decode,
		.old_priv_encode = old_rsa_priv_encode,
		.item_verify = rsa_item_verify,
		.item_sign = rsa_item_sign,

		.pkey_check = rsa_pkey_check,
	},

	{
		.pkey_id = EVP_PKEY_RSA2,
		.pkey_base_id = EVP_PKEY_RSA,
		.pkey_flags = ASN1_PKEY_ALIAS,

		.pkey_check = rsa_pkey_check,
	},
};

const EVP_PKEY_ASN1_METHOD rsa_pss_asn1_meth = {
	.pkey_id = EVP_PKEY_RSA_PSS,
	.pkey_base_id = EVP_PKEY_RSA_PSS,
	.pkey_flags = ASN1_PKEY_SIGPARAM_NULL,

	.pem_str = "RSA-PSS",
	.info = "OpenSSL RSA-PSS method",

	.pub_decode = rsa_pub_decode,
	.pub_encode = rsa_pub_encode,
	.pub_cmp = rsa_pub_cmp,
	.pub_print = rsa_pub_print,

	.priv_decode = rsa_priv_decode,
	.priv_encode = rsa_priv_encode,
	.priv_print = rsa_priv_print,

	.pkey_size = int_rsa_size,
	.pkey_bits = rsa_bits,
	.pkey_security_bits = rsa_security_bits,

	.sig_print = rsa_sig_print,

	.pkey_free = int_rsa_free,
	.pkey_ctrl = rsa_pkey_ctrl,
	.item_verify = rsa_item_verify,
	.item_sign = rsa_item_sign
};
