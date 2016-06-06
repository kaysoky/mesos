// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/try.hpp>
#include <stout/stringify.hpp>
#include <stout/uri.hpp>

using namespace uri;

TEST(URITest, ParseHTTP)
{
  std::string original = "https://auth.docker.com";
  Try<URI> uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("https", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("auth.docker.com", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "http://docker.com/";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("docker.com", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "http://registry.docker.com:1234/abc/1";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("registry.docker.com", uri.get().host);
  EXPECT_SOME_EQ(1234u, uri.get().port);
  EXPECT_EQ("/abc/1", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  // Missing scheme.
  EXPECT_ERROR(URI::parse("mesos.com"));
  EXPECT_ERROR(URI::parse("://///"));
  EXPECT_ERROR(URI::parse("://"));

  // Too many ports.
  EXPECT_ERROR(URI::parse("http://localhost:80:81/"));

  // Port out of range.
  EXPECT_ERROR(URI::parse("http://localhost:99999/"));
}


TEST(URITest, ParseFile)
{
  std::string original = "file:relative/path";
  Try<URI> uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("file", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_NONE(uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("relative/path", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "file:/absolute/path";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("file", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_NONE(uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/absolute/path", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "file:///host/and/absolute/path";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("file", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/host/and/absolute/path", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  // NOTE: A relative path with a host is not allowed.
  // See RFC 3986 Section 3.3.
}


TEST(URITest, ParseIPv6)
{
  std::string original = "http://[::1]/foo";
  Try<URI> uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("[::1]", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/foo", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "http://[2::1]";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("[2::1]", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "http://[1234:5:6:7:8::9]:1234";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("[1234:5:6:7:8::9]", uri.get().host);
  EXPECT_SOME_EQ(1234u, uri.get().port);
  EXPECT_EQ("", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);
}


TEST(URITest, ParseUser)
{
  std::string original = "ftp://me@awesome/";
  Try<URI> uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("ftp", uri.get().scheme);
  EXPECT_SOME_EQ("me", uri.get().user);
  EXPECT_SOME_EQ("awesome", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "ftp://admin:password@secure.com/";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("ftp", uri.get().scheme);
  EXPECT_SOME_EQ("admin:password", uri.get().user);
  EXPECT_SOME_EQ("secure.com", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "ftp://lots:of:user:info:in:a:row@weird/";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("ftp", uri.get().scheme);
  EXPECT_SOME_EQ("lots:of:user:info:in:a:row", uri.get().user);
  EXPECT_SOME_EQ("weird", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);
}


TEST(URITest, ParseQueryFragment)
{
  std::string original = "http://localhost/?query";
  Try<URI> uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("localhost", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/", uri.get().path);
  EXPECT_SOME_EQ("query", uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "http://localhost?query#fragment";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("localhost", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("", uri.get().path);
  EXPECT_SOME_EQ("query", uri.get().query);
  EXPECT_SOME_EQ("fragment", uri.get().fragment);

  original = "http://localhost#fragment";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("localhost", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_SOME_EQ("fragment", uri.get().fragment);

  original = "http://localhost#fragment?query";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("localhost", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_SOME_EQ("fragment?query", uri.get().fragment);

  original = "http://localhost:5050/#/frameworks";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("localhost", uri.get().host);
  EXPECT_SOME_EQ(5050u, uri.get().port);
  EXPECT_EQ("/", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_SOME_EQ("/frameworks", uri.get().fragment);
}


// These are the example URI's found in the `URI` struct's comment.
TEST(URITest, ParseExamples)
{
  std::string original = "ftp://ftp.is.co.za/rfc/rfc1808.txt";
  Try<URI> uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("ftp", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("ftp.is.co.za", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/rfc/rfc1808.txt", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "http://www.ietf.org/rfc/rfc2396.txt";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("http", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("www.ietf.org", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/rfc/rfc2396.txt", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "ldap://[2001:db8::7]/c=GB?objectClass?one";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("ldap", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("[2001:db8::7]", uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("/c=GB", uri.get().path);
  EXPECT_SOME_EQ("objectClass?one", uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "mailto:John.Doe@example.com";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("mailto", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_NONE(uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("John.Doe@example.com", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "news:comp.infosystems.www.servers.unix";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("news", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_NONE(uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("comp.infosystems.www.servers.unix", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "tel:+1-816-555-1212";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("tel", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_NONE(uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("+1-816-555-1212", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "telnet://192.0.2.16:80/";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("telnet", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_SOME_EQ("192.0.2.16", uri.get().host);
  EXPECT_SOME_EQ(80u, uri.get().port);
  EXPECT_EQ("/", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "urn:oasis:names:specification:docbook:dtd:xml:4.1.2";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("urn", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_NONE(uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("oasis:names:specification:docbook:dtd:xml:4.1.2", uri.get().path);
  EXPECT_NONE(uri.get().query);
  EXPECT_NONE(uri.get().fragment);

  original = "magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn";
  uri = URI::parse(original);
  ASSERT_SOME(uri);
  EXPECT_EQ(original, stringify(uri.get()));

  EXPECT_EQ("magnet", uri.get().scheme);
  EXPECT_NONE(uri.get().user);
  EXPECT_NONE(uri.get().host);
  EXPECT_NONE(uri.get().port);
  EXPECT_EQ("", uri.get().path);
  EXPECT_SOME_EQ(
      "xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn",
      uri.get().query);
  EXPECT_NONE(uri.get().fragment);
}
