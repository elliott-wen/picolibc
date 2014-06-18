/* ldap.cc: Helper functions for ldap access to Active Directory.

   Copyright 2014 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include "ldap.h"
#include "cygerrno.h"
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "registry.h"
#include "pinfo.h"
#include "lm.h"
#include "dsgetdc.h"
#include "tls_pbuf.h"

#define CYG_LDAP_TIMEOUT	30	/* seconds */

static LDAP_TIMEVAL tv = { CYG_LDAP_TIMEOUT, 0 };

static PWCHAR rootdse_attr[] =
{
  (PWCHAR) L"defaultNamingContext",
  (PWCHAR) L"supportedCapabilities",
  NULL
};

static PWCHAR user_attr[] =
{
  (PWCHAR) L"primaryGroupID",
  (PWCHAR) L"gecos",
  (PWCHAR) L"unixHomeDirectory",
  (PWCHAR) L"loginShell",
  (PWCHAR) L"uidNumber",
  NULL
};

static PWCHAR group_attr[] =
{
  (PWCHAR) L"cn",
  (PWCHAR) L"gidNumber",
  NULL
};

PWCHAR tdom_attr[] =
{
  (PWCHAR) L"trustPosixOffset",
  NULL
};

PWCHAR sid_attr[] =
{
  (PWCHAR) L"objectSid",
  NULL
};

PWCHAR rfc2307_uid_attr[] =
{
  (PWCHAR) L"uid",
  NULL
};

PWCHAR rfc2307_gid_attr[] =
{
  (PWCHAR) L"cn",
  NULL
};

bool
cyg_ldap::connect_ssl (PCWSTR domain)
{
  ULONG ret, timelimit = CYG_LDAP_TIMEOUT;

  if (!(lh = ldap_sslinitW ((PWCHAR) domain, LDAP_SSL_PORT, 1)))
    {
      debug_printf ("ldap_init(%W) error 0x%02x", domain, LdapGetLastError ());
      return false;
    }
  if ((ret = ldap_set_option (lh, LDAP_OPT_TIMELIMIT, &timelimit))
      != LDAP_SUCCESS)
    debug_printf ("ldap_set_option(LDAP_OPT_TIMELIMIT) error 0x%02x", ret);
  if ((ret = ldap_bind_s (lh, NULL, NULL, LDAP_AUTH_NEGOTIATE)) != LDAP_SUCCESS)
    {
      debug_printf ("ldap_bind(%W) 0x%02x", domain, ret);
      ldap_unbind (lh);
      lh = NULL;
      return false;
    }
  return true;
}

bool
cyg_ldap::connect_non_ssl (PCWSTR domain)
{
  ULONG ret, timelimit = CYG_LDAP_TIMEOUT;

  if (!(lh = ldap_initW ((PWCHAR) domain, LDAP_PORT)))
    {
      debug_printf ("ldap_init(%W) error 0x%02x", domain, LdapGetLastError ());
      return false;
    }
  if ((ret = ldap_set_option (lh, LDAP_OPT_SIGN, LDAP_OPT_ON))
      != LDAP_SUCCESS)
    debug_printf ("ldap_set_option(LDAP_OPT_SIGN) error 0x%02x", ret);
  if ((ret = ldap_set_option (lh, LDAP_OPT_ENCRYPT, LDAP_OPT_ON))
      != LDAP_SUCCESS)
    debug_printf ("ldap_set_option(LDAP_OPT_ENCRYPT) error 0x%02x", ret);
  if ((ret = ldap_set_option (lh, LDAP_OPT_TIMELIMIT, &timelimit))
      != LDAP_SUCCESS)
    debug_printf ("ldap_set_option(LDAP_OPT_TIMELIMIT) error 0x%02x", ret);
  if ((ret = ldap_bind_s (lh, NULL, NULL, LDAP_AUTH_NEGOTIATE)) != LDAP_SUCCESS)
    {
      debug_printf ("ldap_bind(%W) 0x%02x", domain, ret);
      ldap_unbind (lh);
      lh = NULL;
      return false;
    }
  return true;
}

bool
cyg_ldap::open (PCWSTR domain)
{
  ULONG ret;

  /* Already open? */
  if (lh)
    return true;

  /* FIXME?  connect_ssl can take ages even when failing, so we're trying to
     do everything the non-SSL (but still encrypted) way. */
  if (/*!connect_ssl (NULL) && */ !connect_non_ssl (domain))
    return false;
  if ((ret = ldap_search_stW (lh, NULL, LDAP_SCOPE_BASE,
			      (PWCHAR) L"(objectclass=*)", rootdse_attr,
			      0, &tv, &msg))
      != LDAP_SUCCESS)
    {
      debug_printf ("ldap_search(%W, ROOTDSE) error 0x%02x", domain, ret);
      goto err;
    }
  if (!(entry = ldap_first_entry (lh, msg)))
    {
      debug_printf ("No ROOTDSE entry for %W", domain);
      goto err;
    }
  if (!(val = ldap_get_valuesW (lh, entry, rootdse_attr[0])))
    {
      debug_printf ("No ROOTDSE value for %W", domain);
      goto err;
    }
  if (!(rootdse = wcsdup (val[0])))
    {
      debug_printf ("wcsdup(%W, ROOTDSE) %d", domain, get_errno ());
      goto err;
    }
  ldap_value_freeW (val);
  if ((val = ldap_get_valuesW (lh, entry, rootdse_attr[1])))
    {
      for (ULONG idx = 0; idx < ldap_count_valuesW (val); ++idx)
	if (!wcscmp (val[idx], LDAP_CAP_ACTIVE_DIRECTORY_OID_W))
	  {
	    isAD = true;
	    break;
	  }
    }
  ldap_value_freeW (val);
  val = NULL;
  ldap_msgfree (msg);
  msg = entry = NULL; return true;
err:
  close ();
  return false;
}

void
cyg_ldap::close ()
{
  if (srch_id != NULL)
    ldap_search_abandon_page (lh, srch_id);
  if (lh)
    ldap_unbind (lh);
  if (srch_msg)
    ldap_msgfree (srch_msg);
  if (msg)
    ldap_msgfree (msg);
  if (val)
    ldap_value_freeW (val);
  if (rootdse)
    free (rootdse);
  lh = NULL;
  msg = entry = NULL;
  val = NULL;
  rootdse = NULL;
  srch_id = NULL;
  srch_msg = srch_entry = NULL;
}

bool
cyg_ldap::fetch_ad_account (PSID sid, bool group, PCWSTR domain)
{
  WCHAR filter[140], *f, *rdse = rootdse;
  LONG len = (LONG) RtlLengthSid (sid);
  PBYTE s = (PBYTE) sid;
  static WCHAR hex_wchars[] = L"0123456789abcdef";
  ULONG ret;
  tmp_pathbuf tp;

  if (msg)
    {
      ldap_msgfree (msg);
      msg = entry = NULL;
    }
  if (val)
    {
      ldap_value_freeW (val);
      val = NULL;
    }
  f = wcpcpy (filter, L"(objectSid=");
  while (len-- > 0)
    {
      *f++ = L'\\';
      *f++ = hex_wchars[*s >> 4];
      *f++ = hex_wchars[*s++ & 0xf];
    }
  wcpcpy (f, L")");
  if (domain)
    {
      /* FIXME:  This is a hack.  The most correct solution is probably to
         open a connection to the DC of the trusted domain.  But this always
	 takes extra time, so we're trying to avoid it.  If this results in
	 problems, we know what to do. */
      rdse = tp.w_get ();
      PWCHAR r = rdse;
      for (PWCHAR dotp = (PWCHAR) domain; dotp && *dotp; domain = dotp)
	{
	  dotp = wcschr (domain, L'.');
	  if (dotp)
	    *dotp++ = L'\0';
	  if (r > rdse)
	    *r++ = L',';
	  r = wcpcpy (r, L"DC=");
	  r = wcpcpy (r, domain);
	}
    }
  attr = group ? group_attr : user_attr;
  if ((ret = ldap_search_stW (lh, rdse, LDAP_SCOPE_SUBTREE, filter,
			      attr, 0, &tv, &msg)) != LDAP_SUCCESS)
    {
      debug_printf ("ldap_search_stW(%W,%W) error 0x%02x",
		    rdse, filter, ret);
      return false;
    }
  if (!(entry = ldap_first_entry (lh, msg)))
    {
      debug_printf ("No entry for %W in rootdse %W", filter, rdse);
      return false;
    }
  return true;
}

bool
cyg_ldap::enumerate_ad_accounts (PCWSTR domain, bool group)
{
  tmp_pathbuf tp;
  PCWSTR filter;

  close ();
  if (!open (domain))
    return false;

  if (!group)
    filter = L"(&(objectClass=User)"
	        "(objectCategory=Person)"
		/* 512 == ADS_UF_NORMAL_ACCOUNT */
	        "(userAccountControl:" LDAP_MATCHING_RULE_BIT_AND ":=512)"
	        "(objectSid=*))";
  else if (!domain)
    filter = L"(&(objectClass=Group)"
		"(objectSid=*))";
  else
    filter = L"(&(objectClass=Group)"
		/* 1 == ACCOUNT_GROUP */
		"(!(groupType:" LDAP_MATCHING_RULE_BIT_AND ":=1))"
		"(objectSid=*))";
  srch_id = ldap_search_init_pageW (lh, rootdse, LDAP_SCOPE_SUBTREE,
				    (PWCHAR) filter, sid_attr, 0,
				    NULL, NULL, CYG_LDAP_TIMEOUT, 100, NULL);
  if (srch_id == NULL)
    {
      debug_printf ("ldap_search_init_pageW(%W,%W) error 0x%02x",
		    rootdse, filter, LdapGetLastError ());
      return false;
    }
  return true;
}

bool
cyg_ldap::next_account (cygsid &sid)
{
  ULONG ret;
  PLDAP_BERVAL *bval;

  ULONG total;

  if (srch_entry)
    {
      if ((srch_entry = ldap_next_entry (lh, srch_entry))
	  && (bval = ldap_get_values_lenW (lh, srch_entry, sid_attr[0])))
	{
	  sid = (PSID) bval[0]->bv_val;
	  ldap_value_free_len (bval);
	  return true;
	}
      ldap_msgfree (srch_msg);
      srch_msg = srch_entry = NULL;
    }
  do
    {
      ret = ldap_get_next_page_s (lh, srch_id, &tv, 100, &total, &srch_msg);
    }
  while (ret == LDAP_SUCCESS && ldap_count_entries (lh, srch_msg) == 0);
  if (ret == LDAP_NO_RESULTS_RETURNED)
    ;
  else if (ret != LDAP_SUCCESS)
    debug_printf ("ldap_result() error 0x%02x", ret);
  else if ((srch_entry = ldap_first_entry (lh, srch_msg))
	   && (bval = ldap_get_values_lenW (lh, srch_entry, sid_attr[0])))
    {
      sid = (PSID) bval[0]->bv_val;
      ldap_value_free_len (bval);
      return true;
    }
  ldap_search_abandon_page (lh, srch_id);
  srch_id = NULL;
  return false;
}

uint32_t
cyg_ldap::fetch_posix_offset_for_domain (PCWSTR domain)
{
  WCHAR filter[300];
  ULONG ret;

  if (msg)
    {
      ldap_msgfree (msg);
      msg = entry = NULL;
    }
  if (val)
    {
      ldap_value_freeW (val);
      val = NULL;
    }
  /* If domain name has no dot, it's a Netbios name.  In that case, filter
     by flatName rather than by name. */
  __small_swprintf (filter, L"(&(objectClass=trustedDomain)(%W=%W))",
		    wcschr (domain, L'.') ? L"name" : L"flatName", domain);
  if ((ret = ldap_search_stW (lh, rootdse, LDAP_SCOPE_SUBTREE, filter,
			      attr = tdom_attr, 0, &tv, &msg)) != LDAP_SUCCESS)
    {
      debug_printf ("ldap_search_stW(%W,%W) error 0x%02x",
		    rootdse, filter, ret);
      return 0;
    }
  if (!(entry = ldap_first_entry (lh, msg)))
    {
      debug_printf ("No entry for %W in rootdse %W", filter, rootdse);
      return 0;
    }
  return get_num_attribute (0);
}

PWCHAR
cyg_ldap::get_string_attribute (int idx)
{
  if (val)
    ldap_value_freeW (val);
  val = ldap_get_valuesW (lh, entry, attr[idx]);
  if (val)
    return val[0];
  return NULL;
}

uint32_t
cyg_ldap::get_num_attribute (int idx)
{
  PWCHAR ret = get_string_attribute (idx);
  if (ret)
    return (uint32_t) wcstoul (ret, NULL, 10);
  return (uint32_t) -1;
}

bool
cyg_ldap::fetch_unix_sid_from_ad (uint32_t id, cygsid &sid, bool group)
{
  WCHAR filter[48];
  ULONG ret;
  PLDAP_BERVAL *bval;

  if (msg)
    {
      ldap_msgfree (msg);
      msg = entry = NULL;
    }
  if (group)
    __small_swprintf (filter, L"(&(objectClass=Group)(gidNumber=%u))", id);
  else
    __small_swprintf (filter, L"(&(objectClass=User)(uidNumber=%u))", id);
  if ((ret = ldap_search_stW (lh, rootdse, LDAP_SCOPE_SUBTREE, filter,
			      sid_attr, 0, &tv, &msg)) != LDAP_SUCCESS)
    {
      debug_printf ("ldap_search_stW(%W,%W) error 0x%02x",
		    rootdse, filter, ret);
      return false;
    }
  if ((entry = ldap_first_entry (lh, msg))
      && (bval = ldap_get_values_lenW (lh, entry, sid_attr[0])))
    {
      sid = (PSID) bval[0]->bv_val;
      ldap_value_free_len (bval);
      return true;
    }
  return false;
}

PWCHAR
cyg_ldap::fetch_unix_name_from_rfc2307 (uint32_t id, bool group)
{
  WCHAR filter[52];
  ULONG ret;

  if (msg)
    {
      ldap_msgfree (msg);
      msg = entry = NULL;
    }
  if (val)
    {
      ldap_value_freeW (val);
      val = NULL;
    }
  attr = group ? rfc2307_gid_attr : rfc2307_uid_attr;
  if (group)
    __small_swprintf (filter, L"(&(objectClass=posixGroup)(gidNumber=%u))", id);
  else
    __small_swprintf (filter, L"(&(objectClass=posixAccount)(uidNumber=%u))",
		      id);
  if ((ret = ldap_search_stW (lh, rootdse, LDAP_SCOPE_SUBTREE, filter, attr,
			      0, &tv, &msg)) != LDAP_SUCCESS)
    {
      debug_printf ("ldap_search_stW(%W,%W) error 0x%02x",
		    rootdse, filter, ret);
      return NULL;
    }
  if (!(entry = ldap_first_entry (lh, msg)))
    {
      debug_printf ("No entry for %W in rootdse %W", filter, rootdse);
      return NULL;
    }
  return get_string_attribute (0);
}

uid_t
cyg_ldap::remap_uid (uid_t uid)
{
  cygsid user (NO_SID);
  PWCHAR name;
  struct passwd *pw;

  if (isAD)
    {
      if (fetch_unix_sid_from_ad (uid, user, false)
	  && user != NO_SID
	  && (pw = internal_getpwsid (user, this)))
	return pw->pw_uid;
    }
  else if ((name = fetch_unix_name_from_rfc2307 (uid, false)))
    {
      char *mbname = NULL;
      sys_wcstombs_alloc (&mbname, HEAP_NOTHEAP, name);
      if ((pw = internal_getpwnam (mbname)))
	return pw->pw_uid;
    }
  return ILLEGAL_UID;
}

gid_t
cyg_ldap::remap_gid (gid_t gid)
{
  cygsid group (NO_SID);
  PWCHAR name;
  struct group *gr;

  if (isAD)
    {
      if (fetch_unix_sid_from_ad (gid, group, true)
	  && group != NO_SID
	  && (gr = internal_getgrsid (group, this)))
	return gr->gr_gid;
    }
  else if ((name = fetch_unix_name_from_rfc2307 (gid, true)))
    {
      char *mbname = NULL;
      sys_wcstombs_alloc (&mbname, HEAP_NOTHEAP, name);
      if ((gr = internal_getgrnam (mbname)))
	return gr->gr_gid;
    }
  return ILLEGAL_GID;
}