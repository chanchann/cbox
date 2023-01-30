#include "http.h"
#include <stdint.h>
#include <string.h>

// -------------- str util

static int mg_lower(const char *s) {
  int c = *s;
  if (c >= 'A' && c <= 'Z')
    c += 'a' - 'A';
  return c;
}

static int mg_ncasecmp(const char *s1, const char *s2, size_t len) {
  int diff = 0;
  if (len > 0)
    do {
      diff = mg_lower(s1++) - mg_lower(s2++);
    } while (diff == 0 && s1[-1] != '\0' && --len > 0);
  return diff;
}

static int mg_vcasecmp(const struct mg_str *str1, const char *str2) {
  size_t n2 = strlen(str2), n1 = str1->len;
  int r = mg_ncasecmp(str1->ptr, str2, (n1 < n2) ? n1 : n2);
  if (r == 0)
    return (int)(n1 - n2);
  return r;
}

static int64_t mg_to64(struct mg_str str) {
  int64_t result = 0, neg = 1, max = 922337203685477570 /* INT64_MAX/10-10 */;
  size_t i = 0;
  while (i < str.len && (str.ptr[i] == ' ' || str.ptr[i] == '\t'))
    i++;
  if (i < str.len && str.ptr[i] == '-')
    neg = -1, i++;
  while (i < str.len && str.ptr[i] >= '0' && str.ptr[i] <= '9') {
    if (result > max)
      return 0;
    result *= 10;
    result += (str.ptr[i] - '0');
    i++;
  }
  return result * neg;
}

static const char *skip(const char *s, const char *e, const char *d,
                        struct mg_str *v) {
  v->ptr = s;
  while (s < e && *s != '\n' && strchr(d, *s) == NULL)
    s++;
  v->len = (size_t)(s - v->ptr);
  while (s < e && strchr(d, *s) != NULL)
    s++;
  return s;
}

static void mg_http_parse_headers(const char *s, const char *end,
                                  struct mg_http_header *h, int max_headers) {
  int i;
  for (i = 0; i < max_headers; i++) {
    struct mg_str k, v, tmp;
    // todo : why \n and :\r\n
    const char *he = skip(s, end, "\n", &tmp);
    s = skip(s, he, ": \r\n", &k);
    s = skip(s, he, "\r\n", &v);
    if (k.len == tmp.len)
      continue;
    while (v.len > 0 && v.ptr[v.len - 1] == ' ')
      v.len--; // Trim spaces
    if (k.len == 0)
      break;
    // MG_INFO(("--HH [%.*s] [%.*s] [%.*s]", (int) tmp.len - 1, tmp.ptr,
    //(int) k.len, k.ptr, (int) v.len, v.ptr));
    h[i].name = k;
    h[i].value = v;
  }
}

int mg_http_parse(const char *s, size_t len, struct mg_http_message *hm) {
  int is_response, req_len = mg_http_get_request_len((unsigned char *)s, len);
  const char *end = s == NULL ? NULL : s + req_len, *qs; // Cannot add to NULL
  struct mg_str *cl;

  memset(hm, 0, sizeof(*hm));
  if (req_len <= 0)
    return req_len;

  hm->message.ptr = hm->head.ptr = s;
  hm->body.ptr = end;
  hm->head.len = (size_t)req_len;
  hm->chunk.ptr = end;
  hm->message.len = hm->body.len = (size_t)~0; // Set body length to infinite

  // Parse request line
  s = skip(s, end, " ", &hm->method);
  s = skip(s, end, " ", &hm->uri);
  s = skip(s, end, "\r\n", &hm->proto);

  // Sanity check. Allow protocol/reason to be empty
  if (hm->method.len == 0 || hm->uri.len == 0)
    return -1;

  // If URI contains '?' character, setup query string
  if ((qs = (const char *)memchr(hm->uri.ptr, '?', hm->uri.len)) != NULL) {
    hm->query.ptr = qs + 1;
    hm->query.len = (size_t)(&hm->uri.ptr[hm->uri.len] - (qs + 1));
    hm->uri.len = (size_t)(qs - hm->uri.ptr);
  }

  mg_http_parse_headers(s, end, hm->headers,
                        sizeof(hm->headers) / sizeof(hm->headers[0]));
  if ((cl = mg_http_get_header(hm, "Content-Length")) != NULL) {
    hm->body.len = (size_t)mg_to64(*cl);
    hm->message.len = (size_t)req_len + hm->body.len;
  }

  // mg_http_parse() is used to parse both HTTP requests and HTTP
  // responses. If HTTP response does not have Content-Length set, then
  // body is read until socket is closed, i.e. body.len is infinite (~0).
  //
  // For HTTP requests though, according to
  // http://tools.ietf.org/html/rfc7231#section-8.1.3,
  // only POST and PUT methods have defined body semantics.
  // Therefore, if Content-Length is not specified and methods are
  // not one of PUT or POST, set body length to 0.
  //
  // So, if it is HTTP request, and Content-Length is not set,
  // and method is not (PUT or POST) then reset body length to zero.
  is_response = mg_ncasecmp(hm->method.ptr, "HTTP/", 5) == 0;
  if (hm->body.len == (size_t)~0 && !is_response &&
      mg_vcasecmp(&hm->method, "PUT") != 0 &&
      mg_vcasecmp(&hm->method, "POST") != 0) {
    hm->body.len = 0;
    hm->message.len = (size_t)req_len;
  }

  // The 204 (No content) responses also have 0 body length
  if (hm->body.len == (size_t)~0 && is_response &&
      mg_vcasecmp(&hm->uri, "204") == 0) {
    hm->body.len = 0;
    hm->message.len = (size_t)req_len;
  }

  return req_len;
}

static int isok(uint8_t c) { return c == '\n' || c == '\r' || c >= ' '; }

int mg_http_get_request_len(const unsigned char *buf, size_t buf_len) {
  size_t i;
  for (i = 0; i < buf_len; i++) {
    if (!isok(buf[i]))
      return -1;
    if (i > 4 && buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' &&
        buf[i - 3] == '\r')
      return (int)i + 1;
  }
  return 0;
}

struct mg_str *mg_http_get_header(struct mg_http_message *h, const char *name) {
  size_t i, n = strlen(name), max = sizeof(h->headers) / sizeof(h->headers[0]);
  for (i = 0; i < max && h->headers[i].name.len > 0; i++) {
    struct mg_str *k = &h->headers[i].name, *v = &h->headers[i].value;
    if (n == k->len && mg_ncasecmp(k->ptr, name, n) == 0)
      return v;
  }
  return NULL;
}
