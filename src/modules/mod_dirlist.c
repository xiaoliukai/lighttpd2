/*
 * mod_dirlist - directory listing
 *
 * Description:
 *     mod_dirlist enables you to list files inside a directory.
 *     The output can be customized in various ways from style via css to excluding certain entries.
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     dirlist [options] - show directory listing
 *         options: optional (not required), array, can contain any of the following string => value pairs:
 *             "sort" => criterium             - string, one of "name", "size" or "type"
 *             "css" => url                    - string, external css to use for styling, default: use internal css
 *
 *             "hide-dotfiles" => bool         - hide entries beginning with a dot, default: true
 *             "hide-tildefiles" => bool       - hide entries ending with a tilde (~), often used for backups, default: true
 *             "hide-directories" => bool      - hide directories from the directory listing, default: false
 *
 *             "include-header" => bool        - include HEADER.txt above the directory listing, default: false
 *             "hide-header" => bool           - hide HEADER.txt from the directory listing, default: false
 *             "encode-header" => bool         - html-encode HEADER.txt (if included), set to false if it contains real HTML, default: true
 *
 *             "include-readme" => bool        - include README.txt below the directory listing, default: true
 *             "hide-readme" => bool           - hide README.txt from the directory listing, default: false
 *             "encode-readme" => bool         - html-encode README.txt (if included), set to false if it contains real HTML, default: true
 *
 *             "exclude-suffix" => suffixlist  - list of strings, filter entries that end with one of the strings supplied
 *             "exclude-prefix" => prefixlist  - list of strings, filter entries that begin with one of the strings supplied
 *
 *             "debug" => bool                 - outout debug information to log, default: false
 *
 * Example config:
 *     if req.path =^ "/files/" {
 *         dirlist ("include-header" => true, "hide-header" => true, "hide->suffix" => (".bak"));
 *     }
 *         - shows a directory listing including the content of HEADER.txt above the list and hiding itself from it
 *           also hides all files ending in ".bak"
 *
 * Tip:
 *     xyz
 *
 * Todo:
 *     - make output generating "async", give up control every N entries
 *     - filters for entries (pattern, regex)
 *     - include-* parameters
 *     - javascript for sorting
 *     - sort parameter
 *     - parameter to chose if dirs should be seperated from other files (listed first)
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>
#include <lighttpd/encoding.h>

#include <sys/stat.h>
#include <fcntl.h>

/* maximum filesize for HEADER.txt and README.txt to have them included */
#define MAX_INCLUDE_FILE_SIZE (64*1024)

LI_API gboolean mod_dirlist_init(liModules *mods, liModule *mod);
LI_API gboolean mod_dirlist_free(liModules *mods, liModule *mod);

/* html snippet constants */
static const gchar html_header_start[] =
	"<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
	"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
	"         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
	"<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
	"	<head>\n"
	"		<title>Index of %s</title>\n";
	
static const gchar html_header_end[] =
	"	</head>\n"
	"	<body>\n";

static const gchar html_table_start[] =
	"		<h2 id=\"title\">Index of %s</h2>\n"
	"		<div id=\"dirlist\">\n"
	"			<table summary=\"Directory Listing\" cellpadding=\"0\" cellspacing=\"0\">\n"
	"				<thead><tr><th id=\"name\">Name</th><th id=\"modified\">Last Modified</th><th id=\"size\">Size</th><th id=\"type\">Type</th></tr></thead>\n"
	"				<tbody>\n";

static const gchar html_table_row[] =
	"				<tr><td><a href=\"%s\">%s</a></td>"
	"<td class=\"modified\" val=\"%"G_GINT64_FORMAT"\">%s</td>"
	"<td class=\"size\" val=\"%"G_GINT64_FORMAT"\">%s</td>"
	"<td class=\"type\">%s</td></tr>\n";

static const gchar html_table_end[] =
	"				</tbody>\n"
	"			</table>\n"
	"		</div>\n";

static const gchar html_footer[] =
	"	<div id=\"footer\">%s</div>\n"
	"	</body>\n"
	"</html>";

static const gchar html_css[] =
	"<style type=\"text/css\">\n"
	"	body { background-color: #F5F5F5; }\n"
	"	h2#title { margin-bottom: 12px; }\n"
	"	a, a:active { text-decoration: none; color: blue; }\n"
	"	a:visited { color: #48468F; }\n"
	"	a:hover, a:focus { text-decoration: underline; color: red; }\n"
	"	table { margin-left: 12px; }\n"
	"	th, td { font: 90% monospace; text-align: left; }\n"
	"	th { font-weight: bold; padding-right: 14px; padding-bottom: 3px; }\n"
	"	td { padding-right: 14px; }\n"
	"	td.size, th#size { text-align: right; }\n"
	"	#dirlist { background-color: white; border-top: 1px solid #646464; border-bottom: 1px solid #646464; padding-top: 10px; padding-bottom: 14px; }\n"
	"	div#footer { font: 90% monospace; color: #787878; padding-top: 4px; }\n"
	"</style>\n";

struct dirlist_data {
	liPlugin *plugin;
	GString *css;
	gboolean hide_dotfiles;
	gboolean hide_tildefiles;
	gboolean include_header, hide_header, encode_header;
	gboolean include_readme, hide_readme, encode_readme;
	gboolean hide_directories;
	gboolean debug;
	GPtrArray *exclude_suffix;
	GPtrArray *exclude_prefix;
	GString *content_type;
};
typedef struct dirlist_data dirlist_data;

struct dirlist_plugin_data {
	void *unused;
};
typedef struct dirlist_plugin_data dirlist_plugin_data;

/** uses/modifies wrk->tmp_str */
static void try_append_file(liVRequest *vr, GString **curbuf, const gchar *filename, gboolean encode_html) {
	GString *f = vr->wrk->tmp_str;
	g_string_truncate(f, 0);
	g_string_append_len(f, GSTR_LEN(vr->physical.path));
	li_path_append_slash(f);
	g_string_append(f, filename);

	if (!encode_html) {
		int fd;
		struct stat st;

		while (-1 == (fd = open(f->str, O_RDONLY))) {
			if (errno == EINTR)
				continue;
			return; /* failed to open, ignore */
		}
		if (-1 == fstat(fd, &st)) {
			close(fd);
			return; /* failed to open, ignore */
		}
		if (st.st_size > MAX_INCLUDE_FILE_SIZE) {
			close(fd);
			return; /* file too big, ignore */
		}

		/* flush current buffer and append file */
		li_chunkqueue_append_string(vr->out, *curbuf);
		*curbuf = g_string_sized_new(4*1024-1);
		li_chunkqueue_append_file_fd(vr->out, NULL, 0, st.st_size, fd);
	} else {
		GError *error = NULL;
		gchar *contents;
		gsize length;

		if (!g_file_get_contents(f->str, &contents, &length, &error)) {
			g_error_free(error);
			return; /* ignore errors */
		}
		if (length > MAX_INCLUDE_FILE_SIZE) {
			g_free(contents);
			return; /* file too big, ignore */
		}

		g_string_append_len(*curbuf, CONST_STR_LEN("<pre>"));
		li_string_encode_append(contents, *curbuf, LI_ENCODING_HTML);
		g_string_append_len(*curbuf, CONST_STR_LEN("</pre>"));
		g_free(contents);
	}
}

static void dirlist_format_size(gchar *buf, goffset size) {
	const gchar unit[] = "BKMGTPE"; /* Kilo, Mega, Tera, Peta, Exa */
	gchar *u = (gchar*)unit;
	guint remaining = 0;

	while (size > 1024) {
		remaining = size & 1023; /* % 1024 */
		size >>= 10; /* /= 1024 */
		u++;
	}

	remaining /= 100;
	if (remaining > 9)
		remaining = 9;
	if (size > 999) {
		size = 0;
		remaining = 9;
		u++;
	}

	if (size > 99) {
		*buf++ = size / 100 + '0';
		size = size % 100;
		*buf++ = size / 10 + '0';
		*buf++ = (size % 10) + '0';
	} else if (size > 9) {
		*buf++ = size / 10 + '0';
		*buf++ = (size % 10) + '0';
	} else {
		*buf++ = size + '0';
	}

	if (u != unit) {
		*buf++ = '.';
		*buf++ = remaining + '0';
	}
	*buf++ = *u;
	*buf = '\0';
}

static liHandlerResult dirlist(liVRequest *vr, gpointer param, gpointer *context) {
	GString *listing;
	liStatCacheEntry *sce;
	dirlist_data *dd;

	UNUSED(context);

	switch (vr->request.http_method) {
	case LI_HTTP_METHOD_GET:
	case LI_HTTP_METHOD_HEAD:
		break;
	default:
		return LI_HANDLER_GO_ON;
	}

	if (li_vrequest_is_handled(vr)) return LI_HANDLER_GO_ON;

	if (vr->physical.path->len == 0) return LI_HANDLER_GO_ON;

	switch (li_stat_cache_get_dirlist(vr, vr->physical.path, &sce)) {
	case LI_HANDLER_GO_ON: break;
	case LI_HANDLER_WAIT_FOR_EVENT: return LI_HANDLER_WAIT_FOR_EVENT;
	default: return LI_HANDLER_ERROR;
	}

	dd = param;

	if (sce->data.failed) {
		/* stat failed */
		int e = sce->data.err;

		li_stat_cache_entry_release(vr, sce);

		switch (e) {
		case ENOENT:
		case ENOTDIR:
			return LI_HANDLER_GO_ON;
		case EACCES:
			if (!li_vrequest_handle_direct(vr)) return LI_HANDLER_ERROR;
			vr->response.http_status = 403;
			return LI_HANDLER_GO_ON;
		default:
			VR_ERROR(vr, "stat('%s') failed: %s", sce->data.path->str, g_strerror(sce->data.err));
			return LI_HANDLER_ERROR;
		}
	} else if (!S_ISDIR(sce->data.st.st_mode)) {
		li_stat_cache_entry_release(vr, sce);
		return LI_HANDLER_GO_ON;
	} else if (vr->request.uri.path->len == 0 || vr->request.uri.path->str[vr->request.uri.path->len-1] != '/') {
		li_stat_cache_entry_release(vr, sce);
		li_vrequest_redirect_directory(vr);
		return LI_HANDLER_GO_ON;
	} else {
		/* everything ok, we have the directory listing */
		gboolean cachable;
		guint i, j;
		liStatCacheEntryData *sced;
		GString *mime_str, *encoded;
		GArray *directories, *files;
		gchar sizebuf[sizeof("999.9K")+1];
		gchar datebuf[sizeof("2005-Jan-01 22:23:24")+1];
		guint datebuflen;
		struct tm tm;
		gboolean hide;
		gboolean have_header = FALSE, have_readme = FALSE;

		if (!li_vrequest_handle_direct(vr)) {
			li_stat_cache_entry_release(vr, sce);
			return LI_HANDLER_ERROR;
		}
		vr->response.http_status = 200;

		if (dd->debug)
			VR_DEBUG(vr, "dirlist for \"%s\", %u entries", sce->data.path->str, sce->dirlist->len);

		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(dd->content_type));
		li_etag_set_header(vr, &sce->data.st, &cachable);
		if (cachable) {
			vr->response.http_status = 304;
			li_stat_cache_entry_release(vr, sce);
			return LI_HANDLER_GO_ON;
		}

		/* temporary string for encoded names */
		encoded = g_string_sized_new(64-1);

		/* seperate directories from other files */
		directories = g_array_sized_new(FALSE, FALSE, sizeof(guint), 16);
		files = g_array_sized_new(FALSE, FALSE, sizeof(guint), sce->dirlist->len);
		for (i = 0; i < sce->dirlist->len; i++) {
			sced = &g_array_index(sce->dirlist, liStatCacheEntryData, i);
			hide = FALSE;

			/* ingore entries where the stat() failed */
			if (sced->failed)
				continue;

			if (dd->hide_dotfiles && sced->path->str[0] == '.')
				continue;

			if (dd->hide_tildefiles && sced->path->str[sced->path->len-1] == '~')
				continue;

			for (j = 0; j < dd->exclude_suffix->len; j++) {
				if (li_string_suffix(sced->path, GSTR_LEN((GString*)g_ptr_array_index(dd->exclude_suffix, j)))) {
					hide = TRUE;
					break;
				}
			}

			if (hide)
				continue;

			for (j = 0; j < dd->exclude_prefix->len; j++) {
				if (li_string_prefix(sced->path, GSTR_LEN((GString*)g_ptr_array_index(dd->exclude_prefix, j)))) {
					hide = TRUE;
					break;
				}
			}

			if (hide)
				continue;

			if (S_ISDIR(sced->st.st_mode)) {
				if (dd->hide_directories) continue;
				g_array_append_val(directories, i);
			} else {
				if ((dd->include_header || dd->hide_header) && g_str_equal(sced->path, "HEADER.txt")) {
					if (dd->include_header && sced->st.st_size > 0 && sced->st.st_size < MAX_INCLUDE_FILE_SIZE) have_header = TRUE;
					if (dd->hide_header) continue;
				} else if ((dd->include_readme || dd->hide_readme) && g_str_equal(sced->path, "README.txt")) {
					if (dd->include_readme && sced->st.st_size > 0 && sced->st.st_size < MAX_INCLUDE_FILE_SIZE) have_readme = TRUE;
					if (dd->hide_readme) continue;
				}
				g_array_append_val(files, i);
			}
		}

		listing = g_string_sized_new(4*1024-1);
		g_string_append_printf(listing, html_header_start, vr->request.uri.path->str);

		if (dd->css) {
			/* custom css */
			g_string_append_len(listing, CONST_STR_LEN("		<link rel=\"stylesheet\" type=\"text/css\" href=\""));
			g_string_append_len(listing, GSTR_LEN(dd->css));
			g_string_append_len(listing, CONST_STR_LEN("\" />\n"));
		} else {
			/* default css */
			g_string_append_len(listing, CONST_STR_LEN(html_css));
		}
		g_string_append_len(listing, CONST_STR_LEN(html_header_end));

		try_append_file(vr, &listing, "HEADER.txt", dd->encode_header);

		g_string_append_printf(listing, html_table_start, vr->request.uri.path->str);

		g_string_append_printf(listing, html_table_row, "../",
			"Parent Directory", (gint64)0, "", (gint64)0, "-", "Directory");

		/* list directories */
		if (!dd->hide_directories) {
			for (i = 0; i < directories->len; i++) {
				sced = &g_array_index(sce->dirlist, liStatCacheEntryData, g_array_index(directories, guint, i));

				localtime_r(&(sced->st.st_mtime), &tm);
				datebuflen = strftime(datebuf, sizeof(datebuf), "%Y-%b-%d %H:%M:%S", &tm);
				datebuf[datebuflen] = '\0';

				g_string_append_len(listing, CONST_STR_LEN("				<tr><td><a href=\""));
				li_string_encode(sced->path->str, encoded, LI_ENCODING_URI);
				g_string_append_len(listing, GSTR_LEN(encoded));
				g_string_append_len(listing, CONST_STR_LEN("/\">"));
				li_string_encode(sced->path->str, encoded, LI_ENCODING_HTML);
				g_string_append_len(listing, GSTR_LEN(encoded));
				g_string_append_len(listing, CONST_STR_LEN("</a></td><td class=\"modified\" val=\""));
				li_string_append_int(listing, sced->st.st_mtime);
				g_string_append_len(listing, CONST_STR_LEN("\">"));
				g_string_append_len(listing, datebuf, datebuflen);
				g_string_append_len(listing, CONST_STR_LEN("</td>"
					"<td class=\"size\" val=\"0\">-</td>"
					"<td class=\"type\">Directory</td></tr>\n"));
			}
		}

		/*g_string_append_len(listing, CONST_STR_LEN("<tr><td colspan=\"4\">&nbsp;</td></tr>\n"));*/

		/* list files */
		for (i = 0; i < files->len; i++) {
			sced = &g_array_index(sce->dirlist, liStatCacheEntryData, g_array_index(files, guint, i));
			mime_str = li_mimetype_get(vr, sced->path);

			localtime_r(&(sced->st.st_mtime), &tm);
			datebuflen = strftime(datebuf, sizeof(datebuf), "%Y-%b-%d %H:%M:%S", &tm);
			datebuf[datebuflen] = '\0';

			dirlist_format_size(sizebuf, sced->st.st_size);

			g_string_append_len(listing, CONST_STR_LEN("				<tr><td><a href=\""));
			li_string_encode(sced->path->str, encoded, LI_ENCODING_URI);
			g_string_append_len(listing, GSTR_LEN(encoded));
			g_string_append_len(listing, CONST_STR_LEN("\">"));
			li_string_encode(sced->path->str, encoded, LI_ENCODING_HTML);
			g_string_append_len(listing, GSTR_LEN(encoded));
			g_string_append_len(listing, CONST_STR_LEN(
				"</a></td>"
				"<td class=\"modified\" val=\""));
			li_string_append_int(listing, sced->st.st_mtime);
			g_string_append_len(listing, CONST_STR_LEN("\">"));
			g_string_append_len(listing, datebuf, datebuflen);
			g_string_append_len(listing, CONST_STR_LEN("</td><td class=\"size\" val=\""));
			li_string_append_int(listing, sced->st.st_size);
			g_string_append_len(listing, CONST_STR_LEN("\">"));
			g_string_append(listing, sizebuf);
			g_string_append_len(listing, CONST_STR_LEN("</td><td class=\"type\">"));
			if (mime_str) {
				g_string_append_len(listing, GSTR_LEN(mime_str));
			} else {
				g_string_append_len(listing, CONST_STR_LEN("application/octet-stream"));
			}
			g_string_append_len(listing, CONST_STR_LEN("</td></tr>\n"));

			/*
			g_string_append_printf(listing, html_table_row,
				sced->path->str, sced->path->str,
				(gint64)sced->st.st_mtime, datebuf,
				sced->st.st_size, sizebuf,
				mime_str ? mime_str->str : "application/octet-stream");
			*/
		}

		g_string_append_len(listing, CONST_STR_LEN(html_table_end));

		try_append_file(vr, &listing, "README.txt", dd->encode_header);

		g_string_append_printf(listing, html_footer, CORE_OPTION(LI_CORE_OPTION_SERVER_TAG).string->str);

		li_chunkqueue_append_string(vr->out, listing);
		g_string_free(encoded, TRUE);
		g_array_free(directories, TRUE);
		g_array_free(files, TRUE);
	}

	li_stat_cache_entry_release(vr, sce);

	return LI_HANDLER_GO_ON;
}

static void dirlist_free(liServer *srv, gpointer param) {
	guint i;
	dirlist_data *data = param;

	UNUSED(srv);

	if (data->css)
		g_string_free(data->css, TRUE);

	for (i = 0; i < data->exclude_suffix->len; i++)
		g_string_free(g_ptr_array_index(data->exclude_suffix, i), TRUE);
	g_ptr_array_free(data->exclude_suffix, TRUE);

	for (i = 0; i < data->exclude_prefix->len; i++)
		g_string_free(g_ptr_array_index(data->exclude_prefix, i), TRUE);
	g_ptr_array_free(data->exclude_prefix, TRUE);

	g_string_free(data->content_type, TRUE);

	g_slice_free(dirlist_data, data);
}

static liAction* dirlist_create(liServer *srv, liPlugin* p, liValue *val) {
	dirlist_data *data;
	guint i;
	guint j;
	liValue *v, *tmpval;
	GString *k;

	if (val && val->type != LI_VALUE_LIST) {
		ERROR(srv, "%s", "dirlist expects an optional list of string-value pairs");
		return NULL;
	}

	data = g_slice_new0(dirlist_data);
	data->plugin = p;
	data->hide_dotfiles = TRUE;
	data->hide_tildefiles = TRUE;
	data->include_readme = TRUE;
	data->encode_header = TRUE;
	data->encode_readme = TRUE;
	data->exclude_suffix = g_ptr_array_new();
	data->exclude_prefix = g_ptr_array_new();
	data->content_type = g_string_new("text/html; charset=utf-8");

	if (val) {
		for (i = 0; i < val->data.list->len; i++) {
			tmpval = g_array_index(val->data.list, liValue*, i);
			if (tmpval->type != LI_VALUE_LIST || tmpval->data.list->len != 2 ||
				g_array_index(tmpval->data.list, liValue*, 0)->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "dirlist expects an optional list of string-value pairs");
				dirlist_free(srv, data);
				return NULL;
			}

			k = g_array_index(tmpval->data.list, liValue*, 0)->data.string;
			v = g_array_index(tmpval->data.list, liValue*, 1);

			if (g_str_equal(k->str, "sort")) { /* "name", "size" or "type" */
				if (v->type != LI_VALUE_STRING) {
					ERROR(srv, "%s", "dirlist: sort parameter must be a string");
					dirlist_free(srv, data);
					return NULL;
				}
				/* TODO */
				WARNING(srv, "%s", "dirlist: sort parameter not supported yet!");
			} else if (g_str_equal(k->str, "css")) {
				if (v->type != LI_VALUE_STRING) {
					ERROR(srv, "%s", "dirlist: css parameter must be a string");
					dirlist_free(srv, data);
					return NULL;
				}
				data->css = g_string_new_len(GSTR_LEN(v->data.string));
			} else if (g_str_equal(k->str, "hide-dotfiles")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: hide-dotfiles parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->hide_dotfiles = v->data.boolean;
			} else if (g_str_equal(k->str, "hide-tildefiles")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: hide-tildefiles parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->hide_tildefiles = v->data.boolean;
			} else if (g_str_equal(k->str, "hide-directories")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: hide-directories parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->hide_directories = v->data.boolean;
			} else if (g_str_equal(k->str, "include-header")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: include-header parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->include_header = v->data.boolean;
			} else if (g_str_equal(k->str, "hide-header")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: hide-header parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->hide_header = v->data.boolean;
			} else if (g_str_equal(k->str, "encode-header")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: encode-header parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->encode_header = v->data.boolean;
			} else if (g_str_equal(k->str, "include-readme")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: include-readme parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->include_readme = v->data.boolean;
			} else if (g_str_equal(k->str, "hide-readme")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: hide-readme parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->hide_readme = v->data.boolean;
			} else if (g_str_equal(k->str, "encode-readme")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: encode-readme parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->encode_readme = v->data.boolean;
			} else if (g_str_equal(k->str, "exclude-suffix")) {
				if (v->type != LI_VALUE_LIST) {
					ERROR(srv, "%s", "dirlist: exclude-suffix parameter must be a list of strings");
					dirlist_free(srv, data);
					return NULL;
				}
				for (j = 0; j < v->data.list->len; j++) {
					if (v->type != LI_VALUE_LIST) {
						ERROR(srv, "%s", "dirlist: exclude-suffix parameter must be a list of strings");
						dirlist_free(srv, data);
						return NULL;
					} else {
						g_ptr_array_add(data->exclude_suffix, g_string_new_len(GSTR_LEN(g_array_index(v->data.list, liValue*, j)->data.string)));
					}
				}
			} else if (g_str_equal(k->str, "exclude-prefix")) {
				if (v->type != LI_VALUE_LIST) {
					ERROR(srv, "%s", "dirlist: exclude-prefix parameter must be a list of strings");
					dirlist_free(srv, data);
					return NULL;
				}
				for (j = 0; j < v->data.list->len; j++) {
					if (v->type != LI_VALUE_LIST) {
						ERROR(srv, "%s", "dirlist: exclude-prefix parameter must be a list of strings");
						dirlist_free(srv, data);
						return NULL;
					} else {
						g_ptr_array_add(data->exclude_prefix, g_string_new_len(GSTR_LEN(g_array_index(v->data.list, liValue*, j)->data.string)));
					}
				}
			} else if (g_str_equal(k->str, "debug")) {
				if (v->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlist: debug parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->debug = v->data.boolean;
			} else if (g_str_equal(k->str, "content-type")) {
				if (v->type != LI_VALUE_STRING) {
					ERROR(srv, "%s", "dirlist: content-type parameter must be a string");
					dirlist_free(srv, data);
					return NULL;
				}
				g_string_assign(data->content_type, v->data.string->str);
			} else {
				ERROR(srv, "dirlist: unknown parameter \"%s\"", k->str);
				dirlist_free(srv, data);
				return NULL;
			}
		}
	}

	return li_action_new_function(dirlist, NULL, dirlist_free, data);
}

static const liPluginOption options[] = {
	{ "dirlist.debug", LI_VALUE_BOOLEAN, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "dirlist", dirlist_create },

	{ NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL }
};


static void plugin_dirlist_free(liServer *srv, liPlugin *p) {
	dirlist_plugin_data *pd;

	UNUSED(srv);

	pd = p->data;
	g_slice_free(dirlist_plugin_data, pd);
}


static void plugin_dirlist_init(liServer *srv, liPlugin *p) {
	dirlist_plugin_data *pd;

	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
	p->free = plugin_dirlist_free;

	pd = g_slice_new0(dirlist_plugin_data);
	p->data = pd;
}


gboolean mod_dirlist_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_dirlist", plugin_dirlist_init);

	return mod->config != NULL;
}

gboolean mod_dirlist_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
