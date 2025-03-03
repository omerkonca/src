/*	$OpenBSD: x509.c,v 1.67 2023/03/10 12:02:11 job Exp $ */
/*
 * Copyright (c) 2022 Theo Buehler <tb@openbsd.org>
 * Copyright (c) 2021 Claudio Jeker <claudio@openbsd.org>
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

#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/x509v3.h>

#include "extern.h"

ASN1_OBJECT	*certpol_oid;	/* id-cp-ipAddr-asNumber cert policy */
ASN1_OBJECT	*carepo_oid;	/* 1.3.6.1.5.5.7.48.5 (caRepository) */
ASN1_OBJECT	*manifest_oid;	/* 1.3.6.1.5.5.7.48.10 (rpkiManifest) */
ASN1_OBJECT	*signedobj_oid;	/* 1.3.6.1.5.5.7.48.11 (signedObject) */
ASN1_OBJECT	*notify_oid;	/* 1.3.6.1.5.5.7.48.13 (rpkiNotify) */
ASN1_OBJECT	*roa_oid;	/* id-ct-routeOriginAuthz CMS content type */
ASN1_OBJECT	*mft_oid;	/* id-ct-rpkiManifest CMS content type */
ASN1_OBJECT	*gbr_oid;	/* id-ct-rpkiGhostbusters CMS content type */
ASN1_OBJECT	*bgpsec_oid;	/* id-kp-bgpsec-router Key Purpose */
ASN1_OBJECT	*cnt_type_oid;	/* pkcs-9 id-contentType */
ASN1_OBJECT	*msg_dgst_oid;	/* pkcs-9 id-messageDigest */
ASN1_OBJECT	*sign_time_oid;	/* pkcs-9 id-signingTime */
ASN1_OBJECT	*bin_sign_time_oid;	/* pkcs-9 id-aa-binarySigningTime */
ASN1_OBJECT	*rsc_oid;	/* id-ct-signedChecklist */
ASN1_OBJECT	*aspa_oid;	/* id-ct-ASPA */
ASN1_OBJECT	*tak_oid;	/* id-ct-SignedTAL */
ASN1_OBJECT	*geofeed_oid;	/* id-ct-geofeedCSVwithCRLF */

static const struct {
	const char	 *oid;
	ASN1_OBJECT	**ptr;
} oid_table[] = {
	{
		.oid = "1.3.6.1.5.5.7.14.2",
		.ptr = &certpol_oid,
	},
	{
		.oid = "1.3.6.1.5.5.7.48.5",
		.ptr = &carepo_oid,
	},
	{
		.oid = "1.3.6.1.5.5.7.48.10",
		.ptr = &manifest_oid,
	},
	{
		.oid = "1.3.6.1.5.5.7.48.11",
		.ptr = &signedobj_oid,
	},
	{
		.oid = "1.3.6.1.5.5.7.48.13",
		.ptr = &notify_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.16.1.24",
		.ptr = &roa_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.16.1.26",
		.ptr = &mft_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.16.1.35",
		.ptr = &gbr_oid,
	},
	{
		.oid = "1.3.6.1.5.5.7.3.30",
		.ptr = &bgpsec_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.3",
		.ptr = &cnt_type_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.4",
		.ptr = &msg_dgst_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.5",
		.ptr = &sign_time_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.16.2.46",
		.ptr = &bin_sign_time_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.16.1.47",
		.ptr = &geofeed_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.16.1.48",
		.ptr = &rsc_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.16.1.49",
		.ptr = &aspa_oid,
	},
	{
		.oid = "1.2.840.113549.1.9.16.1.50",
		.ptr = &tak_oid,
	},
};

void
x509_init_oid(void)
{
	size_t	i;

	for (i = 0; i < sizeof(oid_table) / sizeof(oid_table[0]); i++) {
		*oid_table[i].ptr = OBJ_txt2obj(oid_table[i].oid, 1);
		if (*oid_table[i].ptr == NULL)
			errx(1, "OBJ_txt2obj for %s failed", oid_table[i].oid);
	}
}

/*
 * Parse X509v3 authority key identifier (AKI), RFC 6487 sec. 4.8.3.
 * Returns the AKI or NULL if it could not be parsed.
 * The AKI is formatted as a hex string.
 */
int
x509_get_aki(X509 *x, const char *fn, char **aki)
{
	const unsigned char	*d;
	AUTHORITY_KEYID		*akid;
	ASN1_OCTET_STRING	*os;
	int			 dsz, crit, rc = 0;

	*aki = NULL;
	akid = X509_get_ext_d2i(x, NID_authority_key_identifier, &crit, NULL);
	if (akid == NULL)
		return 1;
	if (crit != 0) {
		warnx("%s: RFC 6487 section 4.8.3: "
		    "AKI: extension not non-critical", fn);
		goto out;
	}
	if (akid->issuer != NULL || akid->serial != NULL) {
		warnx("%s: RFC 6487 section 4.8.3: AKI: "
		    "authorityCertIssuer or authorityCertSerialNumber present",
		    fn);
		goto out;
	}

	os = akid->keyid;
	if (os == NULL) {
		warnx("%s: RFC 6487 section 4.8.3: AKI: "
		    "Key Identifier missing", fn);
		goto out;
	}

	d = os->data;
	dsz = os->length;

	if (dsz != SHA_DIGEST_LENGTH) {
		warnx("%s: RFC 6487 section 4.8.2: AKI: "
		    "want %d bytes SHA1 hash, have %d bytes",
		    fn, SHA_DIGEST_LENGTH, dsz);
		goto out;
	}

	*aki = hex_encode(d, dsz);
	rc = 1;
out:
	AUTHORITY_KEYID_free(akid);
	return rc;
}

/*
 * Parse X509v3 subject key identifier (SKI), RFC 6487 sec. 4.8.2.
 * The SKI must be the SHA1 hash of the Subject Public Key.
 * Returns the SKI formatted as hex string, or NULL if it couldn't be parsed.
 */
int
x509_get_ski(X509 *x, const char *fn, char **ski)
{
	const unsigned char	*d, *spk;
	ASN1_OCTET_STRING	*os;
	X509_PUBKEY		*pubkey;
	unsigned char		 spkd[SHA_DIGEST_LENGTH];
	int			 crit, dsz, spkz, rc = 0;

	*ski = NULL;
	os = X509_get_ext_d2i(x, NID_subject_key_identifier, &crit, NULL);
	if (os == NULL)
		return 1;
	if (crit != 0) {
		warnx("%s: RFC 6487 section 4.8.2: "
		    "SKI: extension not non-critical", fn);
		goto out;
	}

	d = os->data;
	dsz = os->length;

	if (dsz != SHA_DIGEST_LENGTH) {
		warnx("%s: RFC 6487 section 4.8.2: SKI: "
		    "want %d bytes SHA1 hash, have %d bytes",
		    fn, SHA_DIGEST_LENGTH, dsz);
		goto out;
	}

	if ((pubkey = X509_get_X509_PUBKEY(x)) == NULL) {
		warnx("%s: X509_get_X509_PUBKEY", fn);
		goto out;
	}
	if (!X509_PUBKEY_get0_param(NULL, &spk, &spkz, NULL, pubkey)) {
		warnx("%s: X509_PUBKEY_get0_param", fn);
		goto out;
	}

	if (!EVP_Digest(spk, spkz, spkd, NULL, EVP_sha1(), NULL)) {
		warnx("%s: EVP_Digest failed", fn);
		goto out;
	}

	if (memcmp(spkd, d, dsz) != 0) {
		warnx("%s: SKI does not match SHA1 hash of SPK", fn);
		goto out;
	}

	*ski = hex_encode(d, dsz);
	rc = 1;
 out:
	ASN1_OCTET_STRING_free(os);
	return rc;
}

/*
 * Check the certificate's purpose: CA or BGPsec Router.
 * Return a member of enum cert_purpose.
 */
enum cert_purpose
x509_get_purpose(X509 *x, const char *fn)
{
	BASIC_CONSTRAINTS		*bc = NULL;
	EXTENDED_KEY_USAGE		*eku = NULL;
	int				 crit;
	enum cert_purpose		 purpose = CERT_PURPOSE_INVALID;

	if (X509_check_ca(x) == 1) {
		bc = X509_get_ext_d2i(x, NID_basic_constraints, &crit, NULL);
		if (bc->pathlen != NULL) {
			warnx("%s: RFC 6487 section 4.8.1: Path Length "
			    "Constraint must be absent", fn);
			goto out;
		}
		purpose = CERT_PURPOSE_CA;
		goto out;
	}

	if (X509_get_extension_flags(x) & EXFLAG_BCONS) {
		warnx("%s: Basic Constraints ext in non-CA cert", fn);
		goto out;
	}

	eku = X509_get_ext_d2i(x, NID_ext_key_usage, &crit, NULL);
	if (eku == NULL) {
		warnx("%s: EKU: extension missing", fn);
		goto out;
	}
	if (crit != 0) {
		warnx("%s: EKU: extension must not be marked critical", fn);
		goto out;
	}
	if (sk_ASN1_OBJECT_num(eku) != 1) {
		warnx("%s: EKU: expected 1 purpose, have %d", fn,
		    sk_ASN1_OBJECT_num(eku));
		goto out;
	}

	if (OBJ_cmp(bgpsec_oid, sk_ASN1_OBJECT_value(eku, 0)) == 0) {
		purpose = CERT_PURPOSE_BGPSEC_ROUTER;
		goto out;
	}

 out:
	BASIC_CONSTRAINTS_free(bc);
	EXTENDED_KEY_USAGE_free(eku);
	return purpose;
}

/*
 * Extract Subject Public Key Info (SPKI) from BGPsec X.509 Certificate.
 * Returns NULL on failure, on success return the SPKI as base64 encoded pubkey
 */
char *
x509_get_pubkey(X509 *x, const char *fn)
{
	EVP_PKEY	*pkey;
	EC_KEY		*eckey;
	int		 nid;
	const char	*cname;
	uint8_t		*pubkey = NULL;
	char		*res = NULL;
	int		 len;

	pkey = X509_get0_pubkey(x);
	if (pkey == NULL) {
		warnx("%s: X509_get0_pubkey failed in %s", fn, __func__);
		goto out;
	}
	if (EVP_PKEY_base_id(pkey) != EVP_PKEY_EC) {
		warnx("%s: Expected EVP_PKEY_EC, got %d", fn,
		    EVP_PKEY_base_id(pkey));
		goto out;
	}

	eckey = EVP_PKEY_get0_EC_KEY(pkey);
	if (eckey == NULL) {
		warnx("%s: Incorrect key type", fn);
		goto out;
	}

	nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(eckey));
	if (nid != NID_X9_62_prime256v1) {
		if ((cname = EC_curve_nid2nist(nid)) == NULL)
			cname = OBJ_nid2sn(nid);
		warnx("%s: Expected P-256, got %s", fn, cname);
		goto out;
	}

	if (!EC_KEY_check_key(eckey)) {
		warnx("%s: EC_KEY_check_key failed in %s", fn, __func__);
		goto out;
	}

	len = i2d_PUBKEY(pkey, &pubkey);
	if (len <= 0) {
		warnx("%s: i2d_PUBKEY failed in %s", fn, __func__);
		goto out;
	}

	if (base64_encode(pubkey, len, &res) == -1)
		errx(1, "base64_encode failed in %s", __func__);

 out:
	free(pubkey);
	return res;
}

/*
 * Parse the Authority Information Access (AIA) extension
 * See RFC 6487, section 4.8.7 for details.
 * Returns NULL on failure, on success returns the AIA URI
 * (which has to be freed after use).
 */
int
x509_get_aia(X509 *x, const char *fn, char **aia)
{
	ACCESS_DESCRIPTION		*ad;
	AUTHORITY_INFO_ACCESS		*info;
	int				 crit, rc = 0;

	*aia = NULL;
	info = X509_get_ext_d2i(x, NID_info_access, &crit, NULL);
	if (info == NULL)
		return 1;

	if (crit != 0) {
		warnx("%s: RFC 6487 section 4.8.7: "
		    "AIA: extension not non-critical", fn);
		goto out;
	}
	if (sk_ACCESS_DESCRIPTION_num(info) != 1) {
		warnx("%s: RFC 6487 section 4.8.7: AIA: "
		    "want 1 element, have %d", fn,
		    sk_ACCESS_DESCRIPTION_num(info));
		goto out;
	}

	ad = sk_ACCESS_DESCRIPTION_value(info, 0);
	if (OBJ_obj2nid(ad->method) != NID_ad_ca_issuers) {
		warnx("%s: RFC 6487 section 4.8.7: AIA: "
		    "expected caIssuers, have %d", fn, OBJ_obj2nid(ad->method));
		goto out;
	}

	if (!x509_location(fn, "AIA: caIssuers", NULL, ad->location, aia))
		goto out;

	rc = 1;

out:
	AUTHORITY_INFO_ACCESS_free(info);
	return rc;
}

/*
 * Parse the Subject Information Access (SIA) extension
 * See RFC 6487, section 4.8.8 for details.
 * Returns NULL on failure, on success returns the SIA signedObject URI
 * (which has to be freed after use).
 */
int
x509_get_sia(X509 *x, const char *fn, char **sia)
{
	ACCESS_DESCRIPTION		*ad;
	AUTHORITY_INFO_ACCESS		*info;
	ASN1_OBJECT			*oid;
	int				 i, crit, rsync_found = 0;

	*sia = NULL;

	info = X509_get_ext_d2i(x, NID_sinfo_access, &crit, NULL);
	if (info == NULL)
		return 1;

	if (crit != 0) {
		warnx("%s: RFC 6487 section 4.8.8: "
		    "SIA: extension not non-critical", fn);
		goto out;
	}

	for (i = 0; i < sk_ACCESS_DESCRIPTION_num(info); i++) {
		ad = sk_ACCESS_DESCRIPTION_value(info, i);
		oid = ad->method;

		/*
		 * XXX: RFC 6487 4.8.8.2 states that the accessMethod MUST be
		 * signedObject. However, rpkiNotify accessMethods currently
		 * exist in the wild. Consider removing this special case.
		 * See also https://www.rfc-editor.org/errata/eid7239.
		 */
		if (OBJ_cmp(oid, notify_oid) == 0) {
			if (verbose > 1)
				warnx("%s: RFC 6487 section 4.8.8.2: SIA should"
				    " not contain rpkiNotify accessMethod", fn);
			continue;
		}
		if (OBJ_cmp(oid, signedobj_oid) != 0) {
			char buf[128];

			OBJ_obj2txt(buf, sizeof(buf), oid, 0);
			warnx("%s: RFC 6487 section 4.8.8.2: unexpected"
			    " accessMethod: %s", fn, buf);
			goto out;
		}

		/* Don't fail on non-rsync URI, so check this afterward. */
		if (!x509_location(fn, "SIA: signedObject", NULL, ad->location,
		    sia))
			goto out;

		if (rsync_found)
			continue;

		if (strncasecmp(*sia, "rsync://", 8) == 0) {
			rsync_found = 1;
			continue;
		}

		free(*sia);
		*sia = NULL;
	}

	if (!rsync_found)
		goto out;

	AUTHORITY_INFO_ACCESS_free(info);
	return 1;

 out:
	free(*sia);
	*sia = NULL;
	AUTHORITY_INFO_ACCESS_free(info);
	return 0;
}

/*
 * Extract the notBefore of a certificate.
 */
int
x509_get_notbefore(X509 *x, const char *fn, time_t *tt)
{
	const ASN1_TIME	*at;

	at = X509_get0_notBefore(x);
	if (at == NULL) {
		warnx("%s: X509_get0_notBefore failed", fn);
		return 0;
	}
	if (!x509_get_time(at, tt)) {
		warnx("%s: ASN1_time_parse failed", fn);
		return 0;
	}
	return 1;
}

/*
 * Extract the expire time (not-after) of a certificate.
 */
int
x509_get_expire(X509 *x, const char *fn, time_t *tt)
{
	const ASN1_TIME	*at;

	at = X509_get0_notAfter(x);
	if (at == NULL) {
		warnx("%s: X509_get0_notafter failed", fn);
		return 0;
	}
	if (!x509_get_time(at, tt)) {
		warnx("%s: ASN1_time_parse failed", fn);
		return 0;
	}
	return 1;
}

/*
 * Check whether all RFC 3779 extensions are set to inherit.
 * Return 1 if both AS & IP are set to inherit.
 * Return 0 on failure (such as missing extensions or no inheritance).
 */
int
x509_inherits(X509 *x)
{
	STACK_OF(IPAddressFamily)	*addrblk = NULL;
	ASIdentifiers			*asidentifiers = NULL;
	const IPAddressFamily		*af;
	int				 i, rc = 0;

	addrblk = X509_get_ext_d2i(x, NID_sbgp_ipAddrBlock, NULL, NULL);
	if (addrblk == NULL)
		goto out;

	/*
	 * Check by hand, since X509v3_addr_inherits() success only means that
	 * at least one address family inherits, not all of them.
	 */
	for (i = 0; i < sk_IPAddressFamily_num(addrblk); i++) {
		af = sk_IPAddressFamily_value(addrblk, i);
		if (af->ipAddressChoice->type != IPAddressChoice_inherit)
			goto out;
	}

	asidentifiers = X509_get_ext_d2i(x, NID_sbgp_autonomousSysNum, NULL,
	    NULL);
	if (asidentifiers == NULL)
		goto out;

	/* We need to have AS numbers and don't want RDIs. */
	if (asidentifiers->asnum == NULL || asidentifiers->rdi != NULL)
		goto out;
	if (!X509v3_asid_inherits(asidentifiers))
		goto out;

	rc = 1;
 out:
	ASIdentifiers_free(asidentifiers);
	sk_IPAddressFamily_pop_free(addrblk, IPAddressFamily_free);
	return rc;
}

/*
 * Check whether at least one RFC 3779 extension is set to inherit.
 * Return 1 if an inherit element is encountered in AS or IP.
 * Return 0 otherwise.
 */
int
x509_any_inherits(X509 *x)
{
	STACK_OF(IPAddressFamily)	*addrblk = NULL;
	ASIdentifiers			*asidentifiers = NULL;
	int				 rc = 0;

	addrblk = X509_get_ext_d2i(x, NID_sbgp_ipAddrBlock, NULL, NULL);
	if (X509v3_addr_inherits(addrblk))
		rc = 1;

	asidentifiers = X509_get_ext_d2i(x, NID_sbgp_autonomousSysNum, NULL,
	    NULL);
	if (X509v3_asid_inherits(asidentifiers))
		rc = 1;

	ASIdentifiers_free(asidentifiers);
	sk_IPAddressFamily_pop_free(addrblk, IPAddressFamily_free);
	return rc;
}

/*
 * Parse the very specific subset of information in the CRL distribution
 * point extension.
 * See RFC 6487, section 4.8.6 for details.
 * Returns NULL on failure, the crl URI on success which has to be freed
 * after use.
 */
int
x509_get_crl(X509 *x, const char *fn, char **crl)
{
	CRL_DIST_POINTS		*crldp;
	DIST_POINT		*dp;
	GENERAL_NAMES		*names;
	GENERAL_NAME		*name;
	int			 i, crit, rsync_found = 0;

	*crl = NULL;
	crldp = X509_get_ext_d2i(x, NID_crl_distribution_points, &crit, NULL);
	if (crldp == NULL)
		return 1;

	if (crit != 0) {
		warnx("%s: RFC 6487 section 4.8.6: "
		    "CRL distribution point: extension not non-critical", fn);
		goto out;
	}

	if (sk_DIST_POINT_num(crldp) != 1) {
		warnx("%s: RFC 6487 section 4.8.6: CRL: "
		    "want 1 element, have %d", fn,
		    sk_DIST_POINT_num(crldp));
		goto out;
	}

	dp = sk_DIST_POINT_value(crldp, 0);
	if (dp->CRLissuer != NULL) {
		warnx("%s: RFC 6487 section 4.8.6: CRL CRLIssuer field"
		    " disallowed", fn);
		goto out;
	}
	if (dp->reasons != NULL) {
		warnx("%s: RFC 6487 section 4.8.6: CRL Reasons field"
		    " disallowed", fn);
		goto out;
	}
	if (dp->distpoint == NULL) {
		warnx("%s: RFC 6487 section 4.8.6: CRL: "
		    "no distribution point name", fn);
		goto out;
	}
	if (dp->distpoint->dpname != NULL) {
		warnx("%s: RFC 6487 section 4.8.6: nameRelativeToCRLIssuer"
		    " disallowed", fn);
		goto out;
	}
	if (dp->distpoint->type != 0) {
		warnx("%s: RFC 6487 section 4.8.6: CRL: "
		    "expected GEN_OTHERNAME, have %d", fn, dp->distpoint->type);
		goto out;
	}

	names = dp->distpoint->name.fullname;
	for (i = 0; i < sk_GENERAL_NAME_num(names); i++) {
		name = sk_GENERAL_NAME_value(names, i);

		/* Don't fail on non-rsync URI, so check this afterward. */
		if (!x509_location(fn, "CRL distribution point", NULL, name,
		    crl))
			goto out;

		if (strncasecmp(*crl, "rsync://", 8) == 0) {
			rsync_found = 1;
			goto out;
		}

		free(*crl);
		*crl = NULL;
	}

	warnx("%s: RFC 6487 section 4.8.6: no rsync URI "
	    "in CRL distributionPoint", fn);

 out:
	CRL_DIST_POINTS_free(crldp);
	return rsync_found;
}

/*
 * Parse X509v3 authority key identifier (AKI) from the CRL.
 * This is matched against the string from x509_get_ski() above.
 * Returns the AKI or NULL if it could not be parsed.
 * The AKI is formatted as a hex string.
 */
char *
x509_crl_get_aki(X509_CRL *crl, const char *fn)
{
	const unsigned char	*d;
	AUTHORITY_KEYID		*akid;
	ASN1_OCTET_STRING	*os;
	int			 dsz, crit;
	char			*res = NULL;

	akid = X509_CRL_get_ext_d2i(crl, NID_authority_key_identifier, &crit,
	    NULL);
	if (akid == NULL) {
		warnx("%s: RFC 6487 section 4.8.3: AKI: extension missing", fn);
		return NULL;
	}
	if (crit != 0) {
		warnx("%s: RFC 6487 section 4.8.3: "
		    "AKI: extension not non-critical", fn);
		goto out;
	}
	if (akid->issuer != NULL || akid->serial != NULL) {
		warnx("%s: RFC 6487 section 4.8.3: AKI: "
		    "authorityCertIssuer or authorityCertSerialNumber present",
		    fn);
		goto out;
	}

	os = akid->keyid;
	if (os == NULL) {
		warnx("%s: RFC 6487 section 4.8.3: AKI: "
		    "Key Identifier missing", fn);
		goto out;
	}

	d = os->data;
	dsz = os->length;

	if (dsz != SHA_DIGEST_LENGTH) {
		warnx("%s: RFC 6487 section 4.8.2: AKI: "
		    "want %d bytes SHA1 hash, have %d bytes",
		    fn, SHA_DIGEST_LENGTH, dsz);
		goto out;
	}

	res = hex_encode(d, dsz);
out:
	AUTHORITY_KEYID_free(akid);
	return res;
}

/*
 * Convert passed ASN1_TIME to time_t *t.
 * Returns 1 on success and 0 on failure.
 */
int
x509_get_time(const ASN1_TIME *at, time_t *t)
{
	struct tm	 tm;

	*t = 0;
	memset(&tm, 0, sizeof(tm));
	if (ASN1_time_parse(at->data, at->length, &tm, 0) == -1)
		return 0;
	if ((*t = timegm(&tm)) == -1)
		errx(1, "timegm failed");
	return 1;
}

/*
 * Extract and validate an accessLocation, RFC 6487, 4.8 and RFC 8182, 3.2.
 * Returns 0 on failure and 1 on success.
 */
int
x509_location(const char *fn, const char *descr, const char *proto,
    GENERAL_NAME *location, char **out)
{
	ASN1_IA5STRING	*uri;

	if (location->type != GEN_URI) {
		warnx("%s: RFC 6487 section 4.8: %s not URI", fn, descr);
		return 0;
	}

	uri = location->d.uniformResourceIdentifier;

	if (!valid_uri(uri->data, uri->length, proto)) {
		warnx("%s: RFC 6487 section 4.8: %s bad location", fn, descr);
		return 0;
	}

	if (*out != NULL) {
		warnx("%s: RFC 6487 section 4.8: multiple %s specified, "
		    "using the first one", fn, descr);
		return 1;
	}

	if ((*out = strndup(uri->data, uri->length)) == NULL)
		err(1, NULL);

	return 1;
}

/*
 * Convert an ASN1_INTEGER into a hexstring.
 * Returned string needs to be freed by the caller.
 */
char *
x509_convert_seqnum(const char *fn, const ASN1_INTEGER *i)
{
	BIGNUM	*seqnum = NULL;
	char	*s = NULL;

	if (i == NULL)
		goto out;

	seqnum = ASN1_INTEGER_to_BN(i, NULL);
	if (seqnum == NULL) {
		warnx("%s: ASN1_INTEGER_to_BN error", fn);
		goto out;
	}

	if (BN_is_negative(seqnum)) {
		warnx("%s: %s: want positive integer, have negative.",
		    __func__, fn);
		goto out;
	}

	if (BN_num_bytes(seqnum) > 20) {
		warnx("%s: %s: want 20 octets or fewer, have more.",
		    __func__, fn);
		goto out;
	}

	s = BN_bn2hex(seqnum);
	if (s == NULL)
		warnx("%s: BN_bn2hex error", fn);

 out:
	BN_free(seqnum);
	return s;
}
