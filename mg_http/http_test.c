#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void test_mg_http_get_request_len() {
  const char *buf = "GET /test \r\n\r\n";
  int req_len = mg_http_get_request_len(buf, strlen(buf)); // req_len == 14
  assert(req_len == 14);
}

int main() { test_mg_http_get_request_len(); }
