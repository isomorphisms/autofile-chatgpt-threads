#include "autofile_api.h"
#include "af_json.h"

#include <stdio.h>
#include <string.h>

static int identifier(const char *value)
{
    size_t length = 0;
    if (value == NULL) return 0;
    while (value[length] != '\0') {
        unsigned char byte = (unsigned char)value[length];
        int accepted = (byte >= 'A' && byte <= 'Z')
            || (byte >= 'a' && byte <= 'z')
            || (byte >= '0' && byte <= '9')
            || byte == '_' || byte == '-';
        if (!accepted || ++length >= AF_ID_CAPACITY) return 0;
    }
    return length != 0;
}

static int project_identifier(const char *value)
{
    return identifier(value) && strncmp(value, "g-p-", 4) == 0;
}

static int digest(const char *value)
{
    if (value == NULL || strlen(value) != 64) return 0;
    for (size_t index = 0; index < 64; ++index) {
        char byte = value[index];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return 0;
    }
    return 1;
}

static int cursor(const char *value)
{
    size_t length = 0;
    if (value == NULL) return 0;
    while (value[length] != '\0') {
        unsigned char byte = (unsigned char)value[length];
        if (byte < 32 || byte == 127 || ++length > 2048) return 0;
    }
    return length != 0;
}

static int append_encoded(char *path, size_t capacity, const char *value)
{
    size_t used = strlen(path);
    if (!cursor(value)) return 0;
    for (size_t index = 0; value[index] != '\0'; ++index) {
        unsigned char byte = (unsigned char)value[index];
        int unreserved = (byte >= 'A' && byte <= 'Z')
            || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '.' || byte == '_' || byte == '~';
        int written = snprintf(path + used, capacity - used,
                               unreserved ? "%c" : "%%%02X", (unsigned)byte);
        if (written < 0 || (size_t)written >= capacity - used) return 0;
        used += (size_t)written;
    }
    return 1;
}

static af_status map_http(unsigned status)
{
    if (status == 401 || status == 403) return AF_AUTHENTICATION_REQUIRED;
    if (status == 404) return AF_THREAD_NOT_FOUND;
    if (status == 429) return AF_RATE_LIMITED;
    return AF_PROVIDER_REJECTED;
}

static af_status send(const af_client *client, const af_request *request,
                      af_response *response, af_transport_result *transport)
{
    if (client == NULL || client->send == NULL || response == NULL
        || request == NULL || client->expected_workspace_fingerprint == NULL
        || client->expected_workspace_fingerprint[0] == '\0') return AF_INVALID_ARGUMENT;
    memset(response, 0, sizeof(*response));
    *transport = client->send(client->context, request, response);
    if (*transport == AF_TRANSPORT_REFUSED) return AF_PROVIDER_REJECTED;
    if (response->workspace_fingerprint == NULL
        || strcmp(response->workspace_fingerprint,
                  client->expected_workspace_fingerprint) != 0) return AF_WRONG_WORKSPACE;
    if (*transport == AF_TRANSPORT_AMBIGUOUS) return AF_TRANSPORT_UNKNOWN;
    if (response->body_length > request->maximum_response_bytes) return AF_CAPACITY_EXCEEDED;
    if (response->status < 200 || response->status >= 300) return map_http(response->status);
    return AF_OK;
}

static af_status parse(const af_client *client, const af_response *response,
                       afj_document *document)
{
    size_t ceiling = client->maximum_json_work == 0 ? 4000000 : client->maximum_json_work;
    if (response->body == NULL || response->body_length == 0
        || !afj_parse(document, response->body, response->body_length, ceiling))
        return AF_SCHEMA_CHANGED;
    return AF_OK;
}

static int text(afj_document *document, size_t object, const char *member,
                char *output, size_t capacity)
{
    return afj_text(document, afj_member(document, object, member), output, capacity);
}

static int optional_project(afj_document *document, size_t object,
                            char *output, size_t capacity)
{
    size_t member = afj_member(document, object, "gizmo_id");
    if (member == AFJ_MISSING) return 0;
    if (document->tokens[member].kind == AFJ_NULL) {
        output[0] = '\0';
        return 1;
    }
    return afj_text(document, member, output, capacity);
}

static size_t project_object(afj_document *document, size_t item)
{
    size_t first, second;
    if (item >= document->count || document->tokens[item].kind != AFJ_OBJECT) return AFJ_MISSING;
    first = afj_member(document, item, "gizmo");
    if (first == AFJ_MISSING || document->tokens[first].kind != AFJ_OBJECT) return item;
    second = afj_member(document, first, "gizmo");
    return second != AFJ_MISSING && document->tokens[second].kind == AFJ_OBJECT
        ? second : first;
}

static af_status parse_project_item(afj_document *document, size_t item,
                                    af_project *project, int *archived)
{
    size_t object = project_object(document, item);
    size_t display, permissions, archived_token;
    int writable;
    if (object == AFJ_MISSING || !text(document, object, "id", project->id,
                                       sizeof(project->id))
        || !project_identifier(project->id)) return AF_SCHEMA_CHANGED;
    display = afj_member(document, object, "display");
    if (display == AFJ_MISSING || document->tokens[display].kind != AFJ_OBJECT
        || !text(document, display, "name", project->name, sizeof(project->name)))
        return AF_SCHEMA_CHANGED;
    permissions = afj_member(document, object, "current_user_permission");
    if (permissions == AFJ_MISSING
        || document->tokens[permissions].kind != AFJ_OBJECT
        || !afj_boolean(document, afj_member(document, permissions, "can_write"),
                        &writable)) return AF_SCHEMA_CHANGED;
    project->can_write = writable;
    archived_token = afj_member(document, object, "is_archived");
    if (archived_token == AFJ_MISSING) *archived = 0;
    else if (!afj_boolean(document, archived_token, archived)) return AF_SCHEMA_CHANGED;
    return AF_OK;
}

static int same_project(const af_project *left, const af_project *right)
{
    return strcmp(left->id, right->id) == 0
        && strcmp(left->name, right->name) == 0
        && left->can_write == right->can_write;
}

af_status af_list_projects(const af_client *client, af_project *projects,
                           size_t capacity, size_t *count)
{
    char next[2049] = {0};
    char seen[AF_MAX_PAGES][2049];
    size_t seen_count = 0, used = 0;
    if (projects == NULL || capacity == 0 || count == NULL) return AF_INVALID_ARGUMENT;
    *count = 0;
    for (size_t page = 0; page < AF_MAX_PAGES; ++page) {
        af_request request = {0};
        af_response response;
        af_transport_result transport;
        afj_document document;
        af_status status;
        request.method = AF_HTTP_GET;
        request.maximum_response_bytes = 20000000;
        request.retry_permitted = 1;
        if (snprintf(request.path, sizeof(request.path),
                     "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20%s",
                     next[0] == '\0' ? "" : "&cursor=") >= (int)sizeof(request.path))
            return AF_CAPACITY_EXCEEDED;
        if (next[0] != '\0' && !append_encoded(request.path, sizeof(request.path), next))
            return AF_INVALID_ARGUMENT;
        status = send(client, &request, &response, &transport);
        if (status != AF_OK) return status;
        status = parse(client, &response, &document);
        if (status != AF_OK) return status;
        size_t items = afj_member(&document, 0, "items");
        size_t item_count = afj_array_count(&document, items);
        if (item_count == AFJ_MISSING) { afj_free(&document); return AF_SCHEMA_CHANGED; }
        for (size_t index = 0; index < item_count; ++index) {
            af_project candidate = {0};
            int archived;
            status = parse_project_item(&document,
                                        afj_array_item(&document, items, index),
                                        &candidate, &archived);
            if (status != AF_OK) { afj_free(&document); return status; }
            size_t duplicate;
            for (duplicate = 0; duplicate < used; ++duplicate)
                if (strcmp(projects[duplicate].id, candidate.id) == 0) break;
            if (duplicate < used) {
                if (!same_project(&projects[duplicate], &candidate)) {
                    afj_free(&document); return AF_DUPLICATE_CONFLICT;
                }
            } else if (!archived) {
                if (used == capacity) { afj_free(&document); return AF_CAPACITY_EXCEEDED; }
                projects[used++] = candidate;
            }
        }
        size_t cursor_token = afj_member(&document, 0, "cursor");
        if (cursor_token == AFJ_MISSING || document.tokens[cursor_token].kind == AFJ_NULL
            || document.tokens[cursor_token].kind == AFJ_FALSE) {
            afj_free(&document); *count = used; return AF_OK;
        }
        char decoded[2049];
        if (!afj_text(&document, cursor_token, decoded, sizeof(decoded)) || decoded[0] == '\0') {
            afj_free(&document); return AF_SCHEMA_CHANGED;
        }
        afj_free(&document);
        if (item_count == 0) return AF_SCHEMA_CHANGED;
        for (size_t old = 0; old < seen_count; ++old)
            if (strcmp(seen[old], decoded) == 0) return AF_CURSOR_CYCLE;
        if (seen_count == AF_MAX_PAGES) return AF_CAPACITY_EXCEEDED;
        strcpy(seen[seen_count++], decoded);
        strcpy(next, decoded);
    }
    return AF_CAPACITY_EXCEEDED;
}

static af_status parse_thread(afj_document *document, size_t object,
                              af_thread *thread)
{
    if (object >= document->count || document->tokens[object].kind != AFJ_OBJECT
        || !text(document, object, "id", thread->id, sizeof(thread->id))
        || !identifier(thread->id)
        || !optional_project(document, object, thread->project_id,
                             sizeof(thread->project_id))) return AF_SCHEMA_CHANGED;
    return AF_OK;
}

af_status af_list_project_threads(const af_client *client,
                                  const char *project_id,
                                  af_thread *threads, size_t capacity,
                                  size_t *count)
{
    char next[2049] = "0";
    char seen[AF_MAX_PAGES][2049];
    size_t seen_count = 0, used = 0;
    if (!project_identifier(project_id) || threads == NULL || capacity == 0
        || count == NULL) return AF_INVALID_ARGUMENT;
    *count = 0;
    for (size_t page = 0; page < AF_MAX_PAGES; ++page) {
        af_request request = {0};
        af_response response;
        af_transport_result transport;
        afj_document document;
        af_status status;
        request.method = AF_HTTP_GET;
        request.maximum_response_bytes = 20000000;
        request.retry_permitted = 1;
        if (snprintf(request.path, sizeof(request.path),
                     "/backend-api/gizmos/%s/conversations?cursor=", project_id)
            >= (int)sizeof(request.path)
            || !append_encoded(request.path, sizeof(request.path), next))
            return AF_CAPACITY_EXCEEDED;
        status = send(client, &request, &response, &transport);
        if (status != AF_OK) return status;
        status = parse(client, &response, &document);
        if (status != AF_OK) return status;
        size_t items = afj_member(&document, 0, "items");
        size_t item_count = afj_array_count(&document, items);
        if (item_count == AFJ_MISSING) { afj_free(&document); return AF_SCHEMA_CHANGED; }
        for (size_t index = 0; index < item_count; ++index) {
            size_t object = afj_array_item(&document, items, index);
            af_thread candidate = {0};
            if (object >= document.count || document.tokens[object].kind != AFJ_OBJECT
                || !text(&document, object, "id", candidate.id, sizeof(candidate.id))
                || !identifier(candidate.id)) { afj_free(&document); return AF_SCHEMA_CHANGED; }
            strcpy(candidate.project_id, project_id);
            for (size_t old = 0; old < used; ++old)
                if (strcmp(threads[old].id, candidate.id) == 0) {
                    afj_free(&document); return AF_DUPLICATE_CONFLICT;
                }
            if (used == capacity) { afj_free(&document); return AF_CAPACITY_EXCEEDED; }
            threads[used++] = candidate;
        }
        size_t cursor_token = afj_member(&document, 0, "cursor");
        if (cursor_token == AFJ_MISSING || document.tokens[cursor_token].kind == AFJ_NULL
            || document.tokens[cursor_token].kind == AFJ_FALSE) {
            afj_free(&document); *count = used; return AF_OK;
        }
        char decoded[2049];
        if (!afj_text(&document, cursor_token, decoded, sizeof(decoded)) || decoded[0] == '\0') {
            afj_free(&document); return AF_SCHEMA_CHANGED;
        }
        afj_free(&document);
        if (item_count == 0) return AF_SCHEMA_CHANGED;
        for (size_t old = 0; old < seen_count; ++old)
            if (strcmp(seen[old], decoded) == 0) return AF_CURSOR_CYCLE;
        strcpy(seen[seen_count++], decoded);
        strcpy(next, decoded);
    }
    return AF_CAPACITY_EXCEEDED;
}

af_status af_get_thread(const af_client *client, const char *thread_id,
                        af_thread *thread, unsigned *http_status)
{
    af_request request = {0};
    af_response response;
    af_transport_result transport;
    afj_document document;
    af_status status;
    if (!identifier(thread_id) || thread == NULL) return AF_INVALID_ARGUMENT;
    memset(thread, 0, sizeof(*thread));
    request.method = AF_HTTP_GET;
    request.maximum_response_bytes = 100000000;
    request.retry_permitted = 1;
    if (snprintf(request.path, sizeof(request.path),
                 "/backend-api/conversation/%s", thread_id) >= (int)sizeof(request.path))
        return AF_CAPACITY_EXCEEDED;
    status = send(client, &request, &response, &transport);
    if (http_status != NULL) *http_status = response.status;
    if (status != AF_OK) return status;
    status = parse(client, &response, &document);
    if (status != AF_OK) return status;
    status = parse_thread(&document, 0, thread);
    afj_free(&document);
    if (status == AF_OK && strcmp(thread->id, thread_id) != 0) return AF_SCHEMA_CHANGED;
    return status;
}

static void initialize_receipt(const af_move_intent *intent, af_move_receipt *receipt)
{
    memset(receipt, 0, sizeof(*receipt));
    receipt->verdict = AF_MOVE_BLOCKED;
    receipt->status = AF_INVALID_ARGUMENT;
    if (intent == NULL) return;
    if (intent->thread_id != NULL)
        snprintf(receipt->thread_id, sizeof(receipt->thread_id), "%s", intent->thread_id);
    if (intent->destination_project_id != NULL)
        snprintf(receipt->destination_project_id,
                 sizeof(receipt->destination_project_id), "%s",
                 intent->destination_project_id);
    if (intent->approval_digest != NULL)
        snprintf(receipt->approval_digest, sizeof(receipt->approval_digest), "%s",
                 intent->approval_digest);
}

af_status af_move_thread(const af_client *client,
                         const af_move_intent *intent,
                         af_move_receipt *receipt)
{
    af_project projects[256];
    size_t project_count;
    af_thread before, after;
    af_status status;
    unsigned before_http = 0;
    if (receipt == NULL) return AF_INVALID_ARGUMENT;
    initialize_receipt(intent, receipt);
    if (client == NULL || intent == NULL || !identifier(intent->thread_id)
        || !project_identifier(intent->destination_project_id)
        || intent->expected_source_project_id == NULL
        || (intent->expected_source_project_id[0] != '\0'
            && !project_identifier(intent->expected_source_project_id))
        || !digest(intent->approval_digest) || !digest(intent->expected_approval_digest)
        || strcmp(intent->approval_digest, intent->expected_approval_digest) != 0
        || (intent->execute != 0 && intent->execute != 1)) {
        receipt->status = AF_NOT_AUTHORIZED;
        return receipt->status;
    }
    status = af_list_projects(client, projects, 256, &project_count);
    if (status != AF_OK) { receipt->status = status; return status; }
    size_t destination;
    for (destination = 0; destination < project_count; ++destination)
        if (strcmp(projects[destination].id, intent->destination_project_id) == 0) break;
    if (destination == project_count) {
        receipt->status = AF_PROJECT_NOT_FOUND; return receipt->status;
    }
    if (!projects[destination].can_write) {
        receipt->status = AF_PROJECT_NOT_WRITABLE; return receipt->status;
    }
    status = af_get_thread(client, intent->thread_id, &before, &before_http);
    if (status != AF_OK) { receipt->status = status; return status; }
    strcpy(receipt->before_project_id, before.project_id);
    if (before.project_id[0] != '\0' && !project_identifier(before.project_id)) {
        receipt->status = AF_THREAD_INELIGIBLE; return receipt->status;
    }
    if (strcmp(before.project_id, intent->destination_project_id) == 0) {
        strcpy(receipt->after_project_id, before.project_id);
        receipt->verification_status = before_http;
        receipt->status = AF_OK;
        receipt->verdict = AF_MOVE_VERIFIED_NOOP;
        return AF_OK;
    }
    if (strcmp(before.project_id, intent->expected_source_project_id) != 0) {
        receipt->status = AF_STALE_SOURCE; return receipt->status;
    }
    if (!intent->execute) {
        receipt->status = AF_OK;
        receipt->verdict = AF_MOVE_DRY_RUN;
        return AF_OK;
    }

    af_request request = {0};
    af_response response;
    af_transport_result transport;
    request.method = AF_HTTP_PATCH;
    request.maximum_response_bytes = 1000000;
    request.retry_permitted = 0;
    snprintf(request.path, sizeof(request.path), "/backend-api/conversation/%s",
             intent->thread_id);
    snprintf(request.body, sizeof(request.body), "{\"gizmo_id\":\"%s\"}",
             intent->destination_project_id);
    receipt->patch_attempts = 1;
    status = send(client, &request, &response, &transport);
    receipt->patch_status = response.status;
    if (status == AF_WRONG_WORKSPACE || status == AF_AUTHENTICATION_REQUIRED
        || status == AF_RATE_LIMITED) {
        receipt->status = status;
        return status;
    }
    status = af_get_thread(client, intent->thread_id, &after,
                           &receipt->verification_status);
    if (status != AF_OK) {
        receipt->status = AF_TRANSPORT_UNKNOWN;
        receipt->verdict = AF_MOVE_UNKNOWN;
        return receipt->status;
    }
    strcpy(receipt->after_project_id, after.project_id);
    if (strcmp(after.id, intent->thread_id) != 0) {
        receipt->status = AF_SCHEMA_CHANGED;
        receipt->verdict = AF_MOVE_UNKNOWN;
        return receipt->status;
    }
    if (strcmp(after.project_id, intent->destination_project_id) == 0) {
        receipt->status = AF_OK;
        receipt->verdict = AF_MOVE_VERIFIED;
        return AF_OK;
    }
    receipt->status = transport == AF_TRANSPORT_AMBIGUOUS
        ? AF_TRANSPORT_UNKNOWN : AF_PROVIDER_REJECTED;
    receipt->verdict = AF_MOVE_UNKNOWN;
    return receipt->status;
}

const char *af_status_name(af_status status)
{
    static const char *names[] = {
        "ok", "invalid_argument", "not_authorized", "wrong_workspace",
        "authentication_required", "rate_limited", "provider_rejected",
        "transport_unknown", "schema_changed", "capacity_exceeded",
        "cursor_cycle", "duplicate_conflict", "project_not_found",
        "project_not_writable", "thread_not_found", "thread_ineligible",
        "stale_source"
    };
    return status <= AF_STALE_SOURCE ? names[status] : "invalid_status";
}

const char *af_move_verdict_name(af_move_verdict verdict)
{
    static const char *names[] = {
        "blocked", "dry_run", "verified_noop", "verified", "unknown"
    };
    return verdict <= AF_MOVE_UNKNOWN ? names[verdict] : "invalid_verdict";
}

