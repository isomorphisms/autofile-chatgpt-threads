#ifndef AUTOFILE_API_H
#define AUTOFILE_API_H

#include <stddef.h>

enum {
    AF_ID_CAPACITY = 257,
    AF_NAME_CAPACITY = 513,
    AF_PATH_CAPACITY = 4096,
    AF_BODY_CAPACITY = 1024,
    AF_WORKSPACE_CAPACITY = 129,
    AF_DIGEST_CAPACITY = 65,
    AF_MAX_PAGES = 100
};

typedef enum { AF_HTTP_GET = 1, AF_HTTP_PATCH } af_http_method;
typedef enum {
    AF_TRANSPORT_DELIVERED = 0,
    AF_TRANSPORT_REFUSED,
    AF_TRANSPORT_AMBIGUOUS
} af_transport_result;

typedef struct {
    af_http_method method;
    char path[AF_PATH_CAPACITY];
    char body[AF_BODY_CAPACITY];
    size_t maximum_response_bytes;
    int retry_permitted;
} af_request;

typedef struct {
    unsigned status;
    const unsigned char *body;
    size_t body_length;
    const char *workspace_fingerprint;
} af_response;

typedef af_transport_result (*af_transport)(void *context,
                                            const af_request *request,
                                            af_response *response);

typedef struct {
    af_transport send;
    void *context;
    const char *expected_workspace_fingerprint;
    size_t maximum_json_work;
} af_client;

typedef struct {
    char id[AF_ID_CAPACITY];
    char name[AF_NAME_CAPACITY];
    int can_write;
} af_project;

typedef struct {
    char id[AF_ID_CAPACITY];
    char project_id[AF_ID_CAPACITY];
} af_thread;

typedef enum {
    AF_OK = 0,
    AF_INVALID_ARGUMENT,
    AF_NOT_AUTHORIZED,
    AF_WRONG_WORKSPACE,
    AF_AUTHENTICATION_REQUIRED,
    AF_RATE_LIMITED,
    AF_PROVIDER_REJECTED,
    AF_TRANSPORT_UNKNOWN,
    AF_SCHEMA_CHANGED,
    AF_CAPACITY_EXCEEDED,
    AF_CURSOR_CYCLE,
    AF_DUPLICATE_CONFLICT,
    AF_PROJECT_NOT_FOUND,
    AF_PROJECT_NOT_WRITABLE,
    AF_THREAD_NOT_FOUND,
    AF_THREAD_INELIGIBLE,
    AF_STALE_SOURCE
} af_status;

typedef struct {
    const char *thread_id;
    const char *destination_project_id;
    const char *expected_source_project_id; /* empty means outside Projects */
    const char *approval_digest;
    const char *expected_approval_digest;
    int execute;
} af_move_intent;

typedef enum {
    AF_MOVE_BLOCKED = 0,
    AF_MOVE_DRY_RUN,
    AF_MOVE_VERIFIED_NOOP,
    AF_MOVE_VERIFIED,
    AF_MOVE_UNKNOWN
} af_move_verdict;

typedef struct {
    af_move_verdict verdict;
    af_status status;
    char thread_id[AF_ID_CAPACITY];
    char before_project_id[AF_ID_CAPACITY];
    char destination_project_id[AF_ID_CAPACITY];
    char after_project_id[AF_ID_CAPACITY];
    char approval_digest[AF_DIGEST_CAPACITY];
    unsigned patch_status;
    unsigned verification_status;
    unsigned patch_attempts;
} af_move_receipt;

af_status af_list_projects(const af_client *client, af_project *projects,
                           size_t capacity, size_t *count);
af_status af_list_project_threads(const af_client *client,
                                  const char *project_id,
                                  af_thread *threads, size_t capacity,
                                  size_t *count);
af_status af_get_thread(const af_client *client, const char *thread_id,
                        af_thread *thread, unsigned *http_status);
af_status af_move_thread(const af_client *client,
                         const af_move_intent *intent,
                         af_move_receipt *receipt);
const char *af_status_name(af_status status);
const char *af_move_verdict_name(af_move_verdict verdict);

#endif

