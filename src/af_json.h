#ifndef AF_JSON_H
#define AF_JSON_H

#include <stddef.h>

enum { AFJ_MAX_TOKENS = 16384, AFJ_MAX_DEPTH = 64 };

typedef enum {
    AFJ_OBJECT,
    AFJ_ARRAY,
    AFJ_STRING,
    AFJ_NUMBER,
    AFJ_TRUE,
    AFJ_FALSE,
    AFJ_NULL
} afj_kind;

typedef struct {
    afj_kind kind;
    size_t begin;
    size_t end;
    size_t next;
} afj_token;

typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t position;
    size_t count;
    size_t work;
    size_t ceiling;
    afj_token *tokens;
} afj_document;

#define AFJ_MISSING ((size_t)-1)

int afj_parse(afj_document *document, const unsigned char *bytes,
              size_t length, size_t work_ceiling);
void afj_free(afj_document *document);
size_t afj_member(afj_document *document, size_t object, const char *name);
int afj_text(const afj_document *document, size_t token,
             char *output, size_t capacity);
int afj_boolean(const afj_document *document, size_t token, int *output);
size_t afj_array_count(const afj_document *document, size_t array);
size_t afj_array_item(const afj_document *document, size_t array, size_t index);

#endif

