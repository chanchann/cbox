#ifndef __MG_HTTP_H
#define __MG_HTTP_H

#include <stdlib.h>

#define MG_MAX_HTTP_HEADERS 30

struct mg_str {
  const char *ptr; // Pointer to string data
  size_t len;      // String len
};

struct mg_http_header {
  struct mg_str name;  // Header name
  struct mg_str value; // Header value
};

struct mg_http_message {
  struct mg_str method, uri, query, proto;            // Request/response line
  struct mg_http_header headers[MG_MAX_HTTP_HEADERS]; // Headers
  struct mg_str body;                                 // Body
  struct mg_str head;                                 // Request + headers
  struct mg_str chunk;   // Chunk for chunked encoding,  or partial body
  struct mg_str message; // Request + headers + body
};

int mg_http_parse(const char *s, size_t len, struct mg_http_message *);
int mg_http_get_request_len(const unsigned char *buf, size_t buf_len);

struct mg_str *mg_http_get_header(struct mg_http_message *, const char *name);

#endif
