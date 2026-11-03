#include <gtest/gtest.h>
#include "../app/request.h"

using namespace bt;

TEST(RequestTest, RoundTrip) {
    request r;
    r.kind = request_kind::pick;
    r.url = "https://example.com/a b?c=1\nsecond line|pipe";
    r.source_hwnd = 0xABCDEF;
    r.modifiers = mod_ctrl | mod_shift;
    r.launcher_start_100ns = 133000000000000000ULL;
    r.launcher_main_100ns = 133000000000123456ULL;

    auto back = request::deserialize(r.serialize());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->kind, request_kind::pick);
    EXPECT_EQ(back->url, r.url);
    EXPECT_EQ(back->source_hwnd, r.source_hwnd);
    EXPECT_EQ(back->modifiers, r.modifiers);
    EXPECT_EQ(back->launcher_start_100ns, r.launcher_start_100ns);
    EXPECT_EQ(back->launcher_main_100ns, r.launcher_main_100ns);
}

TEST(RequestTest, EmptyUrl) {
    request r;
    r.kind = request_kind::config;
    auto back = request::deserialize(r.serialize());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->kind, request_kind::config);
    EXPECT_TRUE(back->url.empty());
}

TEST(RequestTest, RejectsGarbage) {
    EXPECT_FALSE(request::deserialize("").has_value());
    EXPECT_FALSE(request::deserialize("hello").has_value());
    EXPECT_FALSE(request::deserialize("bt1\n99\n0\n0\n0\n0\nurl").has_value());
    EXPECT_FALSE(request::deserialize("bt1\n0\n0").has_value());
}
