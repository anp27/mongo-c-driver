#include <mongoc.h>
#include <mongoc-collection-private.h>
#include <mongoc-apm-private.h>
#include <mongoc-host-list-private.h>

#include "json-test.h"
#include "test-conveniences.h"
#include "test-libmongoc.h"


typedef struct
{
   uint32_t           n_events;
   bson_t             events;
   mongoc_uri_t      *test_framework_uri;
   int64_t            operation_id;
   bool               verbose;
} context_t;


static void
context_init (context_t *context)
{
   context->n_events = 0;
   bson_init (&context->events);
   context->test_framework_uri = test_framework_get_uri ();
   context->operation_id = 0;
   context->verbose =
      test_framework_getenv_bool ("MONGOC_TEST_MONITORING_VERBOSE");
}


static void
context_destroy (context_t *context)
{
   bson_destroy (&context->events);
   mongoc_uri_destroy (context->test_framework_uri);
}


static int
check_server_version (const bson_t *test,
                      context_t *context)
{
   const char *s;
   char *padded;
   server_version_t test_version, server_version;
   bool r;

   if (bson_has_field (test, "ignore_if_server_version_greater_than")) {
      s = bson_lookup_utf8 (test, "ignore_if_server_version_greater_than");
      /* s is like "3.0", don't skip if server is 3.0.x but skip 3.1+ */
      padded = bson_strdup_printf ("%s.99", s);
      test_version = test_framework_str_to_version (padded);
      bson_free (padded);
      server_version = test_framework_get_server_version ();
      r = server_version <= test_version;

      if (!r && context->verbose) {
         printf ("      SKIP, Server version > %s\n", s);
         fflush (stdout);
      }
   } else if (bson_has_field (test, "ignore_if_server_version_less_than")) {
      s = bson_lookup_utf8 (test, "ignore_if_server_version_less_than");
      test_version = test_framework_str_to_version (s);
      server_version = test_framework_get_server_version ();
      r = server_version >= test_version;

      if (!r && context->verbose) {
         printf ("      SKIP, Server version < %s\n", s);
         fflush (stdout);
      }
   } else {
      /* server version is ok, don't skip the test */
      return true;
   }

   return r;
}


static void
insert_data (mongoc_collection_t *collection,
             const bson_t        *test)
{
   mongoc_bulk_operation_t *bulk;
   bson_iter_t iter;
   bson_iter_t array_iter;
   bson_t doc;
   uint32_t r;
   bson_error_t error;

   if (!mongoc_collection_drop (collection, &error)) {
      if (strcmp (error.message, "ns not found")) {
         /* an error besides ns not found */
         ASSERT_OR_PRINT (false, error);
      }
   }

   bulk = mongoc_collection_create_bulk_operation (collection, true, NULL);

   BSON_ASSERT (bson_iter_init_find (&iter, test, "data"));
   BSON_ASSERT (BSON_ITER_HOLDS_ARRAY (&iter));
   bson_iter_recurse (&iter, &array_iter);

   while (bson_iter_next (&array_iter)) {
      BSON_ASSERT (BSON_ITER_HOLDS_DOCUMENT (&array_iter));
      bson_iter_bson (&array_iter, &doc);
      mongoc_bulk_operation_insert (bulk, &doc);
      bson_destroy (&doc);
   }

   r = mongoc_bulk_operation_execute (bulk, NULL, &error);
   ASSERT_OR_PRINT (r, error);

   mongoc_bulk_operation_destroy (bulk);
}


static void
check_expectations (const bson_t *events,
                    const bson_t *expectations)
{
   if (!match_bson (events, expectations, false /* is_command */)) {
      MONGOC_ERROR ("command monitoring test failed expectations:\n\n"
                    "%s\n\n"
                    "events:\n%s\n",
                    bson_as_json (expectations, NULL),
                    bson_as_json (events, NULL));

      abort ();
   }
}


static void
assert_host_in_uri (const mongoc_host_list_t *host,
                    const mongoc_uri_t       *uri)
{
   const mongoc_host_list_t *hosts;

   hosts = mongoc_uri_get_hosts (uri);
   while (hosts) {
      if (_mongoc_host_list_equal (hosts, host)) {
         return;
      }

      hosts = hosts->next;
   }

   fprintf (stderr, "Host \"%s\" not in \"%s\"",
            host->host_and_port, mongoc_uri_get_string (uri));
   fflush (stderr);
   abort ();
}


static void
started_cb (const mongoc_apm_command_started_t *event)
{
   context_t *context = (context_t *)
      mongoc_apm_command_started_get_context (event);
   int64_t operation_id;
   char *cmd_json;
   bson_t *events = &context->events;
   bson_t cmd;
   bson_iter_t iter;
   char str[16];
   const char *key;
   bson_t *new_event;

   if (context->verbose) {
      cmd_json = bson_as_json (event->command, NULL);
      printf ("%s\n", cmd_json);
      fflush (stdout);
      bson_free (cmd_json);
   }

   BSON_ASSERT (mongoc_apm_command_started_get_request_id (event) > 0);
   BSON_ASSERT (mongoc_apm_command_started_get_hint (event) > 0);
   assert_host_in_uri (event->host, context->test_framework_uri);

   /* subsequent events share the first event's operation id */
   operation_id = mongoc_apm_command_started_get_operation_id (event);
   ASSERT_CMPINT64 (operation_id, !=, (int64_t) 0);
   if (!context->operation_id) {
      context->operation_id = operation_id;
   } else {
      ASSERT_CMPINT64 (context->operation_id, ==, operation_id);
   }

   bson_uint32_to_string (context->n_events, &key, str, sizeof str);
   bson_copy_to (event->command, &cmd);

   /* special case for command monitoring JSON tests */
   if (bson_iter_init_find (&iter, &cmd, "getMore")) {
      BSON_ASSERT (BSON_ITER_HOLDS_INT64 (&iter));
      bson_iter_overwrite_int64 (&iter, 42);
   }

   new_event = BCON_NEW ("command_started_event", "{",
                         "command", BCON_DOCUMENT (&cmd),
                         "command_name", BCON_UTF8 (event->command_name),
                         "database_name", BCON_UTF8 (event->database_name),
                         "}");

   bson_uint32_to_string (context->n_events, &key, str, sizeof str);
   BSON_APPEND_DOCUMENT (events, key, new_event);

   context->n_events++;

   bson_destroy (new_event);
   bson_destroy (&cmd);
}


static void
succeeded_cb (const mongoc_apm_command_succeeded_t *event)
{
   context_t *context = (context_t *)
      mongoc_apm_command_succeeded_get_context (event);
   bson_t *events = &context->events;
   char *reply_json = bson_as_json (event->reply, NULL);
   char str[16];
   const char *key;

   BSON_ASSERT (mongoc_apm_command_succeeded_get_request_id (event) > 0);
   BSON_ASSERT (mongoc_apm_command_succeeded_get_hint (event) > 0);
   assert_host_in_uri (event->host, context->test_framework_uri);

   bson_uint32_to_string (context->n_events, &key, str, sizeof str);
   context->n_events++;

   bson_append_json (
      events,
      key,
      "{'command_succeeded_event': {"
      "    'command': %s,"
      "    'command_name': '%s'}}",
      reply_json, event->command_name);

   bson_free (reply_json);
}


static void
one_bulk_op (mongoc_bulk_operation_t *bulk,
             const bson_t            *request)
{
   bson_iter_t iter;
   const char *request_name;
   bson_t request_doc, document, filter, update;

   bson_iter_init (&iter, request);
   bson_iter_next (&iter);
   request_name = bson_iter_key (&iter);
   bson_iter_bson (&iter, &request_doc);

   if (!strcmp (request_name, "insertOne")) {
      bson_lookup_doc (&request_doc, "document", &document);
      mongoc_bulk_operation_insert (bulk, &document);
   } else if (!strcmp (request_name, "updateOne")) {
      bson_lookup_doc (&request_doc, "filter", &filter);
      bson_lookup_doc (&request_doc, "update", &update);
      mongoc_bulk_operation_update_one (bulk, &filter, &update,
                                        false /* upsert */);
   } else {
      MONGOC_ERROR ("unrecognized request name %s\n", request_name);
      abort ();
   }
}


static void
test_bulk_write (mongoc_collection_t *collection,
                 const bson_t        *arguments)
{
   bool ordered;
   mongoc_write_concern_t *wc;
   mongoc_bulk_operation_t *bulk;
   bson_iter_t requests_iter;
   bson_t requests;
   bson_t request;
   uint32_t r;
   bson_error_t error;

   ordered = bson_lookup_bool (arguments, "ordered", true);

   if (bson_has_field (arguments, "writeConcern")) {
      wc = bson_lookup_write_concern (arguments, "writeConcern");
   } else {
      wc = mongoc_write_concern_new ();
   }

   if (bson_has_field (arguments, "requests")) {
      bson_lookup_doc (arguments, "requests", &requests);
   }

   bulk = mongoc_collection_create_bulk_operation (collection, ordered, wc);
   bson_iter_init (&requests_iter, &requests);
   while (bson_iter_next (&requests_iter)) {
      bson_iter_bson (&requests_iter, &request);
      one_bulk_op (bulk, &request);
   }

   r = mongoc_bulk_operation_execute (bulk, NULL, &error);
   ASSERT_OR_PRINT (r, error);

   mongoc_bulk_operation_destroy (bulk);
   mongoc_write_concern_destroy (wc);
}


static void
test_count (mongoc_collection_t *collection,
            const bson_t        *arguments)
{
   bson_t filter;

   bson_lookup_doc (arguments, "filter", &filter);
   mongoc_collection_count (collection, MONGOC_QUERY_NONE, &filter,
                            0, 0, NULL, NULL);
}


static void
test_find (mongoc_collection_t *collection,
           const bson_t        *arguments,
           mongoc_read_prefs_t *read_prefs)
{
   bson_t query;
   bson_t filter;
   bson_t sort;
   uint32_t skip = 0;
   uint32_t limit = 0;
   uint32_t batch_size = 0;
   bson_t modifiers;
   mongoc_cursor_t *cursor;
   const bson_t *doc;

   bson_lookup_doc (arguments, "filter", &filter);

   if (read_prefs ||
       bson_has_field (arguments, "sort") ||
       bson_has_field (arguments, "modifiers")) {
      bson_init (&query);
      BSON_APPEND_DOCUMENT (&query, "$query", &filter);

      if (bson_has_field (arguments, "sort")) {
         bson_lookup_doc (arguments, "sort", &sort);
         BSON_APPEND_DOCUMENT (&query, "$orderby", &sort);
      }

      if (bson_has_field (arguments, "modifiers")) {
         bson_lookup_doc (arguments, "modifiers", &modifiers);
         bson_concat (&query, &modifiers);
      }
   } else {
      bson_copy_to (&filter, &query);
   }

   if (bson_has_field (arguments, "skip")) {
      skip = (uint32_t) bson_lookup_int64 (arguments, "skip");
   }

   if (bson_has_field (arguments, "limit")) {
      limit = (uint32_t) bson_lookup_int64 (arguments, "limit");
   }

   if (bson_has_field (arguments, "batchSize")) {
      batch_size = (uint32_t) bson_lookup_int64 (arguments, "batchSize");
   }

   cursor = mongoc_collection_find (collection, MONGOC_QUERY_NONE,
                                    skip, limit, batch_size, &query,
                                    NULL, read_prefs);

   BSON_ASSERT (cursor);
   while (mongoc_cursor_next (cursor, &doc)) {
   }

   /* can cause a killCursors command */
   mongoc_cursor_destroy (cursor);
   bson_destroy (&query);
}


static void
test_delete_many (mongoc_collection_t *collection,
                  const bson_t        *arguments)
{
   bson_t filter;

   bson_lookup_doc (arguments, "filter", &filter);
   mongoc_collection_remove (collection, MONGOC_REMOVE_NONE, &filter,
                             NULL, NULL);
}


static void
test_delete_one (mongoc_collection_t *collection,
                 const bson_t        *arguments)
{
   bson_t filter;

   bson_lookup_doc (arguments, "filter", &filter);
   mongoc_collection_remove (collection, MONGOC_REMOVE_SINGLE_REMOVE, &filter,
                             NULL, NULL);
}


static void
test_insert_many (mongoc_collection_t *collection,
                  const bson_t        *arguments)
{
   bool ordered;
   mongoc_bulk_operation_t *bulk;
   bson_t documents;
   bson_iter_t iter;
   bson_t doc;

   ordered = bson_lookup_bool (arguments, "ordered", true);
   bulk = mongoc_collection_create_bulk_operation (collection, ordered, NULL);

   bson_lookup_doc (arguments, "documents", &documents);
   bson_iter_init (&iter, &documents);
   while (bson_iter_next (&iter)) {
      bson_iter_bson (&iter, &doc);
      mongoc_bulk_operation_insert (bulk, &doc);
   }

   mongoc_bulk_operation_execute (bulk, NULL, NULL);

   mongoc_bulk_operation_destroy (bulk);
}


static void
test_insert_one (mongoc_collection_t *collection,
                 const bson_t        *arguments)
{
   bson_t document;

   bson_lookup_doc (arguments, "document", &document);
   mongoc_collection_insert (collection, MONGOC_INSERT_NONE, &document,
                             NULL, NULL);
}


static void
test_update (mongoc_collection_t *collection,
             const bson_t        *arguments,
             bool                 multi)
{
   bson_t filter;
   bson_t update;
   mongoc_update_flags_t flags = MONGOC_UPDATE_NONE;

   if (multi) {
      flags |= MONGOC_UPDATE_MULTI_UPDATE;
   }

   if (bson_lookup_bool (arguments, "upsert", false)) {
      flags |= MONGOC_UPDATE_UPSERT;
   }

   bson_lookup_doc (arguments, "filter", &filter);
   bson_lookup_doc (arguments, "update", &update);

   mongoc_collection_update (collection, flags, &filter, &update, NULL, NULL);
}


static void
test_update_many (mongoc_collection_t *collection,
                  const bson_t        *arguments)
{
   test_update (collection, arguments, true);
}


static void
test_update_one (mongoc_collection_t *collection,
                 const bson_t        *arguments)
{
   test_update (collection, arguments, false);
}


static void
one_test (mongoc_collection_t *collection,
          bson_t              *test)
{
   context_t context;
   const char *description;
   mongoc_apm_callbacks_t *callbacks;
   bson_t operation;
   bson_t arguments;
   const char *op_name;
   mongoc_read_prefs_t *read_prefs = NULL;
   bson_t expectations;

   context_init (&context);
   callbacks = mongoc_apm_callbacks_new ();

   if (test_suite_debug_output ()) {
      description = bson_lookup_utf8 (test, "description");
      printf ("  - %s\n", description);
      fflush (stdout);
   }

   if (!check_server_version (test, &context)) {
      goto done;
   }

   mongoc_apm_set_command_started_cb (callbacks, started_cb);
   mongoc_apm_set_command_succeeded_cb (callbacks, succeeded_cb);
   mongoc_client_set_apm_callbacks (collection->client, callbacks, &context);

   bson_lookup_doc (test, "operation", &operation);
   op_name = bson_lookup_utf8 (&operation, "name");
   bson_lookup_doc (&operation, "arguments", &arguments);

   if (bson_has_field (&operation, "read_preference")) {
      read_prefs = bson_lookup_read_prefs (&operation, "read_preference");
   }

   if (!strcmp (op_name, "bulkWrite")) {
      test_bulk_write (collection, &arguments);
   } else if (!strcmp (op_name, "count")) {
      test_count (collection, &arguments);
   } else if (!strcmp (op_name, "find")) {
      test_find (collection, &arguments, read_prefs);
   } else if (!strcmp (op_name, "deleteMany")) {
      test_delete_many (collection, &arguments);
   } else if (!strcmp (op_name, "deleteOne")) {
      test_delete_one (collection, &arguments);
   } else if (!strcmp (op_name, "insertMany")) {
      test_insert_many (collection, &arguments);
   } else if (!strcmp (op_name, "insertOne")) {
      test_insert_one (collection, &arguments);
   } else if (!strcmp (op_name, "updateMany")) {
      test_update_many (collection, &arguments);
   } else if (!strcmp (op_name, "updateOne")) {
      test_update_one (collection, &arguments);
   } else {
      MONGOC_ERROR ("unrecognized operation name %s\n", op_name);
      abort ();
   }

   bson_lookup_doc (test, "expectations", &expectations);
   check_expectations (&context.events, &expectations);

done:
   mongoc_client_set_apm_callbacks (collection->client, NULL, NULL);
   mongoc_apm_callbacks_destroy (callbacks);
   context_destroy (&context);
}


/*
 *-----------------------------------------------------------------------
 *
 * test_command_monitoring_cb --
 *
 *       Runs the JSON tests included with the Command Monitoring spec.
 *
 *-----------------------------------------------------------------------
 */

static void
test_command_monitoring_cb (bson_t *scenario)
{
   mongoc_client_t *client;
   const char *db_name;
   const char *collection_name;
   bson_iter_t iter;
   bson_iter_t tests_iter;
   bson_t test_op;
   mongoc_collection_t *collection;

   BSON_ASSERT (scenario);

   db_name = bson_lookup_utf8 (scenario, "database_name");
   collection_name = bson_lookup_utf8 (scenario, "collection_name");

   client = test_framework_client_new ();
   collection = mongoc_client_get_collection (client, db_name, collection_name);

   BSON_ASSERT (bson_iter_init_find (&iter, scenario, "tests"));
   BSON_ASSERT (BSON_ITER_HOLDS_ARRAY (&iter));
   bson_iter_recurse (&iter, &tests_iter);

   while (bson_iter_next (&tests_iter)) {
      insert_data (collection, scenario);
      bson_iter_bson (&tests_iter, &test_op);
      one_test (collection, &test_op);
   }
}


/*
 *-----------------------------------------------------------------------
 *
 * Runner for the JSON tests for command monitoring.
 *
 *-----------------------------------------------------------------------
 */
static void
test_all_spec_tests (TestSuite *suite)
{
   char resolved[PATH_MAX];

   if (realpath ("tests/json/command_monitoring", resolved)) {
      install_json_test_suite (suite, resolved, &test_command_monitoring_cb);
   }
}

void
test_command_monitoring_install (TestSuite *suite)
{
   test_all_spec_tests (suite);
}
