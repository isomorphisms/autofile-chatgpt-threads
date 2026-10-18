#include "autofile_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    af_http_method method;
    const char *path;
    const char *body;
    af_transport_result transport;
    unsigned status;
    const char *response;
    const char *workspace;
} step;

typedef struct {
    const step *steps;
    size_t count;
    size_t position;
    unsigned patches;
} fixture;

static unsigned assertions;

static void check(int condition, const char *message)
{
    ++assertions;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static af_transport_result fake_send(void *context, const af_request *request,
                                     af_response *response)
{
    fixture *state = context;
    check(state->position < state->count, "no unexpected request");
    const step *expected = &state->steps[state->position++];
    check(request->method == expected->method, "exact method");
    check(strcmp(request->path, expected->path) == 0, "exact relative path");
    check(strcmp(request->body, expected->body) == 0, "exact request body");
    if (request->method == AF_HTTP_PATCH) {
        ++state->patches;
        check(!request->retry_permitted, "move request is never retryable");
    } else {
        check(request->retry_permitted, "read request is retryable");
    }
    response->status = expected->status;
    response->body = (const unsigned char *)expected->response;
    response->body_length = expected->response == NULL ? 0 : strlen(expected->response);
    response->workspace_fingerprint = expected->workspace;
    return expected->transport;
}

static af_client client(fixture *fixture)
{
    af_client result = { fake_send, fixture, "workspace-sha256", 1000000 };
    return result;
}

static const char *projects =
    "{\"items\":[{\"gizmo\":{\"gizmo\":{\"id\":\"g-p-one\","
    "\"display\":{\"name\":\"One\"},\"current_user_permission\":{\"can_write\":true},"
    "\"is_archived\":false}}}],\"cursor\":null}";

static const char *readonly_projects =
    "{\"items\":[{\"gizmo\":{\"id\":\"g-p-one\","
    "\"display\":{\"name\":\"One\"},\"current_user_permission\":{\"can_write\":false}}}],"
    "\"cursor\":null}";

static af_move_intent intent(int execute)
{
    af_move_intent value = {
        "thread-1", "g-p-one", "",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        execute
    };
    return value;
}

static void expect_done(fixture *state)
{
    check(state->position == state->count, "all expected requests consumed");
}

static void list_primitives(void)
{
    const step project_steps[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"items\":[{\"gizmo\":{\"id\":\"g-p-one\",\"display\":{\"name\":\"One\"},\"current_user_permission\":{\"can_write\":true}}}],\"cursor\":\"a&b\"}", "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20&cursor=a%26b", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"items\":[{\"gizmo\":{\"gizmo\":{\"id\":\"g-p-two\",\"display\":{\"name\":\"Two\"},\"current_user_permission\":{\"can_write\":false},\"is_archived\":false}}}],\"cursor\":null}", "workspace-sha256" }
    };
    fixture state = { project_steps, 2, 0, 0 };
    af_client api = client(&state);
    af_project output[4]; size_t count = 0;
    check(af_list_projects(&api, output, 4, &count) == AF_OK, "project pages accepted");
    check(count == 2 && strcmp(output[0].id, "g-p-one") == 0
          && strcmp(output[1].name, "Two") == 0 && !output[1].can_write,
          "project data parsed");
    expect_done(&state);

    const step thread_steps[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/g-p-one/conversations?cursor=0", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"items\":[{\"id\":\"thread-1\"}],\"cursor\":\"next\"}", "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/gizmos/g-p-one/conversations?cursor=next", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"items\":[{\"id\":\"thread-2\"}],\"cursor\":null}", "workspace-sha256" }
    };
    state = (fixture){ thread_steps, 2, 0, 0 }; api = client(&state);
    af_thread threads[4]; count = 0;
    check(af_list_project_threads(&api, "g-p-one", threads, 4, &count) == AF_OK,
          "project thread pages accepted");
    check(count == 2 && strcmp(threads[1].id, "thread-2") == 0
          && strcmp(threads[1].project_id, "g-p-one") == 0,
          "thread identities parsed");
    expect_done(&state);
}

static void move_success_and_dry_run(void)
{
    const step dry_steps[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200, projects, "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"id\":\"thread-1\",\"gizmo_id\":null}", "workspace-sha256" }
    };
    fixture state = { dry_steps, 2, 0, 0 }; af_client api = client(&state);
    af_move_intent move = intent(0); af_move_receipt receipt;
    check(af_move_thread(&api, &move, &receipt) == AF_OK
          && receipt.verdict == AF_MOVE_DRY_RUN && state.patches == 0,
          "dry run reads and does not mutate");
    expect_done(&state);

    const step live_steps[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200, projects, "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"id\":\"thread-1\",\"gizmo_id\":null}", "workspace-sha256" },
        { AF_HTTP_PATCH, "/backend-api/conversation/thread-1", "{\"gizmo_id\":\"g-p-one\"}", AF_TRANSPORT_DELIVERED, 200,
          "{\"success\":true}", "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"id\":\"thread-1\",\"gizmo_id\":\"g-p-one\"}", "workspace-sha256" }
    };
    state = (fixture){ live_steps, 4, 0, 0 }; api = client(&state); move = intent(1);
    check(af_move_thread(&api, &move, &receipt) == AF_OK
          && receipt.verdict == AF_MOVE_VERIFIED && receipt.patch_attempts == 1
          && state.patches == 1 && strcmp(receipt.after_project_id, "g-p-one") == 0,
          "one patch plus independent postcondition verifies move");
    expect_done(&state);
}

static void fail_closed(void)
{
    af_move_receipt receipt;
    af_move_intent move = intent(1);

    const step stale_steps[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200, projects, "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"id\":\"thread-1\",\"gizmo_id\":\"g-p-other\"}", "workspace-sha256" }
    };
    fixture state = { stale_steps, 2, 0, 0 }; af_client api = client(&state);
    check(af_move_thread(&api, &move, &receipt) == AF_STALE_SOURCE
          && state.patches == 0, "stale source blocks mutation");

    const step readonly_steps[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200, readonly_projects, "workspace-sha256" }
    };
    state = (fixture){ readonly_steps, 1, 0, 0 }; api = client(&state);
    check(af_move_thread(&api, &move, &receipt) == AF_PROJECT_NOT_WRITABLE
          && state.patches == 0, "read-only destination blocks mutation");

    const step wrong_workspace[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200, projects, "different-workspace" }
    };
    state = (fixture){ wrong_workspace, 1, 0, 0 }; api = client(&state);
    check(af_move_thread(&api, &move, &receipt) == AF_WRONG_WORKSPACE
          && state.patches == 0, "workspace mismatch blocks mutation");

    const step custom_gpt[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200, projects, "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"id\":\"thread-1\",\"gizmo_id\":\"g-custom\"}", "workspace-sha256" }
    };
    state = (fixture){ custom_gpt, 2, 0, 0 }; api = client(&state);
    check(af_move_thread(&api, &move, &receipt) == AF_THREAD_INELIGIBLE
          && state.patches == 0, "custom GPT chat blocks mutation");

    move.approval_digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    state = (fixture){ NULL, 0, 0, 0 }; api = client(&state);
    check(af_move_thread(&api, &move, &receipt) == AF_NOT_AUTHORIZED
          && state.position == 0, "mismatched approval rejected before network");
}

static void ambiguous_and_false_success(void)
{
    af_move_intent move = intent(1); af_move_receipt receipt;
    const step ambiguous_applied[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200, projects, "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200, "{\"id\":\"thread-1\",\"gizmo_id\":null}", "workspace-sha256" },
        { AF_HTTP_PATCH, "/backend-api/conversation/thread-1", "{\"gizmo_id\":\"g-p-one\"}", AF_TRANSPORT_AMBIGUOUS, 0, NULL, "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200, "{\"id\":\"thread-1\",\"gizmo_id\":\"g-p-one\"}", "workspace-sha256" }
    };
    fixture state = { ambiguous_applied, 4, 0, 0 }; af_client api = client(&state);
    check(af_move_thread(&api, &move, &receipt) == AF_OK
          && receipt.verdict == AF_MOVE_VERIFIED && state.patches == 1,
          "ambiguous response can be verified without retry");

    const step false_success[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200, projects, "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200, "{\"id\":\"thread-1\",\"gizmo_id\":null}", "workspace-sha256" },
        { AF_HTTP_PATCH, "/backend-api/conversation/thread-1", "{\"gizmo_id\":\"g-p-one\"}", AF_TRANSPORT_DELIVERED, 200, "{\"success\":true}", "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/conversation/thread-1", "", AF_TRANSPORT_DELIVERED, 200, "{\"id\":\"thread-1\",\"gizmo_id\":null}", "workspace-sha256" }
    };
    state = (fixture){ false_success, 4, 0, 0 }; api = client(&state);
    check(af_move_thread(&api, &move, &receipt) == AF_PROVIDER_REJECTED
          && receipt.verdict == AF_MOVE_UNKNOWN && state.patches == 1,
          "2xx does not replace postcondition");
}

static void malformed_and_cycles(void)
{
    af_project output[4]; size_t count;
    const step malformed[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"items\":[],\"items\":[]}", "workspace-sha256" }
    };
    fixture state = { malformed, 1, 0, 0 }; af_client api = client(&state);
    check(af_list_projects(&api, output, 4, &count) == AF_SCHEMA_CHANGED,
          "duplicate JSON keys rejected");

    const step cycle[] = {
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"items\":[{\"gizmo\":{\"id\":\"g-p-one\",\"display\":{\"name\":\"One\"},\"current_user_permission\":{\"can_write\":true}}}],\"cursor\":\"same\"}", "workspace-sha256" },
        { AF_HTTP_GET, "/backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20&cursor=same", "", AF_TRANSPORT_DELIVERED, 200,
          "{\"items\":[{\"gizmo\":{\"id\":\"g-p-two\",\"display\":{\"name\":\"Two\"},\"current_user_permission\":{\"can_write\":true}}}],\"cursor\":\"same\"}", "workspace-sha256" }
    };
    state = (fixture){ cycle, 2, 0, 0 }; api = client(&state);
    check(af_list_projects(&api, output, 4, &count) == AF_CURSOR_CYCLE,
          "repeated project cursor rejected");
}

int main(void)
{
    list_primitives();
    move_success_and_dry_run();
    fail_closed();
    ambiguous_and_false_success();
    malformed_and_cycles();
    check(strcmp(af_status_name(AF_STALE_SOURCE), "stale_source") == 0,
          "status names stable");
    check(strcmp(af_move_verdict_name(AF_MOVE_VERIFIED), "verified") == 0,
          "verdict names stable");
    printf("PASS %u API-layer assertions (synthetic transport, no credentials)\n",
           assertions);
    return 0;
}

