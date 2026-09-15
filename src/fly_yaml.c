#include "fly_yaml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int indent; /* leading spaces */
    /* trimmed content; owned by this struct — the parser rewrites it in place
     * for inline map items and frees it, so it is deliberately not const */
    char *text;
} fly__line;

static char *fly__strdup_n(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static fly_yaml_node *fly__node_new(fly_yaml_kind kind) {
    fly_yaml_node *n = (fly_yaml_node *)calloc(1, sizeof(fly_yaml_node));
    if (n) n->kind = kind;
    return n;
}

static void fly__node_append(fly_yaml_node *parent, fly_yaml_node *child) {
    if (!parent->child) { parent->child = child; return; }
    fly_yaml_node *c = parent->child;
    while (c->next) c = c->next;
    c->next = child;
}

/* strip surrounding quotes and trailing whitespace/comment from a scalar */
static char *fly__scalar_dup(const char *s, size_t len) {
    /* trim */
    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) { ++s; --len; }
    /* strip unquoted trailing comment */
    if (len > 0 && s[0] != '"' && s[0] != '\'') {
        size_t i;
        for (i = 0; i < len; ++i)
            if (s[i] == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) { len = i; break; }
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) --len;
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') || (s[0] == '\'' && s[len - 1] == '\''))) {
        ++s;
        len -= 2;
    }
    return fly__strdup_n(s, len);
}

/* parse "[a, b, c]" into a SEQ of scalars */
static fly_yaml_node *fly__flow_seq(const char *s, size_t len) {
    fly_yaml_node *seq = fly__node_new(FLY_YAML_SEQ);
    if (!seq) return NULL;
    ++s; len -= 2; /* strip [ ] */
    while (len > 0) {
        size_t n = 0;
        int quoted = 0;
        while (n < len) {
            char c = s[n];
            if (c == '"' || c == '\'') quoted = !quoted;
            else if (c == ',' && !quoted) break;
            ++n;
        }
        fly_yaml_node *item = fly__node_new(FLY_YAML_SCALAR);
        if (!item) break;
        item->scalar = fly__scalar_dup(s, n);
        fly__node_append(seq, item);
        if (n < len) ++n; /* skip comma */
        s += n; len -= n;
    }
    return seq;
}

/* create value node for text right of "key:" or "- " */
static fly_yaml_node *fly__value_node(const char *s, size_t len) {
    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) { ++s; --len; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) --len;
    if (len >= 2 && s[0] == '[' && s[len - 1] == ']') return fly__flow_seq(s, len);
    fly_yaml_node *n = fly__node_new(FLY_YAML_SCALAR);
    if (n) n->scalar = fly__scalar_dup(s, len);
    return n;
}

/* find "key:" split point outside quotes; returns index of ':' or -1 */
static int fly__find_colon(const char *s, size_t len) {
    size_t i;
    int quoted = 0;
    for (i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '"' || c == '\'') quoted = !quoted;
        else if (c == '#' && !quoted) return -1;
        else if (c == ':' && !quoted && (i + 1 == len || s[i + 1] == ' ' || s[i + 1] == '\t' || s[i + 1] == '\r'))
            return (int)i;
    }
    return -1;
}

static void fly__parse_map(fly_yaml_node *parent, fly__line *lines, int nlines, int *idx, int indent);
static void fly__parse_seq(fly_yaml_node *parent, fly__line *lines, int nlines, int *idx, int indent);

/* parse the block starting at *idx into parent, deciding map vs seq */
static void fly__parse_block(fly_yaml_node *parent, fly__line *lines, int nlines, int *idx, int indent) {
    if (*idx >= nlines || lines[*idx].indent < indent) return;
    if (lines[*idx].text[0] == '-' &&
        (lines[*idx].text[1] == '\0' || lines[*idx].text[1] == ' ')) {
        parent->kind = FLY_YAML_SEQ;
        fly__parse_seq(parent, lines, nlines, idx, lines[*idx].indent);
    } else {
        parent->kind = FLY_YAML_MAP;
        fly__parse_map(parent, lines, nlines, idx, lines[*idx].indent);
    }
}

static void fly__parse_map(fly_yaml_node *parent, fly__line *lines, int nlines, int *idx, int indent) {
    while (*idx < nlines) {
        fly__line *ln = &lines[*idx];
        if (ln->indent < indent) return;
        if (ln->indent > indent) { ++*idx; continue; } /* stray deeper line: skip */
        const char *s = ln->text;
        size_t len = strlen(s);
        int colon = fly__find_colon(s, len);
        if (colon < 0) { ++*idx; continue; } /* not a map entry: skip */
        char *key = fly__scalar_dup(s, (size_t)colon);
        const char *rest = s + colon + 1;
        size_t rlen = len - (size_t)colon - 1;
        while (rlen > 0 && (rest[0] == ' ' || rest[0] == '\t')) { ++rest; --rlen; }
        /* trailing comment-only value counts as empty */
        if (rlen > 0 && rest[0] == '#') rlen = 0;
        ++*idx;
        fly_yaml_node *val;
        if (rlen == 0) {
            /* nested block or empty scalar */
            if (*idx < nlines && lines[*idx].indent > indent) {
                val = fly__node_new(FLY_YAML_MAP);
                if (val) fly__parse_block(val, lines, nlines, idx, indent + 1);
            } else {
                val = fly__node_new(FLY_YAML_SCALAR);
                if (val) val->scalar = fly__strdup_n("", 0);
            }
        } else {
            val = fly__value_node(rest, rlen);
        }
        if (!val) { free(key); return; }
        val->key = key;
        fly__node_append(parent, val);
    }
}

static void fly__parse_seq(fly_yaml_node *parent, fly__line *lines, int nlines, int *idx, int indent) {
    while (*idx < nlines) {
        fly__line *ln = &lines[*idx];
        if (ln->indent < indent) return;
        if (ln->indent > indent || ln->text[0] != '-') { ++*idx; continue; }
        const char *s = ln->text + 1;
        size_t len = strlen(s);
        while (len > 0 && (s[0] == ' ' || s[0] == '\t')) { ++s; --len; }
        fly_yaml_node *item;
        if (len == 0) {
            /* "-" alone: nested block item */
            ++*idx;
            item = fly__node_new(FLY_YAML_MAP);
            if (item && *idx < nlines && lines[*idx].indent > indent)
                fly__parse_block(item, lines, nlines, idx, indent + 1);
        } else {
            int colon = fly__find_colon(s, len);
            if (colon >= 0) {
                /* "- key: value" starts an inline map item; rewrite this line as
                 * a map entry at the item's effective indent and reparse */
                int item_indent = ln->indent + (int)(s - ln->text);
                memmove(ln->text, s, len + 1); /* keep heap base for free() */
                ln->indent = item_indent;
                item = fly__node_new(FLY_YAML_MAP);
                if (item) fly__parse_map(item, lines, nlines, idx, item_indent);
            } else {
                ++*idx;
                item = fly__value_node(s, len);
            }
        }
        if (!item) return;
        fly__node_append(parent, item);
    }
}

fly_yaml_node *fly_yaml_parse(const char *text) {
    if (!text) return NULL;
    /* split into significant lines */
    int cap = 64, nlines = 0;
    fly__line *lines = (fly__line *)malloc(sizeof(fly__line) * (size_t)cap);
    if (!lines) return NULL;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        int ind = 0;
        while ((size_t)ind < len && p[ind] == ' ') ++ind;
        const char *body = p + ind;
        size_t blen = len - (size_t)ind;
        while (blen > 0 && (body[blen - 1] == '\r' || body[blen - 1] == ' ' || body[blen - 1] == '\t')) --blen;
        if (blen > 0 && body[0] != '#' && !(blen >= 3 && strncmp(body, "---", 3) == 0)) {
            if (nlines == cap) {
                cap *= 2;
                fly__line *nl = (fly__line *)realloc(lines, sizeof(fly__line) * (size_t)cap);
                if (!nl) break;
                lines = nl;
            }
            lines[nlines].indent = ind;
            lines[nlines].text = fly__strdup_n(body, blen);
            ++nlines;
        }
        if (!eol) break;
        p = eol + 1;
    }
    fly_yaml_node *root = fly__node_new(FLY_YAML_MAP);
    int idx = 0;
    if (root && nlines > 0) fly__parse_block(root, lines, nlines, &idx, 0);
    int i;
    for (i = 0; i < nlines; ++i) free(lines[i].text);
    free(lines);
    return root;
}

fly_yaml_node *fly_yaml_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    fly_yaml_node *n = fly_yaml_parse(buf);
    free(buf);
    return n;
}

void fly_yaml_free(fly_yaml_node *n) {
    while (n) {
        fly_yaml_node *next = n->next;
        fly_yaml_free(n->child);
        free(n->key);
        free(n->scalar);
        free(n);
        n = next;
    }
}

const fly_yaml_node *fly_yaml_get(const fly_yaml_node *n, const char *path) {
    if (!n) return NULL;
    if (!path || !*path) return n;
    while (n && path && *path) {
        const char *dot = strchr(path, '.');
        size_t klen = dot ? (size_t)(dot - path) : strlen(path);
        const fly_yaml_node *c = n->child;
        const fly_yaml_node *found = NULL;
        while (c) {
            if (c->key && strlen(c->key) == klen && strncmp(c->key, path, klen) == 0) { found = c; break; }
            c = c->next;
        }
        n = found;
        path = dot ? dot + 1 : NULL;
    }
    return n;
}

const char *fly_yaml_str(const fly_yaml_node *n, const char *path, const char *dflt) {
    const fly_yaml_node *v = fly_yaml_get(n, path);
    return (v && v->kind == FLY_YAML_SCALAR && v->scalar) ? v->scalar : dflt;
}

double fly_yaml_num(const fly_yaml_node *n, const char *path, double dflt) {
    const char *s = fly_yaml_str(n, path, NULL);
    if (!s || !*s) return dflt;
    char *end = NULL;
    double v = strtod(s, &end);
    return (end && end != s) ? v : dflt;
}

int fly_yaml_int(const fly_yaml_node *n, const char *path, int dflt) {
    return (int)fly_yaml_num(n, path, (double)dflt);
}

int fly_yaml_bool(const fly_yaml_node *n, const char *path, int dflt) {
    const char *s = fly_yaml_str(n, path, NULL);
    if (!s) return dflt;
    if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "on") == 0 || strcmp(s, "1") == 0) return 1;
    if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 || strcmp(s, "off") == 0 || strcmp(s, "0") == 0) return 0;
    return dflt;
}

int fly_yaml_count(const fly_yaml_node *n) {
    int c = 0;
    const fly_yaml_node *v = n ? n->child : NULL;
    while (v) { ++c; v = v->next; }
    return c;
}

const fly_yaml_node *fly_yaml_at(const fly_yaml_node *n, int idx) {
    const fly_yaml_node *v = n ? n->child : NULL;
    while (v && idx-- > 0) v = v->next;
    return v;
}
