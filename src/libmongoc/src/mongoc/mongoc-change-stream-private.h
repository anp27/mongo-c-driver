/*
 * Copyright 2017-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if !defined(MONGOC_INSIDE) && !defined(MONGOC_COMPILATION)
#error "Only <mongoc/mongoc.h> can be included directly."
#endif

#ifndef MONGOC_CHANGE_STREAM_PRIVATE_H
#define MONGOC_CHANGE_STREAM_PRIVATE_H

#include "mongoc/mongoc-change-stream.h"
#include "mongoc/mongoc-client-session.h"
#include "mongoc/mongoc-collection.h"
#include "mongoc/mongoc-cursor.h"

typedef enum {
   MONGOC_CHANGE_STREAM_COLLECTION,
   MONGOC_CHANGE_STREAM_DATABASE,
   MONGOC_CHANGE_STREAM_CLIENT
} mongoc_change_stream_type_t;

struct _mongoc_change_stream_t {
   bson_t pipeline_to_append;
   bson_t full_document;
   bson_t opts;
   bson_t resume_token;   /* empty, or has resumeAfter: doc */
   bson_t operation_time; /* empty, or has startAtOperationTime: ts */

   bson_error_t err;
   bson_t err_doc;

   mongoc_cursor_t *cursor;

   mongoc_client_t *client;
   mongoc_read_prefs_t *read_prefs;
   mongoc_read_concern_t *read_concern;

   mongoc_change_stream_type_t change_stream_type;
   char db[140];
   char coll[140];

   int64_t max_await_time_ms;
   int32_t batch_size;

   mongoc_client_session_t *implicit_session;
};

mongoc_change_stream_t *
_mongoc_change_stream_new_from_collection (const mongoc_collection_t *coll,
                                           const bson_t *pipeline,
                                           const bson_t *opts);

mongoc_change_stream_t *
_mongoc_change_stream_new_from_database (const mongoc_database_t *db,
                                         const bson_t *pipeline,
                                         const bson_t *opts);

mongoc_change_stream_t *
_mongoc_change_stream_new_from_client (mongoc_client_t *client,
                                       const bson_t *pipeline,
                                       const bson_t *opts);

#endif /* MONGOC_CHANGE_STREAM_PRIVATE_H */
