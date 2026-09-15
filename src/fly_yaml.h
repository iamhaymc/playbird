/* fly_yaml: self-contained YAML-subset loader for assets, config and saves.
 *
 * Supported: block mappings, block sequences ("- item", "- key: value"),
 * nested structures by indentation, single/double-quoted scalars, flow
 * sequences of scalars ("[a, b, c]"), comments ("#"), blank lines.
 * Not supported (not needed here): anchors, aliases, tags, multi-doc,
 * flow mappings, block scalars.
 */
#ifndef FLY_YAML_H
#define FLY_YAML_H

typedef enum {
    FLY_YAML_SCALAR,
    FLY_YAML_MAP,
    FLY_YAML_SEQ
} fly_yaml_kind;

typedef struct fly_yaml_node {
    fly_yaml_kind kind;
    char *key;    /* set when the parent is a map, else NULL */
    char *scalar; /* set for FLY_YAML_SCALAR, else NULL */
    struct fly_yaml_node *child; /* first child (map entries / seq items) */
    struct fly_yaml_node *next;  /* next sibling */
} fly_yaml_node;

fly_yaml_node *fly_yaml_parse(const char *text); /* returns root MAP, NULL on error */
fly_yaml_node *fly_yaml_load_file(const char *path);
void fly_yaml_free(fly_yaml_node *n);

/* dotted-path lookup from a map node, e.g. "wing.area"; NULL path = n itself */
const fly_yaml_node *fly_yaml_get(const fly_yaml_node *n, const char *path);
const char *fly_yaml_str(const fly_yaml_node *n, const char *path, const char *dflt);
double fly_yaml_num(const fly_yaml_node *n, const char *path, double dflt);
int fly_yaml_int(const fly_yaml_node *n, const char *path, int dflt);
int fly_yaml_bool(const fly_yaml_node *n, const char *path, int dflt);

int fly_yaml_count(const fly_yaml_node *n); /* number of children */
const fly_yaml_node *fly_yaml_at(const fly_yaml_node *n, int idx);

#endif /* FLY_YAML_H */
