target("http")
    set_kind("static")
    add_files("http.c")

target("http_test")
    set_kind("binary")
    add_files("http_test.c")
    add_deps("http")

