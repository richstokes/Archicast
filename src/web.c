/* HTTP(S) fetch and HTML layout. A fixed width text grid suits the system font and
 * 4MB of guest RAM: a page becomes lines of styled runs, and !Web just draws the
 * ones in view. No images, CSS or scripts. Everything here runs on the fetch thread
 * except the web_* entry points, which only swap and read finished pages. */
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "net.h"
#include "web.h"

#define TEXT_SIZES {256 * 1024, 128 * 1024, 64 * 1024, 32 * 1024}
#define HEAP_SPARE (64 * 1024) /* left for the network while a page loads */
#define BYTES_PER_LINE 32
#define WINDOW (16 * 1024)     /* HTML awaiting parsing; a longer tag is dropped */
#define RAW WINDOW              /* header reads can overshoot into the body */
#define MAX_LINKS 2048
#define MAX_FIELDS 48
#define MAX_FORMS 16
#define MAX_URL 256
#define MAX_QUERY 512
#define VALUE_MAX 64
#define FIELD_WIDTH 24
#define MAX_REDIRECTS 5
#define HISTORY 8
#define TIMEOUT_MS 30000
#define RETRY_MS 10
#define ERR_AGAIN 35
#define ERR_INPROGRESS 36
#define ERR_ALREADY 37
#define ERR_NOBUFS 55
#define ERR_ISCONN 56
#define ERR_NOTCONN 57
#define NO_LINK 0xFFFF
#define FIELD_LINK 0x8000      /* a run's link: set means an index into fields */
#define RUN_HEADER 5 /* column, length, style, link high, link low; then the text */
#define MAX_RUN 200
#define TEXT_MARGIN 256        /* always room to say a page was cut short */
#define SEARCH_URL "https://lite.duckduckgo.com/lite/?q="

enum { FIELD_TEXT, FIELD_HIDDEN, FIELD_SUBMIT };

typedef struct {
	uint8_t form, kind, width;
	uint16_t name, value;      /* into link_pool, NO_LINK if absent */
	uint32_t text_at;          /* where a text box shows on the page */
} field_t;

typedef struct { uint16_t action; uint8_t post; } form_t;

typedef struct {
	uint8_t *text;
	uint32_t *line_at;
	int lines;
	uint16_t *link_at;
	char *link_pool;
	int links;
	uint32_t text_size, pool_size;
	int line_max;
	field_t fields[MAX_FIELDS];
	int field_count;
	form_t forms[MAX_FORMS];
	int form_count;
	char refresh[128];         /* a meta refresh, which is how some sites redirect */
	char title[64];
	char url[MAX_URL];
} page_t;

#define USER_AGENT "Archicast/1.0 (RISC OS 3.11; Dreamcast)"

static const char home_page[] =
	"<title>Archicast Browser</title><h1>Archicast Browser</h1>"
	"<p>A small web browser for RISC OS 3.11 on the Dreamcast. Type an address in the bar "
	"above and press Return.</p>"
	"<form action=\"https://lite.duckduckgo.com/lite/\"><input name=q> <input type=submit value=Search></form>"
	"<h2>Try these</h2><ul>"
	"<li><a href=\"http://info.cern.ch/\">The first website</a>"
	"<li><a href=\"https://text.npr.org/\">NPR text news</a>"
	"<li><a href=\"https://lite.cnn.com/\">CNN lite</a>"
	"<li><a href=\"https://news.ycombinator.com/\">Hacker News</a>"
	"<li><a href=\"https://en.wikipedia.org/wiki/RISC_OS\">RISC OS on Wikipedia</a>"
	"</ul><h2>Good to know</h2><p>Pages are text only: no pictures, styles or scripts, "
	"but text boxes and buttons work. Type words in the bar above to search with DuckDuckGo. "
	"Google needs JavaScript to search, so its search box goes there too. <b>Secure sites are encrypted but not verified</b>, so don't type "
	"anything private.</p><pre>Very long pages are cut short.</pre>";

static page_t *shown, *ready;
static volatile int state = WEB_IDLE;
static volatile int want;
static char want_url[MAX_URL];
static int want_keep_history;
static char want_body[MAX_QUERY];
static int active_field = -1;
static char message[48];
static int body_bytes;
static char history[HISTORY][MAX_URL];
static int history_used;
static volatile int started;

static void set_message(const char *text)
{
	snprintf(message, sizeof(message), "%s", text);
}

/* --- layout --- */

static int page_alloc(page_t *p, uint32_t size, int want_spare)
{
	void *spare = NULL;
	p->text_size = size;
	p->line_max = (int)(size / BYTES_PER_LINE);
	p->pool_size = size / 8 > 65000 ? 65000 : size / 8;
	p->text = malloc(size);
	p->line_at = malloc(((size_t)p->line_max + 1) * sizeof(uint32_t));
	p->link_at = malloc(MAX_LINKS * sizeof(uint16_t));
	p->link_pool = malloc(p->pool_size);
	if (want_spare)
		spare = malloc(HEAP_SPARE);
	free(spare);
	if (p->text && p->line_at && p->link_at && p->link_pool && (spare || !want_spare))
		return 1;
	free(p->text);
	free(p->line_at);
	free(p->link_at);
	free(p->link_pool);
	return 0;
}

/* As large as memory allows, which varies a lot with and without an SD card */
static page_t *page_new(void)
{
	static const uint32_t sizes[] = TEXT_SIZES;
	page_t *p = calloc(1, sizeof(*p));
	unsigned i;
	if (!p)
		return NULL;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		if (page_alloc(p, sizes[i], 1))
			return p;
	if (page_alloc(p, sizes[i - 1], 0))
		return p;
	free(p);
	return NULL;
}

static void page_free(page_t *p)
{
	if (!p)
		return;
	free(p->text);
	free(p->line_at);
	free(p->link_at);
	free(p->link_pool);
	free(p);
}

/* Trim to what the page used, so a short page doesn't hold it all */
static void page_trim(page_t *p, uint32_t used, uint32_t pool_used)
{
	uint8_t *text = realloc(p->text, used ? used : 1);
	uint32_t *lines = realloc(p->line_at, ((size_t)p->lines + 1) * sizeof(uint32_t));
	uint16_t *links = realloc(p->link_at, ((size_t)p->links + 1) * sizeof(uint16_t));
	char *pool = realloc(p->link_pool, pool_used ? pool_used : 1);
	if (text)
		p->text = text;
	if (lines)
		p->line_at = lines;
	if (links)
		p->link_at = links;
	if (pool)
		p->link_pool = pool;
}

/* Accumulates text until a space or a tag */
typedef struct { uint8_t data[WEB_COLUMNS]; int len; } word_t;

typedef struct {
	page_t *p;
	uint32_t used, pool_used;
	int column, indent, style, link;
	int run_at;     /* offset of the open run's header, or -1 */
	int blank;      /* the last line was empty, or nothing has been written */
	int space;      /* a space is due before the next word */
	int full, finishing;
	word_t word;
	int pre, plain, list_depth, in_title, title_len, heading, emphasis, strong;
	int in_cell;
	int form;       /* current form, or -1 */
	int hidden;     /* depth of div inside a display:none */
	const char *skip; /* closing text being looked for, across reads */
} layout_t;

static void line_end(layout_t *l)
{
	if (!l->finishing && (l->p->lines + 4 >= l->p->line_max ||
	    l->used + TEXT_MARGIN + WEB_COLUMNS + RUN_HEADER * 8 >= l->p->text_size)) {
		l->full = 1;
		return;
	}
	l->blank = l->column == 0;
	l->p->lines++;
	l->p->line_at[l->p->lines] = l->used;
	l->column = 0;
	l->run_at = -1;
	l->space = 0;
}

static void break_line(layout_t *l)
{
	if (l->column)
		line_end(l);
	l->space = 0;
}

static void paragraph(layout_t *l)
{
	break_line(l);
	if (!l->blank)
		line_end(l);
}

static void put_char(layout_t *l, uint8_t c)
{
	uint8_t *t = l->p->text;
	if (l->full || l->hidden)
		return;
	if (l->column == 0 && l->indent) {
		l->column = l->indent;
		l->run_at = -1;
	}
	if (l->run_at >= 0 && (t[l->run_at + 2] != l->style ||
	    ((t[l->run_at + 3] << 8) | t[l->run_at + 4]) != (l->link < 0 ? NO_LINK : l->link) ||
	    t[l->run_at + 1] >= MAX_RUN))
		l->run_at = -1;
	if (l->run_at < 0) {
		int link = l->link < 0 ? NO_LINK : l->link;
		l->run_at = (int)l->used;
		t[l->used++] = (uint8_t)l->column;
		t[l->used++] = 0;
		t[l->used++] = (uint8_t)l->style;
		t[l->used++] = (uint8_t)(link >> 8);
		t[l->used++] = (uint8_t)link;
	}
	t[l->used++] = c;
	t[l->run_at + 1]++;
	l->column++;
	l->blank = 0;
}

static void put_word(layout_t *l, const uint8_t *word, int len)
{
	int room = WEB_COLUMNS - l->indent;
	if (l->column && l->column + (l->space ? 1 : 0) + len > WEB_COLUMNS)
		line_end(l);
	if (l->space && l->column > l->indent)
		put_char(l, ' ');
	l->space = 0;
	while (len > 0) {
		int n = len > room ? room : len, i;
		for (i = 0; i < n; i++)
			put_char(l, word[i]);
		word += n;
		len -= n;
		if (len)
			line_end(l);
	}
}

static void word_flush(layout_t *l)
{
	if (l->word.len)
		put_word(l, l->word.data, l->word.len);
	l->word.len = 0;
}

static void word_add(layout_t *l, uint8_t c)
{
	if (l->word.len == (int)sizeof(l->word.data))
		word_flush(l);
	l->word.data[l->word.len++] = c;
}

static int entity(const char *s, const char *end, unsigned *out)
{
	static const struct { const char *name; unsigned code; } names[] = {
		{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 160},
		{"copy", 169}, {"reg", 174}, {"pound", 163}, {"mdash", 0x2014}, {"ndash", 0x2013},
		{"hellip", 0x2026}, {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"ldquo", 0x201C},
		{"rdquo", 0x201D}, {"middot", 183}, {"bull", 0x2022}, {"eacute", 233}, {"egrave", 232},
	};
	const char *p = s + 1;
	unsigned i, code = 0;
	if (p < end && *p == '#') {
		int hex = ++p < end && (*p == 'x' || *p == 'X');
		if (hex)
			p++;
		for (; p < end && *p != ';' && p - s < 10; p++) {
			int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
			if (d < 0 || (!hex && d > 9))
				return 0;
			code = code * (hex ? 16 : 10) + (unsigned)d;
		}
		if (p >= end || *p != ';')
			return 0;
		*out = code;
		return (int)(p + 1 - s);
	}
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		size_t n = strlen(names[i].name);
		if ((size_t)(end - p) > n && !strncmp(p, names[i].name, n) && p[n] == ';') {
			*out = names[i].code;
			return (int)n + 2;
		}
	}
	return 0;
}

/* RISC OS is Latin-1. Typography folds to ASCII, anything else becomes a ? */
static void put_code(layout_t *l, unsigned code)
{
	switch (code) {
	case 0x2018: case 0x2019: code = '\''; break;
	case 0x201C: case 0x201D: code = '"'; break;
	case 0x2013: case 0x2014: code = '-'; break;
	case 0x2022: code = '*'; break;
	case 0x2026: word_add(l, '.'); word_add(l, '.'); code = '.'; break;
	default: break;
	}
	if (code == 160)
		code = ' ';
	word_add(l, code < 256 ? (uint8_t)code : '?');
}

/* One UTF-8 sequence, or a lone byte taken as Latin-1. Returns bytes used. */
static int decode_utf8(const uint8_t *s, const uint8_t *end, unsigned *out)
{
	int extra = s[0] >= 0xF0 ? 3 : s[0] >= 0xE0 ? 2 : s[0] >= 0xC0 ? 1 : 0, i;
	unsigned code = s[0] & (0x3F >> extra);
	if (!extra || s + extra >= end) {
		*out = s[0];
		return 1;
	}
	for (i = 1; i <= extra; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			*out = s[0];
			return 1;
		}
		code = (code << 6) | (s[i] & 0x3F);
	}
	*out = code;
	return extra + 1;
}

/* A string kept with the page, or NO_LINK if it won't fit */
static int pool_add(layout_t *l, const char *text, int len, int room)
{
	page_t *p = l->p;
	uint32_t at = l->pool_used;
	if (len >= 0xFFFF || at + (uint32_t)(room > len ? room : len) + 1 > p->pool_size)
		return NO_LINK;
	memcpy(p->link_pool + at, text, (size_t)len);
	p->link_pool[at + (uint32_t)len] = 0;
	l->pool_used = at + (uint32_t)(room > len ? room : len) + 1;
	return (int)at;
}

static int add_link(layout_t *l, const char *url, int len)
{
	page_t *p = l->p;
	int at;
	if (p->links >= MAX_LINKS)
		return -1;
	at = pool_add(l, url, len, 0);
	if (at == NO_LINK)
		return -1;
	p->link_at[p->links] = (uint16_t)at;
	return p->links++;
}

/* The value of attr in a tag's text, unquoted. Returns its length, or -1. */
static int attribute(const char *tag, const char *end, const char *attr, const char **value)
{
	size_t n = strlen(attr);
	const char *p;
	for (p = tag; p + n < end; p++) {
		char q;
		const char *v;
		if (strncasecmp(p, attr, n) || (p[-1] != ' ' && p[-1] != '\n' && p[-1] != '\t'))
			continue;
		v = p + n;
		while (v < end && *v == ' ')
			v++;
		if (v >= end || *v != '=')
			continue;
		for (v++; v < end && *v == ' '; v++)
			;
		q = v < end && (*v == '"' || *v == '\'') ? *v++ : 0;
		*value = v;
		while (v < end && (q ? *v != q : *v != ' ' && *v != '>'))
			v++;
		return (int)(v - *value);
	}
	return -1;
}

/* In place: attributes hold entities too, and &amp; in links is everywhere */
static int decode_attribute(char *text, int len)
{
	int in = 0, out = 0;
	while (in < len) {
		unsigned code;
		int used = text[in] == '&' ? entity(text + in, text + len, &code) : 0;
		if (used && code < 256) {
			text[out++] = (char)code;
			in += used;
		} else {
			text[out++] = text[in++];
		}
	}
	text[out] = 0;
	return out;
}

static int tag_is(const char *name, int len, const char *want_name)
{
	return (int)strlen(want_name) == len && !strncasecmp(name, want_name, (size_t)len);
}

static int tag_in(const char *name, int len, const char *const *list)
{
	for (; *list; list++)
		if (tag_is(name, len, *list))
			return 1;
	return 0;
}

static void update_style(layout_t *l)
{
	l->style = l->link >= 0 ? ((l->link & FIELD_LINK) ? STYLE_FIELD : STYLE_LINK) :
		l->heading || l->strong ? STYLE_BOLD : l->pre ? STYLE_CODE :
		l->emphasis ? STYLE_EM : STYLE_TEXT;
}

static int attr_copy(const char *tag, const char *end, const char *name, char *out, int size)
{
	const char *value;
	int n = attribute(tag, end, name, &value);
	if (n < 0)
		return -1;
	if (n >= size)
		n = size - 1;
	memcpy(out, value, (size_t)n);
	out[n] = 0;
	return decode_attribute(out, n);
}

static int add_field(layout_t *l, int kind, const char *tag, const char *end)
{
	page_t *p = l->p;
	field_t *f;
	char text[VALUE_MAX];
	int n;
	if (l->form < 0 || p->field_count >= MAX_FIELDS)
		return -1;
	f = &p->fields[p->field_count];
	f->form = (uint8_t)l->form;
	f->kind = (uint8_t)kind;
	n = attr_copy(tag, end, "name", text, sizeof(text));
	f->name = n > 0 ? (uint16_t)pool_add(l, text, n, 0) : NO_LINK;
	n = attr_copy(tag, end, "value", text, sizeof(text));
	f->value = (uint16_t)pool_add(l, text, n > 0 ? n : 0, kind == FIELD_TEXT ? VALUE_MAX : 0);
	return p->field_count++;
}

static void put_text_box(layout_t *l, int index)
{
	page_t *p = l->p;
	field_t *f = &p->fields[index];
	const char *value = f->value == NO_LINK ? "" : p->link_pool + f->value;
	size_t n = strlen(value);
	int i;
	word_flush(l);
	if (l->column + FIELD_WIDTH + 1 > WEB_COLUMNS)
		line_end(l);
	if (l->space && l->column > l->indent)
		put_char(l, ' ');
	l->space = 0;
	l->link = FIELD_LINK | index;
	update_style(l);
	f->width = FIELD_WIDTH;
	for (i = 0; i < FIELD_WIDTH; i++)
		put_char(l, i < (int)n ? (uint8_t)value[i] : ' ');
	f->text_at = l->used - FIELD_WIDTH;
	l->link = -1;
	update_style(l);
	l->space = 1;
}

static void put_button(layout_t *l, int index, const char *label, int n)
{
	word_flush(l);
	l->link = FIELD_LINK | index;
	update_style(l);
	l->space = l->space && l->column > l->indent;
	put_word(l, (const uint8_t *)label, n > WEB_COLUMNS - 2 ? WEB_COLUMNS - 2 : n);
	l->link = -1;
	update_style(l);
	l->space = 1;
}

static void input_tag(layout_t *l, const char *tag, const char *end)
{
	char type[16], label[VALUE_MAX];
	int n, index;
	if (attr_copy(tag, end, "type", type, sizeof(type)) < 0)
		strcpy(type, "text");
	if (!strcasecmp(type, "hidden")) {
		add_field(l, FIELD_HIDDEN, tag, end);
	} else if (!strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "image")) {
		index = add_field(l, FIELD_SUBMIT, tag, end);
		if (index >= 0) {
			n = attr_copy(tag, end, "value", label, sizeof(label));
			if (n <= 0)
				n = snprintf(label, sizeof(label), "Submit");
			put_button(l, index, label, n);
		}
	} else if (!strcasecmp(type, "text") || !strcasecmp(type, "search") ||
	           !strcasecmp(type, "email") || !strcasecmp(type, "url") || !strcasecmp(type, "tel")) {
		index = add_field(l, FIELD_TEXT, tag, end);
		if (index >= 0)
			put_text_box(l, index);
	}
}

static void form_tag(layout_t *l, const char *tag, const char *end)
{
	page_t *p = l->p;
	char text[192], method[8];
	int n;
	l->form = -1;
	if (p->form_count >= MAX_FORMS)
		return;
	n = attr_copy(tag, end, "action", text, sizeof(text));
	p->forms[p->form_count].action = (uint16_t)pool_add(l, n > 0 ? text : "", n > 0 ? n : 0, 0);
	p->forms[p->form_count].post = attr_copy(tag, end, "method", method, sizeof(method)) > 0 &&
		!strcasecmp(method, "post");
	l->form = p->form_count++;
}

static void meta_tag(layout_t *l, const char *tag, const char *end)
{
	char equiv[16], content[192], *url;
	if (attr_copy(tag, end, "http-equiv", equiv, sizeof(equiv)) <= 0 ||
	    strcasecmp(equiv, "refresh") ||
	    attr_copy(tag, end, "content", content, sizeof(content)) <= 0)
		return;
	url = strstr(content, "url=");
	if (!url)
		url = strstr(content, "URL=");
	if (url)
		snprintf(l->p->refresh, sizeof(l->p->refresh), "%s", url + 4 + (url[4] == '\'' ? 1 : 0));
}

static int style_hides(const char *tag, const char *end)
{
	char style[64], *p, *out;
	if (attr_copy(tag, end, "style", style, sizeof(style)) <= 0)
		return 0;
	for (p = out = style; *p; p++)
		if (*p != ' ')
			*out++ = *p;
	*out = 0;
	return strstr(style, "display:none") != NULL;
}

/* One complete tag: name is just past the <, close at the > */
static void handle_tag(layout_t *l, const char *name, const char *close, int ending)
{
	static const char *const paragraphs[] = {"p", "section", "article", "header", "footer",
		"main", "nav", "aside", "table", "ul", "ol", "dl", "blockquote", "figure", NULL};
	static const char *const breaks[] = {"br", "tr", "dt", "dd", NULL};
	int len;
	for (len = 0; name + len < close && name[len] != ' ' && name[len] != '/' &&
	     name[len] != '\n' && name[len] != '\t' && name[len] != '\r'; len++)
		;
	word_flush(l);
	if (tag_is(name, len, "div")) {
		if (l->hidden) {
			l->hidden += ending ? -1 : 1;
			return;
		}
		if (!ending && close[-1] != '/' && style_hides(name, close)) {
			l->hidden = 1;
			return;
		}
	}
	if (tag_is(name, len, "title")) {
		l->in_title = !ending;
	} else if (tag_in(name, len, paragraphs)) {
		paragraph(l);
		if (tag_is(name, len, "ul") || tag_is(name, len, "ol") || tag_is(name, len, "blockquote")) {
			l->list_depth += ending ? -1 : 1;
			if (l->list_depth < 0)
				l->list_depth = 0;
			l->indent = (l->list_depth > 6 ? 6 : l->list_depth) * 2;
		}
	} else if (tag_in(name, len, breaks)) {
		break_line(l);
	} else if (tag_is(name, len, "div")) {
		if (!l->in_cell) /* layout tables put a div around almost everything */
			break_line(l);
	} else if (tag_is(name, len, "td") || tag_is(name, len, "th")) {
		l->in_cell = !ending;
		l->space = 1;
	} else if (tag_is(name, len, "li")) {
		break_line(l);
		if (!ending) {
			int saved = l->indent;
			l->indent = saved > 2 ? saved - 2 : 0;
			put_word(l, (const uint8_t *)"* ", 2);
			l->indent = saved;
		}
	} else if (len == 2 && (name[0] | 32) == 'h' && name[1] >= '1' && name[1] <= '6') {
		paragraph(l);
		l->heading = !ending;
	} else if (tag_is(name, len, "hr")) {
		static const uint8_t rule[] = "----------------------------------------";
		paragraph(l);
		put_word(l, rule, sizeof(rule) - 1);
		paragraph(l);
	} else if (tag_is(name, len, "pre")) {
		paragraph(l);
		l->pre = !ending;
	} else if (tag_is(name, len, "b") || tag_is(name, len, "strong")) {
		l->strong = !ending;
	} else if (tag_is(name, len, "i") || tag_is(name, len, "em") || tag_is(name, len, "cite")) {
		l->emphasis = !ending;
	} else if (tag_is(name, len, "a")) {
		if (ending) {
			l->link = -1;
		} else {
			char href[MAX_URL];
			int n = attr_copy(name, close, "href", href, sizeof(href));
			l->link = n > 0 ? add_link(l, href, n) : -1;
		}
	} else if (tag_is(name, len, "form")) {
		if (ending)
			l->form = -1;
		else
			form_tag(l, name, close);
		paragraph(l);
	} else if (tag_is(name, len, "input") && !ending) {
		input_tag(l, name, close);
	} else if (tag_is(name, len, "textarea") && !ending) {
		int index = add_field(l, FIELD_TEXT, name, close);
		if (index >= 0)
			put_text_box(l, index);
		l->skip = "</textarea";
	} else if (tag_is(name, len, "button")) {
		if (ending) {
			if (l->link & FIELD_LINK)
				l->link = -1;
			l->space = 1;
		} else {
			int index = add_field(l, FIELD_SUBMIT, name, close);
			l->link = index >= 0 ? FIELD_LINK | index : -1;
		}
	} else if (tag_is(name, len, "meta") && !ending) {
		meta_tag(l, name, close);
	} else if (tag_is(name, len, "img") && !ending) {
		const char *alt;
		int n = attribute(name, close, "alt", &alt);
		if (n > 0 && n < WEB_COLUMNS - 2) {
			uint8_t text[WEB_COLUMNS];
			text[0] = '[';
			memcpy(text + 1, alt, (size_t)n);
			text[n + 1] = ']';
			put_word(l, text, n + 2);
			l->space = 1;
		}
	}
	update_style(l);
}

/* Skips towards l->skip. Returns where to carry on, or NULL if it needs more. */
static const char *skip_towards(layout_t *l, const char *s, const char *end, int final)
{
	size_t n = strlen(l->skip);
	for (; s + n <= end; s++) {
		if (strncasecmp(s, l->skip, n))
			continue;
		s += n;
		if (l->skip[n - 1] != '>') {
			while (s < end && *s != '>')
				s++;
			if (s == end && !final)
				return NULL;
			if (s < end)
				s++;
		}
		l->skip = NULL;
		return s;
	}
	return final ? end : NULL;
}

/* Lays out what it can of [s, end) and returns how many bytes it used. The rest
 * is an unfinished tag, entity or character, offered again with more after it. */
static int layout_feed(layout_t *l, const char *start, const char *end, int final)
{
	static const char *const skipped[] = {"script", "style", "svg", "select", "iframe", NULL};
	static const char *const skipped_end[] = {"</script", "</style", "</svg", "</select", "</iframe"};
	const char *s = start;
	page_t *p = l->p;
	while (s < end && !l->full) {
		if (l->skip) {
			size_t keep = strlen(l->skip);
			const char *next = skip_towards(l, s, end, final);
			if (!next) {
				/* Nothing here, bar the start of the closing text */
				if ((size_t)(end - s) > keep)
					s = end - keep;
				break;
			}
			s = next;
			continue;
		}
		if (*s == '<' && !l->plain) {
			const char *name, *close;
			int ending, i;
			if (end - s < 4 && !final)
				break;
			if (!strncmp(s, "<!--", 4)) {
				l->skip = "-->";
				s += 4;
				continue;
			}
			close = memchr(s, '>', (size_t)(end - s));
			if (!close) {
				if (!final && end - s < WINDOW - RAW)
					break;
				l->skip = ">"; /* too long to hold, which is a tag nobody will miss */
				continue;
			}
			name = s + 1;
			ending = *name == '/';
			name += ending;
			s = close + 1;
			if (!ending) {
				int len = (int)strcspn(name, " />\n\t\r");
				for (i = 0; skipped[i]; i++)
					if (tag_is(name, len, skipped[i])) {
						word_flush(l);
						l->skip = skipped_end[i];
						break;
					}
				if (l->skip)
					continue;
			}
			handle_tag(l, name, close, ending);
			continue;
		}
		if (l->in_title) {
			unsigned code = (unsigned char)*s;
			int used;
			if (*s == '&' && end - s < 10 && !final)
				break;
			used = *s == '&' ? entity(s, end, &code) : 0;
			s += used ? used : 1;
			if (l->title_len + 1 < (int)sizeof(p->title) && code >= ' ' && code < 256)
				p->title[l->title_len++] = (char)code;
			continue;
		}
		if (*s == '\n' || *s == '\r' || *s == ' ' || *s == '\t') {
			if (l->pre || l->plain) {
				if (*s == '\n') {
					word_flush(l);
					line_end(l);
				} else if (*s != '\r') {
					word_flush(l);
					put_char(l, ' ');
				}
			} else {
				word_flush(l);
				l->space = 1;
			}
			s++;
			continue;
		}
		if (*s == '&' && !l->plain) {
			unsigned code;
			int used;
			if (end - s < 10 && !final)
				break;
			used = entity(s, end, &code);
			if (used) {
				put_code(l, code);
				s += used;
				continue;
			}
		}
		{
			unsigned code;
			if ((*s & 0x80) && end - s < 4 && !final)
				break;
			s += decode_utf8((const uint8_t *)s, (const uint8_t *)end, &code);
			put_code(l, code);
		}
	}
	return (int)(s - start);
}

static void layout_start(layout_t *l, page_t *p, int plain)
{
	memset(l, 0, sizeof(*l));
	p->line_at[0] = 0;
	p->lines = 0;
	l->p = p;
	l->link = -1;
	l->run_at = -1;
	l->blank = 1;
	l->form = -1;
	l->plain = plain;
	l->pre = plain;
	update_style(l);
}

static void layout_finish(layout_t *l)
{
	int cut = l->full;
	l->full = 0;
	l->finishing = 1;
	l->hidden = 0;
	word_flush(l);
	if (cut) {
		static const uint8_t note[] = "[Page cut short here]";
		paragraph(l);
		l->style = STYLE_EM;
		l->link = -1;
		put_word(l, note, sizeof(note) - 1);
	}
	break_line(l);
	page_trim(l->p, l->used, l->pool_used);
}

/* --- addresses --- */

typedef struct { int tls, port; char host[96]; const char *path; } url_t;

static int url_parse(const char *text, url_t *u)
{
	const char *host = strstr(text, "://"), *end;
	size_t n;
	u->tls = !strncasecmp(text, "https:", 6);
	if (!host || (!u->tls && strncasecmp(text, "http:", 5)))
		return 0;
	host += 3;
	u->port = u->tls ? 443 : 80;
	u->path = strchr(host, '/');
	end = u->path ? u->path : host + strlen(host);
	if (!u->path)
		u->path = "/";
	n = (size_t)(end - host);
	if (n == 0 || n >= sizeof(u->host))
		return 0;
	memcpy(u->host, host, n);
	u->host[n] = 0;
	{
		char *colon = strchr(u->host, ':');
		if (colon) {
			*colon = 0;
			u->port = atoi(colon + 1);
		}
	}
	return 1;
}

/* The address a link or redirect points at, from the page it was found on */
static void url_join(char *out, const char *base, const char *link)
{
	const char *host, *path;
	size_t origin;
	char *cut;
	while (*link == ' ' || *link == '\n')
		link++;
	if (strstr(link, "://") && strcspn(link, ":") < strcspn(link, "/?#")) {
		snprintf(out, MAX_URL, "%s", link);
	} else if (!strncmp(link, "about:", 6)) {
		snprintf(out, MAX_URL, "%s", link);
	} else {
		host = strstr(base, "://");
		host = host ? host + 3 : base;
		path = strchr(host, '/');
		origin = path ? (size_t)(path - base) : strlen(base);
		if (!strncmp(link, "//", 2)) {
			size_t scheme = (size_t)(host - base) - 2;
			snprintf(out, MAX_URL, "%.*s%s", (int)scheme, base, link);
		} else if (*link == '/') {
			snprintf(out, MAX_URL, "%.*s%s", (int)origin, base, link);
		} else if (*link == '#' || !*link) {
			snprintf(out, MAX_URL, "%s", base);
		} else {
			char dir[MAX_URL], *slash;
			snprintf(dir, sizeof(dir), "%s", path ? path : "/");
			dir[strcspn(dir, "?#")] = 0;
			slash = strrchr(dir, '/');
			if (slash)
				slash[1] = 0;
			while (!strncmp(link, "../", 3)) {
				link += 3;
				if (slash && slash > dir) {
					*slash = 0;
					slash = strrchr(dir, '/');
					if (slash)
						slash[1] = 0;
				}
			}
			while (!strncmp(link, "./", 2))
				link += 2;
			snprintf(out, MAX_URL, "%.*s%s%s", (int)origin, base, dir, link);
		}
	}
	cut = strchr(out, '#');
	if (cut)
		*cut = 0;
}

/* --- fetch --- */

typedef struct { int fd; web_tls_t *tls; uint64_t deadline; } conn_t;

static int conn_expired(const conn_t *c)
{
	return plat_wall_ms() > c->deadline || want;
}

static int conn_open(conn_t *c, const url_t *u)
{
	struct addrinfo hints, *res = NULL;
	uint8_t ip[4];
	int rv;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	c->deadline = plat_wall_ms() + TIMEOUT_MS;
	c->tls = NULL;
	if (getaddrinfo(u->host, NULL, &hints, &res) || !res) {
		set_message("Could not find that site");
		return 0;
	}
	memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
	freeaddrinfo(res);
	c->fd = net_sock_open(SOCK_KIND_TCP);
	if (c->fd < 0) {
		set_message("No network");
		return 0;
	}
	/* Asking again is the portable way to learn how a non-blocking connect went */
	for (;;) {
		rv = net_sock_connect(c->fd, ip, u->port);
		if (rv == 0 || rv == -ERR_ISCONN)
			break;
		if (rv != -ERR_INPROGRESS && rv != -ERR_ALREADY && rv != -ERR_AGAIN) {
			set_message(rv == -ERR_NOBUFS ? "Not enough memory to connect" : "Could not connect");
			net_sock_close(c->fd);
			return 0;
		}
		if (conn_expired(c)) {
			set_message("The site did not answer");
			net_sock_close(c->fd);
			return 0;
		}
		plat_sleep_ms(RETRY_MS);
	}
	if (u->tls) {
		set_message("Setting up a secure link");
		c->tls = plat_tls_open(c->fd, u->host);
		if (!c->tls) {
			set_message("Secure link failed");
			net_sock_close(c->fd);
			return 0;
		}
	}
	return 1;
}

static int conn_write(conn_t *c, const char *data, int len)
{
	while (len > 0) {
		int n = c->tls ? plat_tls_write(c->tls, data, len) : net_sock_send(c->fd, data, len);
		if (n > 0) {
			data += n;
			len -= n;
		} else if ((n == -ERR_AGAIN || n == -ERR_NOTCONN) && !conn_expired(c)) {
			plat_sleep_ms(RETRY_MS);
		} else {
			return 0;
		}
	}
	return 1;
}

/* Bytes read, 0 at the end, -1 on failure */
static int conn_read(conn_t *c, char *data, int len)
{
	for (;;) {
		int n = c->tls ? plat_tls_read(c->tls, data, len) : net_sock_recv(c->fd, data, len);
		if (n >= 0)
			return n;
		if (n != -ERR_AGAIN || conn_expired(c))
			return -1;
		plat_sleep_ms(RETRY_MS);
	}
}

static void conn_close(conn_t *c)
{
	if (c->tls)
		plat_tls_close(c->tls);
	net_sock_close(c->fd);
}

static const char *header_value(const char *headers, const char *end, const char *name)
{
	size_t n = strlen(name);
	const char *line = headers;
	while (line && line < end) {
		if (!strncasecmp(line, name, n) && line[n] == ':') {
			line += n + 1;
			while (*line == ' ')
				line++;
			return line;
		}
		line = strstr(line, "\r\n");
		line = line ? line + 2 : NULL;
	}
	return NULL;
}

/* Chunked transfer undone as bytes arrive */
typedef struct { int chunked, state, done; unsigned long left; long length; } body_t;
enum { CHUNK_SIZE, CHUNK_DATA, CHUNK_END };

/* Moves up to room bytes from raw to out. Sets *used for raw. */
static int body_decode(body_t *b, const char *raw, int n, char *out, int room, int *used)
{
	int in = 0, put = 0;
	if (!b->chunked) {
		put = n < room ? n : room;
		if (b->length >= 0 && put > b->length)
			put = (int)b->length;
		memcpy(out, raw, (size_t)put);
		if (b->length >= 0) {
			b->length -= put;
			b->done = b->length == 0;
		}
		*used = put;
		return put;
	}
	while (in < n && put < room && !b->done) {
		if (b->state == CHUNK_DATA) {
			int take = n - in < room - put ? n - in : room - put;
			if ((unsigned long)take > b->left)
				take = (int)b->left;
			memcpy(out + put, raw + in, (size_t)take);
			in += take;
			put += take;
			b->left -= (unsigned long)take;
			if (!b->left)
				b->state = CHUNK_END;
			continue;
		}
		if (b->state == CHUNK_END) {
			if (raw[in++] == '\n')
				b->state = CHUNK_SIZE;
			continue;
		}
		{
			char c = raw[in++];
			int d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
			if (c == '\n') {
				if (b->left)
					b->state = CHUNK_DATA;
				else
					b->done = 1;
			} else if (d >= 0 && !b->length) {
				b->left = b->left * 16 + (unsigned long)d;
			} else if (c != '\r') {
				b->length = 1; /* a chunk extension: ignore up to the newline */
			}
			if (c == '\n')
				b->length = 0;
		}
	}
	*used = in;
	return put;
}

/* Pours the rest of the reply through the layout, starting with what's in raw */
static void read_body(conn_t *c, body_t *b, layout_t *l, char *window, char *raw, int have)
{
	int waiting = 0, done = 0, n;
	while (!l->full && !want) {
		while (have > 0 && !l->full) {
			int used, got = body_decode(b, raw, have, window + waiting, WINDOW - waiting, &used);
			int fed;
			have -= used;
			memmove(raw, raw + used, (size_t)have);
			waiting += got;
			fed = layout_feed(l, window, window + waiting, 0);
			waiting -= fed;
			memmove(window, window + fed, (size_t)waiting);
			if (!used && !got)
				break;
		}
		if (done || b->done)
			break;
		n = conn_read(c, raw + have, RAW - have);
		if (n <= 0)
			done = 1;
		else {
			have += n;
			body_bytes += n;
		}
		c->deadline = plat_wall_ms() + TIMEOUT_MS;
		if (body_bytes)
			set_message("Receiving");
	}
	layout_feed(l, window, window + waiting, 1);
}

/* What the headers say to do: 1 read a page, 2 redirected, 0 failed */
static int read_headers(char *url, const char *head, const char *end, body_t *b, int *plain)
{
	const char *value, *type;
	int status;
	if (strncmp(head, "HTTP/1.", 7)) {
		set_message("That isn't a web page");
		return 0;
	}
	status = atoi(head + 9);
	value = header_value(head, end, "Location");
	if (status >= 300 && status < 400 && value) {
		char target[MAX_URL], base[MAX_URL];
		size_t len = strcspn(value, "\r\n");
		if (len >= sizeof(target))
			len = sizeof(target) - 1;
		memcpy(target, value, len);
		target[len] = 0;
		snprintf(base, sizeof(base), "%s", url);
		url_join(url, base, target);
		return 2;
	}
	type = header_value(head, end, "Content-Type");
	if (type && strncasecmp(type, "text/", 5) && strncasecmp(type, "application/xhtml", 17)) {
		set_message("That's not a text page");
		return 0;
	}
	*plain = type && !strncasecmp(type, "text/plain", 10);
	memset(b, 0, sizeof(*b));
	value = header_value(head, end, "Transfer-Encoding");
	b->chunked = value && !strncasecmp(value, "chunked", 7);
	value = header_value(head, end, "Content-Length");
	b->length = !b->chunked && value ? atol(value) : b->chunked ? 0 : -1;
	return 1;
}

/* 1 with a page in *out, 0 on failure, or 2 with *url updated for a redirect */
static int fetch_once(char *url, const char *post, page_t **out)
{
	static layout_t l;
	url_t u;
	conn_t c;
	body_t b;
	char *request = NULL, *window = NULL, *raw = NULL, *headers_end = NULL;
	int got = 0, n, plain = 0, result = 0;
	if (!url_parse(url, &u)) {
		set_message("That address isn't http or https");
		return 0;
	}
	set_message("Connecting");
	if (!conn_open(&c, &u))
		return 0;
	window = malloc(WINDOW + RAW + MAX_URL + MAX_QUERY + 300);
	if (!window) {
		set_message("Not enough memory");
		conn_close(&c);
		return 0;
	}
	raw = window + WINDOW;
	request = raw + RAW;
	if (post)
		n = snprintf(request, MAX_URL + MAX_QUERY + 300, "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
			"Accept: text/html, text/plain\r\nAccept-Encoding: identity\r\nConnection: close\r\n"
			"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n%s",
			u.path, u.host, USER_AGENT, (int)strlen(post), post);
	else
		n = snprintf(request, MAX_URL + MAX_QUERY + 300, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
			"Accept: text/html, text/plain\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
			u.path, u.host, USER_AGENT);
	set_message("Waiting for the site");
	if (!conn_write(&c, request, n)) {
		set_message("Could not send the request");
		goto done;
	}
	/* Headers land in the window, as a long Set-Cookie can outgrow raw */
	while (!headers_end && got < WINDOW - 1) {
		n = conn_read(&c, window + got, WINDOW - 1 - got);
		if (n <= 0)
			break;
		got += n;
		window[got] = 0;
		headers_end = strstr(window, "\r\n\r\n");
	}
	if (!headers_end) {
		set_message(got ? "That isn't a web page" : "The site closed the link");
		goto done;
	}
	result = read_headers(url, window, headers_end, &b, &plain);
	if (result != 1)
		goto done;
	*out = page_new();
	if (!*out) {
		set_message("Not enough memory");
		result = 0;
		goto done;
	}
	{
		int rest = got - (int)(headers_end + 4 - window);
		memcpy(raw, headers_end + 4, (size_t)rest);
		body_bytes = rest;
		layout_start(&l, *out, plain);
		read_body(&c, &b, &l, window, raw, rest);
		layout_finish(&l);
	}
	if ((*out)->refresh[0] && (*out)->lines < 16) {
		char base[MAX_URL];
		snprintf(base, sizeof(base), "%s", url);
		url_join(url, base, (*out)->refresh);
		page_free(*out);
		*out = NULL;
		result = 2;
	}
done:
	conn_close(&c);
	free(window);
	if (want)
		result = 0;
	return result;
}

static void publish(page_t *p, const char *url)
{
	snprintf(p->url, sizeof(p->url), "%s", url);
	if (!p->title[0])
		snprintf(p->title, sizeof(p->title), "%s", url);
	printf("web: %s, %d lines from %d bytes\n", url, p->lines, body_bytes);
	page_free(ready);
	ready = p;
	set_message("");
	state = WEB_DONE;
}

static void load_about(char *url)
{
	static layout_t l;
	page_t *p = page_new();
	if (!p) {
		set_message("Not enough memory");
		state = WEB_FAILED;
		return;
	}
	layout_start(&l, p, 0);
	layout_feed(&l, home_page, home_page + sizeof(home_page) - 1, 1);
	layout_finish(&l);
	publish(p, url);
}

static void load(char *url, const char *post)
{
	page_t *p = NULL;
	int tries, result = 0;
	body_bytes = 0;
	for (tries = 0; tries <= MAX_REDIRECTS; tries++) {
		result = fetch_once(url, tries ? NULL : post, &p);
		if (result != 2)
			break;
	}
	if (result == 2)
		set_message("Too many redirects");
	if (result == 1) {
		publish(p, url);
	} else {
		page_free(p);
		printf("web: %s failed: %s\n", url, message);
		state = want ? WEB_BUSY : WEB_FAILED;
	}
}

static void *fetch_thread(void *arg)
{
	(void)arg;
	for (;;) {
		if (want) {
			char url[MAX_URL], post[MAX_QUERY];
			int is_post = want_body[0] != 0;
			snprintf(url, sizeof(url), "%s", want_url);
			snprintf(post, sizeof(post), "%s", want_body);
			want = 0;
			load(url, is_post ? post : NULL);
		}
		plat_sleep_ms(20);
	}
	return NULL;
}

/* --- what the guest calls --- */

static void request(const char *url, int keep_history)
{
	want_keep_history = keep_history;
	/* Built-in pages need no network, and this keeps harness replays deterministic */
	if (!strncmp(url, "about:", 6)) {
		char copy[MAX_URL];
		snprintf(copy, sizeof(copy), "%s", url);
		load_about(copy);
		return;
	}
	if (!started) {
		started = 1;
		plat_thread_start(fetch_thread);
	}
	snprintf(want_url, sizeof(want_url), "%s", url);
	want_body[0] = 0;
	state = WEB_BUSY;
	set_message("Starting");
	want = 1;
}

static void url_encode(char *out, size_t size, const char *text)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = strlen(out);
	for (; *text && n + 4 < size; text++) {
		unsigned char c = (unsigned char)*text;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.') {
			out[n++] = (char)c;
		} else if (c == ' ') {
			out[n++] = '+';
		} else {
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		}
	}
	out[n] = 0;
}

/* Google's results need JavaScript, so its searches go somewhere that doesn't */
static void search_elsewhere(char *url)
{
	const char *host = strstr(url, "://"), *q;
	if (!host || strncmp(host + 3, "www.google.", 11) || !strstr(host, "/search?"))
		return;
	q = strstr(host, "?q=");
	if (!q)
		q = strstr(host, "&q=");
	if (q) {
		char copy[MAX_URL];
		snprintf(copy, sizeof(copy), "%.*s", (int)strcspn(q + 3, "&"), q + 3);
		snprintf(url, MAX_URL, "%s%s", SEARCH_URL, copy);
	}
}

void web_open(const char *text)
{
	char url[MAX_URL];
	if (strstr(text, "://") || !strncmp(text, "about:", 6)) {
		snprintf(url, sizeof(url), "%s", text);
	} else if (strchr(text, ' ') || !strchr(text, '.')) {
		snprintf(url, sizeof(url), "%s", SEARCH_URL);
		url_encode(url, sizeof(url), text);
	} else {
		snprintf(url, sizeof(url), "http://%s", text);
	}
	search_elsewhere(url);
	request(url, 1);
}

void web_back(void)
{
	if (history_used > 0)
		request(history[--history_used], 0);
}

int web_status(int *lines, int *bytes)
{
	int now = state;
	if (now == WEB_DONE && ready) {
		page_t *p = ready;
		ready = NULL;
		if (shown && want_keep_history) {
			if (history_used == HISTORY) {
				memmove(history[0], history[1], sizeof(history[0]) * (HISTORY - 1));
				history_used--;
			}
			snprintf(history[history_used++], MAX_URL, "%s", shown->url);
		}
		page_free(shown);
		shown = p;
		active_field = -1;
	}
	*lines = shown ? shown->lines : 0;
	*bytes = body_bytes;
	return now;
}

void web_info(web_info_t *info)
{
	memset(info, 0, sizeof(*info));
	if (shown) {
		snprintf(info->title, sizeof(info->title), "%s", shown->title);
		snprintf(info->url, sizeof(info->url), "%s", shown->url);
	}
	snprintf(info->message, sizeof(info->message), "%s", message);
}

int web_line(int line, uint8_t *out, int size)
{
	int n;
	if (!shown || line < 0 || line >= shown->lines)
		return 0;
	n = (int)(shown->line_at[line + 1] - shown->line_at[line]);
	if (n > size)
		n = size;
	memcpy(out, shown->text + shown->line_at[line], (size_t)n);
	return n;
}

/* Sends a form: every named field, plus the button that was pressed */
static void submit(int form, int button)
{
	const form_t *f = &shown->forms[form];
	char query[MAX_QUERY], url[MAX_URL];
	int i;
	query[0] = 0;
	for (i = 0; i < shown->field_count; i++) {
		const field_t *field = &shown->fields[i];
		if (field->form != form || field->name == NO_LINK ||
		    (field->kind == FIELD_SUBMIT && i != button))
			continue;
		if (query[0])
			strncat(query, "&", sizeof(query) - strlen(query) - 1);
		url_encode(query, sizeof(query), shown->link_pool + field->name);
		strncat(query, "=", sizeof(query) - strlen(query) - 1);
		if (field->value != NO_LINK)
			url_encode(query, sizeof(query), shown->link_pool + field->value);
	}
	url_join(url, shown->url, f->action == NO_LINK ? "" : shown->link_pool + f->action);
	if (f->post) {
		request(url, 1);
		if (want) { /* about: never posts */
			want = 0;
			snprintf(want_body, sizeof(want_body), "%s", query);
			want = 1;
		}
	} else {
		char *mark = strchr(url, '?');
		size_t n;
		if (mark)
			*mark = 0;
		n = strlen(url);
		snprintf(url + n, sizeof(url) - n, "?%s", query);
		search_elsewhere(url);
		request(url, 1);
	}
}

int web_click(int line, int column)
{
	const uint8_t *t, *end;
	if (!shown || line < 0 || line >= shown->lines)
		return 0;
	t = shown->text + shown->line_at[line];
	end = shown->text + shown->line_at[line + 1];
	while (t < end) {
		int link = (t[3] << 8) | t[4];
		if (column >= t[0] && column < t[0] + t[1] && link != NO_LINK) {
			if (link & FIELD_LINK) {
				int index = link & ~FIELD_LINK;
				if (shown->fields[index].kind == FIELD_TEXT) {
					active_field = index;
					return 2;
				}
				submit(shown->fields[index].form, index);
			} else {
				char url[MAX_URL];
				url_join(url, shown->url, shown->link_pool + shown->link_at[link]);
				search_elsewhere(url);
				request(url, 1);
			}
			return 1;
		}
		t += RUN_HEADER + t[1];
	}
	return 0;
}

void web_field_get(char *text, int size)
{
	const field_t *f = active_field >= 0 ? &shown->fields[active_field] : NULL;
	snprintf(text, (size_t)size, "%s", f && f->value != NO_LINK ? shown->link_pool + f->value : "");
}

/* Stores the text in the page, shows it, and presses the form's first button */
void web_field_set(const char *text)
{
	field_t *f;
	char *value;
	int i;
	if (active_field < 0 || !shown)
		return;
	f = &shown->fields[active_field];
	if (f->value == NO_LINK)
		return;
	value = shown->link_pool + f->value;
	snprintf(value, VALUE_MAX, "%s", text);
	for (i = 0; i < f->width; i++)
		shown->text[f->text_at + (uint32_t)i] = i < (int)strlen(value) ? (uint8_t)value[i] : ' ';
	for (i = 0; i < shown->field_count; i++)
		if (shown->fields[i].form == f->form && shown->fields[i].kind == FIELD_SUBMIT) {
			submit(f->form, i);
			return;
		}
	submit(f->form, -1);
}
