#include "builtin.h"
#include "config.h"
#include "gettext.h"
#include "hex.h"
#include "refs.h"
#include "object-name.h"
#include "object-store-ll.h"
#include "object.h"
#include "tag.h"
#include "string-list.h"
#include "parse-options.h"

static const char * const show_ref_usage[] = {
	N_("git show-ref [-q | --quiet] [--verify] [--head] [-d | --dereference]\n"
	   "             [-s | --hash[=<n>]] [--abbrev[=<n>]] [--tags]\n"
	   "             [--heads] [--] [<pattern>...]"),
	N_("git show-ref --exclude-existing[=<pattern>]"),
	NULL
};

static int deref_tags, show_head, tags_only, heads_only, verify,
	   quiet, hash_only, abbrev;

static void show_one(const char *refname, const struct object_id *oid)
{
	const char *hex;
	struct object_id peeled;

	if (!repo_has_object_file(the_repository, oid))
		die("git show-ref: bad ref %s (%s)", refname,
		    oid_to_hex(oid));

	if (quiet)
		return;

	hex = repo_find_unique_abbrev(the_repository, oid, abbrev);
	if (hash_only)
		printf("%s\n", hex);
	else
		printf("%s %s\n", hex, refname);

	if (!deref_tags)
		return;

	if (!peel_iterated_oid(oid, &peeled)) {
		hex = repo_find_unique_abbrev(the_repository, &peeled, abbrev);
		printf("%s %s^{}\n", hex, refname);
	}
}

struct show_ref_data {
	const char **patterns;
	int found_match;
};

static int show_ref(const char *refname, const struct object_id *oid,
		    int flag UNUSED, void *cbdata)
{
	struct show_ref_data *data = cbdata;

	if (show_head && !strcmp(refname, "HEAD"))
		goto match;

	if (data->patterns) {
		int reflen = strlen(refname);
		const char **p = data->patterns, *m;
		while ((m = *p++) != NULL) {
			int len = strlen(m);
			if (len > reflen)
				continue;
			if (memcmp(m, refname + reflen - len, len))
				continue;
			if (len == reflen)
				goto match;
			if (refname[reflen - len - 1] == '/')
				goto match;
		}
		return 0;
	}

match:
	data->found_match++;

	show_one(refname, oid);

	return 0;
}

static int add_existing(const char *refname,
			const struct object_id *oid UNUSED,
			int flag UNUSED, void *cbdata)
{
	struct string_list *list = (struct string_list *)cbdata;
	string_list_insert(list, refname);
	return 0;
}

struct exclude_existing_options {
	/*
	 * We need an explicit `enabled` field because it is perfectly valid
	 * for `pattern` to be `NULL` even if `--exclude-existing` was given.
	 */
	int enabled;
	const char *pattern;
};

/*
 * read "^(?:<anything>\s)?<refname>(?:\^\{\})?$" from the standard input,
 * and
 * (1) strip "^{}" at the end of line if any;
 * (2) ignore if match is provided and does not head-match refname;
 * (3) warn if refname is not a well-formed refname and skip;
 * (4) ignore if refname is a ref that exists in the local repository;
 * (5) otherwise output the line.
 */
static int cmd_show_ref__exclude_existing(const struct exclude_existing_options *opts)
{
	struct string_list existing_refs = STRING_LIST_INIT_DUP;
	char buf[1024];
	int patternlen = opts->pattern ? strlen(opts->pattern) : 0;

	for_each_ref(add_existing, &existing_refs);
	while (fgets(buf, sizeof(buf), stdin)) {
		char *ref;
		int len = strlen(buf);

		if (len > 0 && buf[len - 1] == '\n')
			buf[--len] = '\0';
		if (3 <= len && !strcmp(buf + len - 3, "^{}")) {
			len -= 3;
			buf[len] = '\0';
		}
		for (ref = buf + len; buf < ref; ref--)
			if (isspace(ref[-1]))
				break;
		if (opts->pattern) {
			int reflen = buf + len - ref;
			if (reflen < patternlen)
				continue;
			if (strncmp(ref, opts->pattern, patternlen))
				continue;
		}
		if (check_refname_format(ref, 0)) {
			warning("ref '%s' ignored", ref);
			continue;
		}
		if (!string_list_has_string(&existing_refs, ref)) {
			printf("%s\n", buf);
		}
	}

	string_list_clear(&existing_refs, 0);
	return 0;
}

static int cmd_show_ref__verify(const char **refs)
{
	if (!refs || !*refs)
		die("--verify requires a reference");

	while (*refs) {
		struct object_id oid;

		if ((starts_with(*refs, "refs/") || !strcmp(*refs, "HEAD")) &&
		    !read_ref(*refs, &oid)) {
			show_one(*refs, &oid);
		}
		else if (!quiet)
			die("'%s' - not a valid ref", *refs);
		else
			return 1;
		refs++;
	}

	return 0;
}

static int cmd_show_ref__patterns(const char **patterns)
{
	struct show_ref_data show_ref_data = {0};

	if (patterns && *patterns)
		show_ref_data.patterns = patterns;

	if (show_head)
		head_ref(show_ref, &show_ref_data);
	if (heads_only || tags_only) {
		if (heads_only)
			for_each_fullref_in("refs/heads/", show_ref, &show_ref_data);
		if (tags_only)
			for_each_fullref_in("refs/tags/", show_ref, &show_ref_data);
	} else {
		for_each_ref(show_ref, &show_ref_data);
	}
	if (!show_ref_data.found_match)
		return 1;

	return 0;
}

static int hash_callback(const struct option *opt, const char *arg, int unset)
{
	hash_only = 1;
	/* Use full length SHA1 if no argument */
	if (!arg)
		return 0;
	return parse_opt_abbrev_cb(opt, arg, unset);
}

static int exclude_existing_callback(const struct option *opt, const char *arg,
				     int unset)
{
	struct exclude_existing_options *opts = opt->value;
	BUG_ON_OPT_NEG(unset);
	opts->enabled = 1;
	opts->pattern = arg;
	return 0;
}

int cmd_show_ref(int argc, const char **argv, const char *prefix)
{
	struct exclude_existing_options exclude_existing_opts = {0};
	const struct option show_ref_options[] = {
		OPT_BOOL(0, "tags", &tags_only, N_("only show tags (can be combined with heads)")),
		OPT_BOOL(0, "heads", &heads_only, N_("only show heads (can be combined with tags)")),
		OPT_BOOL(0, "verify", &verify, N_("stricter reference checking, "
			    "requires exact ref path")),
		OPT_HIDDEN_BOOL('h', NULL, &show_head,
				N_("show the HEAD reference, even if it would be filtered out")),
		OPT_BOOL(0, "head", &show_head,
		  N_("show the HEAD reference, even if it would be filtered out")),
		OPT_BOOL('d', "dereference", &deref_tags,
			    N_("dereference tags into object IDs")),
		OPT_CALLBACK_F('s', "hash", &abbrev, N_("n"),
			       N_("only show SHA1 hash using <n> digits"),
			       PARSE_OPT_OPTARG, &hash_callback),
		OPT__ABBREV(&abbrev),
		OPT__QUIET(&quiet,
			   N_("do not print results to stdout (useful with --verify)")),
		OPT_CALLBACK_F(0, "exclude-existing", &exclude_existing_opts,
			       N_("pattern"), N_("show refs from stdin that aren't in local repository"),
			       PARSE_OPT_OPTARG | PARSE_OPT_NONEG, exclude_existing_callback),
		OPT_END()
	};

	git_config(git_default_config, NULL);

	argc = parse_options(argc, argv, prefix, show_ref_options,
			     show_ref_usage, 0);

	if (exclude_existing_opts.enabled)
		return cmd_show_ref__exclude_existing(&exclude_existing_opts);
	else if (verify)
		return cmd_show_ref__verify(argv);
	else
		return cmd_show_ref__patterns(argv);
}
