## mongoose http

核心结构就两个，一个是header，一个是http message信息

此处mg_str就是封装了一下字符串及其长度

```
struct mg_str {
  const char *ptr; // Pointer to string data
  size_t len;      // String len
};
```

```cpp
struct mg_http_header {
  struct mg_str name;   // Header name
  struct mg_str value;  // Header value
};

struct mg_http_message {
  struct mg_str method, uri, query, proto;             // Request/response line
  struct mg_http_header headers[MG_MAX_HTTP_HEADERS];  // Headers
  struct mg_str body;                                  // Body
  struct mg_str head;                                  // Request + headers
  struct mg_str chunk;    // Chunk for chunked encoding,  or partial body
  struct mg_str message;  // Request + headers + body
};
```

## parse部分

```cpp
int mg_http_parse(const char *s, size_t len, struct mg_http_message *hm) {
  // 这里获取的是 request-line + header 的长度
  // GET方法没有body，而POST则有，我们可以直接offset到body部分
  int is_response, req_len = mg_http_get_request_len((unsigned char *) s, len);
  // 如果有body，偏移req_len 指向body
  const char *end = s == NULL ? NULL : s + req_len, *qs;  // Cannot add to NULL
  struct mg_str *cl;

  memset(hm, 0, sizeof(*hm));
  if (req_len <= 0) return req_len;

  hm->message.ptr = hm->head.ptr = s;
  hm->body.ptr = end;
  hm->head.len = (size_t) req_len;
  hm->chunk.ptr = end;
  // 此时还没解析body, 暂时无法给出具体值
  hm->message.len = hm->body.len = (size_t) ~0;  // Set body length to infinite

  // Parse request line
  s = skip(s, end, " ", &hm->method);
  s = skip(s, end, " ", &hm->uri);
  s = skip(s, end, "\r\n", &hm->proto);

  // Sanity check. Allow protocol/reason to be empty
  if (hm->method.len == 0 || hm->uri.len == 0) return -1;

  // If URI contains '?' character, setup query string
  // memchr : 参数 str 所指向的字符串的前 n 个字节中搜索第一次出现字符 c（一个无符号字符）的位置。
  // https://www.runoob.com/cprogramming/c-function-memchr.html
  // 如果url中包含?, 说明url中存在查询参数，多个kv对根据 & 分割
  if ((qs = (const char *) memchr(hm->uri.ptr, '?', hm->uri.len)) != NULL) {
    // 跳过 ? 
    hm->query.ptr = qs + 1;
    // 获取query的参数列表长度
    hm->query.len = (size_t) (&hm->uri.ptr[hm->uri.len] - (qs + 1));
    // 计算uri的长度
    hm->uri.len = (size_t) (qs - hm->uri.ptr);
  }
  // 总结下
  /* 
           |uri.len = sizeof(|\|)
  |GET|(sp)|/|?|key1=val1&key2=val2|(sp)|http1.1|
             | |----query.len------|
            qs qs+1
  */

  // 开始解析头部
  mg_http_parse_headers(s, end, hm->headers,
                        sizeof(hm->headers) / sizeof(hm->headers[0]));
  if ((cl = mg_http_get_header(hm, "Content-Length")) != NULL) {
    // 如果找到了Content-Length， 则知道了body的长度和整个报文的长度了
    hm->body.len = (size_t) mg_to64(*cl);
    hm->message.len = (size_t) req_len + hm->body.len;
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
  // 我们可以通过method来判断是request 还是 response
  is_response = mg_ncasecmp(hm->method.ptr, "HTTP/", 5) == 0;
  if (hm->body.len == (size_t) ~0 && !is_response &&
      mg_vcasecmp(&hm->method, "PUT") != 0 &&
      mg_vcasecmp(&hm->method, "POST") != 0) {
    hm->body.len = 0;
    hm->message.len = (size_t) req_len;
  }

  // The 204 (No content) responses also have 0 body length
  if (hm->body.len == (size_t) ~0 && is_response &&
      mg_vcasecmp(&hm->uri, "204") == 0) {
    hm->body.len = 0;
    hm->message.len = (size_t) req_len;
  }

  return req_len;
}
```

```cpp
static bool isok(uint8_t c) { return c == '\n' || c == '\r' || c >= ' '; }

// The length of request is a number of bytes till the end of HTTP headers. It does not include length of HTTP body.
int mg_http_get_request_len(const unsigned char *buf, size_t buf_len) {
  size_t i;
  for (i = 0; i < buf_len; i++) {
    // 若存在不可打印的非法字符，直接结束
    if (!isok(buf[i])) return -1;
    // 找到header结束的位置
    // 此处也支持了 \n\n 这种非标准http
    if ((i > 0 && buf[i] == '\n' && buf[i - 1] == '\n') ||
        (i > 3 && buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n'))
      return (int) i + 1;
  }
  return 0;
}
```

```cpp
// s = skip(s, end, " ", &hm->method);
static const char *skip(const char *s, const char *e, const char *d,
                        struct mg_str *v) {
  v->ptr = s;
  while (s < e && *s != '\n' && strchr(d, *s) == NULL)
    s++;
  // 获取属性具体长度
  v->len = (size_t)(s - v->ptr);
  // 跳过sp
  while (s < e && strchr(d, *s) != NULL)
    s++;
  return s;
}
```

```cpp
// http中有0-多个header
// #define MG_MAX_HTTP_HEADERS 30
// 我们我们最大解析30个header
static void mg_http_parse_headers(const char *s, const char *end,
                                  struct mg_http_header *h, int max_headers) {
  int i;
  for (i = 0; i < max_headers; i++) {
    struct mg_str k, v, tmp;
    // 跳过http的起始行
    const char *he = skip(s, end, "\n", &tmp);
    // 找到header的key
    s = skip(s, he, ": \r\n", &k);
    // 找到value
    s = skip(s, he, "\r\n", &v);
    // TODO : Why do this
    if (k.len == tmp.len) continue;
    while (v.len > 0 && v.ptr[v.len - 1] == ' ') v.len--;  // Trim spaces
    // key 长度为0， 则解析失败
    if (k.len == 0) break;
    // MG_INFO(("--HH [%.*s] [%.*s] [%.*s]", (int) tmp.len - 1, tmp.ptr,
    //(int) k.len, k.ptr, (int) v.len, v.ptr));
    h[i].name = k;
    h[i].value = v;
  }
}
```

```cpp
// mg_http_get_header(hm, "Content-Length")
struct mg_str *mg_http_get_header(struct mg_http_message *h, const char *name) {
  // n 是 key的长度
  size_t i, n = strlen(name), max = sizeof(h->headers) / sizeof(h->headers[0]);
  // 做遍历
  for (i = 0; i < max && h->headers[i].name.len > 0; i++) {
    struct mg_str *k = &h->headers[i].name, *v = &h->headers[i].value;
    // 先比较长度，再cmp
    if (n == k->len && mg_ncasecmp(k->ptr, name, n) == 0) return v;
  }
  return NULL;
}
```

```cpp
int mg_ncasecmp(const char *s1, const char *s2, size_t len) {
  int diff = 0;
  if (len > 0) do {
      diff = mg_lower(s1++) - mg_lower(s2++);
    } while (diff == 0 && s1[-1] != '\0' && --len > 0);
  return diff;
}

int mg_lower(const char *s) {
  int c = *s;
  if (c >= 'A' && c <= 'Z')
    c += 'a' - 'A';
  return c;
}
```

