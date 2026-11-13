// Tests for services::http::BodyFramer (include/services/http_body_framing.h):
// identity and chunked HTTP/1.1 body decoding, error reporting and drain().
#include "test_framework.h"

#include <random>
#include <string>

#include "services/http_body_framing.h"

using services::http::BodyFramer;
using services::http::BodyFraming;

namespace {

/** Byte source over a fixed string, matching the Source concept. */
struct StringSource {
  explicit StringSource(std::string d) : data(std::move(d)) {}
  int read() {
    if (pos >= data.size()) return -1;
    return static_cast<unsigned char>(data[pos++]);
  }
  std::string data;
  size_t pos = 0;
};

std::string readAll(BodyFramer<StringSource>& f) {
  std::string out;
  for (int c = f.read(); c >= 0; c = f.read()) out.push_back(static_cast<char>(c));
  return out;
}

std::string decode(const std::string& wire, BodyFraming framing, int len = -1) {
  StringSource src(wire);
  BodyFramer<StringSource> f(src, framing, len);
  return readAll(f);
}

/** Encode `payload` as a chunked body with the given chunk sizes (cycled). */
std::string chunkEncode(const std::string& payload, const std::vector<size_t>& sizes,
                        bool upper_hex = false) {
  std::string out;
  size_t pos = 0;
  size_t i = 0;
  while (pos < payload.size()) {
    const size_t n = std::min(sizes[i++ % sizes.size()], payload.size() - pos);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), upper_hex ? "%zX\r\n" : "%zx\r\n", n);
    out += hdr;
    out += payload.substr(pos, n);
    out += "\r\n";
    pos += n;
  }
  out += "0\r\n\r\n";
  return out;
}

}  // namespace

// ---------------- identity ----------------

TEST(identity_with_content_length_reads_exactly_that_many_bytes) {
  StringSource src("hello world EXTRA");
  BodyFramer<StringSource> f(src, BodyFraming::kIdentity, 11);
  CHECK_STREQ(readAll(f), "hello world");
  CHECK(f.complete());
  CHECK(!f.truncated());
  CHECK(!f.framingError());
  CHECK_EQ(f.bytesRead(), size_t{11});
  CHECK_EQ(src.pos, size_t{11});  // must not over-read the socket
}

TEST(identity_zero_content_length_is_immediately_complete) {
  StringSource src("ignored");
  BodyFramer<StringSource> f(src, BodyFraming::kIdentity, 0);
  CHECK_EQ(f.read(), -1);
  CHECK(f.complete());
  CHECK_EQ(src.pos, size_t{0});
}

TEST(identity_without_length_reads_until_close) {
  StringSource src("{\"ac\":[]}");
  BodyFramer<StringSource> f(src, BodyFraming::kIdentity, -1);
  CHECK_STREQ(readAll(f), "{\"ac\":[]}");
  CHECK(f.complete());
  CHECK(!f.truncated());
}

TEST(identity_short_read_is_truncated) {
  StringSource src("abc");
  BodyFramer<StringSource> f(src, BodyFraming::kIdentity, 10);
  CHECK_STREQ(readAll(f), "abc");
  CHECK(f.truncated());
  CHECK(!f.complete());
  CHECK(!f.framingError());
}

TEST(identity_read_after_end_keeps_returning_eof) {
  StringSource src("ab");
  BodyFramer<StringSource> f(src, BodyFraming::kIdentity, 2);
  readAll(f);
  CHECK_EQ(f.read(), -1);
  CHECK_EQ(f.read(), -1);
}

TEST(identity_drain_without_length_does_not_block_or_read) {
  StringSource src("abcdef");
  BodyFramer<StringSource> f(src, BodyFraming::kIdentity, -1);
  CHECK_EQ(f.read(), 'a');
  f.drain();  // close-delimited: drain is a no-op
  CHECK_EQ(src.pos, size_t{1});
}

TEST(identity_drain_with_length_consumes_rest_of_body_only) {
  StringSource src("abcdefNEXT");
  BodyFramer<StringSource> f(src, BodyFraming::kIdentity, 6);
  CHECK_EQ(f.read(), 'a');
  f.drain();
  CHECK(f.complete());
  CHECK_EQ(src.pos, size_t{6});
}

TEST(identity_handles_high_bytes_without_sign_extension) {
  const std::string wire("\xff\x80\x00z", 4);
  StringSource src(wire);
  BodyFramer<StringSource> f(src, BodyFraming::kIdentity, 4);
  CHECK_EQ(f.read(), 0xff);
  CHECK_EQ(f.read(), 0x80);
  CHECK_EQ(f.read(), 0x00);
  CHECK_EQ(f.read(), 'z');
}

// ---------------- chunked: happy paths ----------------

TEST(chunked_basic_two_chunks) {
  CHECK_STREQ(decode("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", BodyFraming::kChunked),
              "hello world");
}

TEST(chunked_sets_complete_and_byte_count) {
  StringSource src("3\r\nabc\r\n0\r\n\r\nNEXT");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, 999);  // length ignored
  CHECK_STREQ(readAll(f), "abc");
  CHECK(f.complete());
  CHECK_EQ(f.bytesRead(), size_t{3});
  CHECK_EQ(src.data.substr(src.pos), std::string("NEXT"));  // stops at message end
}

TEST(chunked_uppercase_and_mixed_case_hex) {
  const std::string payload(0x1A + 0xb, 'x');
  CHECK_STREQ(decode("1A\r\n" + payload.substr(0, 0x1A) + "\r\nb\r\n" +
                         payload.substr(0x1A) + "\r\n0\r\n\r\n",
                     BodyFraming::kChunked),
              payload);
}

TEST(chunked_leading_zeros_in_size) {
  CHECK_STREQ(decode("0003\r\nabc\r\n0\r\n\r\n", BodyFraming::kChunked), "abc");
}

TEST(chunked_extensions_are_ignored) {
  CHECK_STREQ(decode("3;name=value;x\r\nabc\r\n0;last\r\n\r\n", BodyFraming::kChunked),
              "abc");
}

TEST(chunked_trailer_fields_are_skipped) {
  StringSource src("3\r\nabc\r\n0\r\nX-Checksum: 1\r\nX-Other: 2\r\n\r\nNEXT");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_STREQ(readAll(f), "abc");
  CHECK(f.complete());
  CHECK_EQ(src.data.substr(src.pos), std::string("NEXT"));
}

TEST(chunked_bare_lf_line_endings_are_tolerated) {
  CHECK_STREQ(decode("3\nabc\n2\nde\n0\n\n", BodyFraming::kChunked), "abcde");
}

TEST(chunked_payload_may_contain_crlf_and_digits) {
  const std::string payload = "\r\n0\r\n\r\n";
  CHECK_STREQ(decode(chunkEncode(payload, {7}), BodyFraming::kChunked), payload);
}

TEST(chunked_peer_closes_after_zero_chunk_is_still_complete) {
  StringSource src("3\r\nabc\r\n0\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_STREQ(readAll(f), "abc");
  CHECK(f.complete());
  CHECK(!f.truncated());
}

TEST(chunked_empty_body) {
  StringSource src("0\r\n\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_EQ(f.read(), -1);
  CHECK(f.complete());
  CHECK_EQ(f.bytesRead(), size_t{0});
}

TEST(chunked_readBytes_spans_chunks_and_stops_at_end) {
  StringSource src(chunkEncode("abcdefghij", {3}));
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  char buf[64] = {};
  CHECK_EQ(f.readBytes(buf, 4), size_t{4});
  CHECK_STREQ(std::string(buf, 4), "abcd");
  CHECK_EQ(f.readBytes(buf, sizeof(buf)), size_t{6});
  CHECK_STREQ(std::string(buf, 6), "efghij");
  CHECK_EQ(f.readBytes(buf, sizeof(buf)), size_t{0});
}

TEST(chunked_drain_consumes_terminator_and_trailer) {
  StringSource src("2\r\n{}\r\n0\r\nT: v\r\n\r\nNEXT");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_EQ(f.read(), '{');
  CHECK_EQ(f.read(), '}');
  CHECK(!f.complete());  // parser stopped before the zero chunk
  f.drain();
  CHECK(f.complete());
  CHECK_EQ(src.data.substr(src.pos), std::string("NEXT"));
}

TEST(chunked_max_eight_hex_digits_accepted) {
  // 8 digits is legal (32-bit size); the body is then cut short.
  StringSource src("00000002\r\nab\r\n0\r\n\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_STREQ(readAll(f), "ab");
  CHECK(f.complete());
}

TEST(chunked_random_payloads_round_trip_for_any_chunking) {
  std::mt19937 rng(1234);
  for (int iter = 0; iter < 300; ++iter) {
    const size_t len = rng() % 2000;
    std::string payload(len, '\0');
    for (auto& ch : payload) ch = static_cast<char>(rng() & 0xff);
    std::vector<size_t> sizes;
    for (int k = 0; k < 5; ++k) sizes.push_back(1 + rng() % 300);
    const bool upper = (iter & 1) != 0;
    StringSource src(chunkEncode(payload, sizes, upper));
    BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
    const std::string got = readAll(f);
    if (got != payload || !f.complete() || f.bytesRead() != len) {
      testfw::fail(__FILE__, __LINE__,
                   "round trip mismatch at iteration " + std::to_string(iter));
      return;
    }
  }
}

// ---------------- chunked: errors ----------------

TEST(chunked_size_line_must_start_with_hex) {
  StringSource src("\r\nabc\r\n0\r\n\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_EQ(f.read(), -1);
  CHECK(f.framingError());
  CHECK(!f.complete());
}

TEST(chunked_non_hex_in_size_is_malformed) {
  StringSource src("3g\r\nabc\r\n0\r\n\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_EQ(f.read(), -1);
  CHECK(f.framingError());
}

TEST(chunked_more_than_eight_hex_digits_is_malformed) {
  StringSource src("000000003\r\nabc\r\n0\r\n\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_EQ(f.read(), -1);
  CHECK(f.framingError());
}

TEST(chunked_missing_crlf_after_data_is_malformed) {
  StringSource src("3\r\nabcX\r\n0\r\n\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_STREQ(readAll(f), "abc");
  CHECK(f.framingError());
  CHECK(!f.complete());
}

TEST(chunked_cr_not_followed_by_lf_after_data_is_malformed) {
  StringSource src("3\r\nabc\rX0\r\n\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_STREQ(readAll(f), "abc");
  CHECK(f.framingError());
}

TEST(chunked_cut_off_mid_data_is_truncated_not_malformed) {
  StringSource src("a\r\nabc");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_STREQ(readAll(f), "abc");
  CHECK(f.truncated());
  CHECK(!f.framingError());
  CHECK(!f.complete());
}

TEST(chunked_cut_off_in_size_line_is_truncated) {
  for (const char* wire : {"", "3", "3\r", "3;ext", "3\r\nabc", "3\r\nabc\r"}) {
    StringSource src(wire);
    BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
    readAll(f);
    if (!f.truncated() || f.framingError()) {
      testfw::fail(__FILE__, __LINE__, std::string("not truncated: ") + wire);
    }
  }
}

TEST(chunked_errors_are_sticky) {
  StringSource src("zz\r\n3\r\nabc\r\n0\r\n\r\n");
  BodyFramer<StringSource> f(src, BodyFraming::kChunked, -1);
  CHECK_EQ(f.read(), -1);
  const size_t pos = src.pos;
  CHECK_EQ(f.read(), -1);
  f.drain();
  CHECK_EQ(src.pos, pos);  // no further reads after an error
}
