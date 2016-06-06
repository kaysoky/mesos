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

#ifndef __STOUT_URI_HPP__
#define __STOUT_URI_HPP__

#include <stdint.h>

#include <memory>
#include <string>

#include <uriparser/Uri.h>

#include <stout/lambda.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

namespace uri {

/**
 * Represents a Uniform Resource Identifier (URI).
 * RFC 3986 (https://www.ietf.org/rfc/rfc3986.txt)
 *
 * scheme:[//[user:password@]host[:port]]path[?query][#fragment]
 *
 * Examples:
 *   ftp://ftp.is.co.za/rfc/rfc1808.txt
 *   http://www.ietf.org/rfc/rfc2396.txt
 *   ldap://[2001:db8::7]/c=GB?objectClass?one
 *   mailto:John.Doe@example.com
 *   news:comp.infosystems.www.servers.unix
 *   tel:+1-816-555-1212
 *   telnet://192.0.2.16:80/
 *   urn:oasis:names:specification:docbook:dtd:xml:4.1.2
 *   magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn
 */
struct URI
{
  static Try<URI> parse(const std::string& value)
  {
    UriParserStateA state;
    UriUriA uri;
    URI result;

    // NOTE: This is used to deallocate the dynamic bits of `UriUriA`
    // when the `parse` function returns.
    auto guard = std::shared_ptr<UriUriA>(&uri, [](UriUriA* uri) {
      uriFreeUriMembersA(uri);
    });

    state.uri = &uri;
    if (uriParseUriA(&state, value.c_str()) != URI_SUCCESS) {
      return Error("Failed to parse uri string");
    }

    // Unpack the parsed URI piece by piece into the `URI` struct.

    // NOTE: There is a difference between not setting a field and
    // setting a field with an empty string. We treat them differently
    // when generating the actual URI string. See more details in the
    // RFC. For instance, the RFC states that the 'path' component is
    // required, but may be empty (no characters).

    if (uri.scheme.first == nullptr) {
      return Error("Missing scheme in uri string");
    }

    result.scheme = std::string(
        uri.scheme.first, uri.scheme.afterLast - uri.scheme.first);

    if (uri.userInfo.first != nullptr) {
      result.user = std::string(
          uri.userInfo.first, uri.userInfo.afterLast - uri.userInfo.first);
    }

    if (uri.hostText.first != nullptr) {
      result.host = std::string(
          uri.hostText.first, uri.hostText.afterLast - uri.hostText.first);

      // NOTE: The uriparser library does not include the surrounding brackets
      // for an IPv6 host.
      if (uri.hostData.ip6 != nullptr ||
          uri.hostData.ipFuture.first != nullptr) {
        result.host = "[" + result.host.get() + "]";
      }
    }

    const std::string _port(
        uri.portText.first, uri.portText.afterLast - uri.portText.first);

    Option<uint16_t> port;
    if (!_port.empty()) {
      Try<uint16_t> numifyPort = numify<uint16_t>(_port);
      if (numifyPort.isError()) {
        return Error("Failed to parse port: " + numifyPort.error());
      }

      result.port = numifyPort.get();
    }

    if (uri.pathHead != nullptr) {
      // NOTE: The uriparser library omits the leading slash in the `path`.
      // It is somewhat counter-intuitive when the `path` of an URI
      // like "http://foo.bar/baz" is "baz" rather than "/baz".
      // Therefore, we prepend the leading slash when required.
      if (uri.absolutePath || result.host.isSome()) {
        result.path = "/" + result.path;
      }

      result.path += std::string(
          uri.pathHead->text.first,
          uri.pathTail->text.afterLast - uri.pathHead->text.first);
    }

    if (uri.query.first != nullptr) {
      result.query = std::string(
        uri.query.first, uri.query.afterLast - uri.query.first);
    }

    if (uri.fragment.first != nullptr) {
      result.fragment = std::string(
        uri.fragment.first, uri.fragment.afterLast - uri.fragment.first);
    }

    return result;
  }


  std::string scheme;
  Option<std::string> user;
  Option<std::string> host;
  Option<uint16_t> port;
  std::string path;
  Option<std::string> query;
  Option<std::string> fragment;
};

inline std::ostream& operator<<(std::ostream& stream, const URI& uri)
{
  stream << uri.scheme << ":";

  // The 'authority' part.
  if (uri.host.isSome()) {
    stream << "//";

    if (uri.user.isSome()) {
      stream << uri.user.get() << "@";
    }

    stream << uri.host.get();

    if (uri.port.isSome()) {
      stream << ":" << uri.port.get();
    }
  }

  // The 'path' part.
  stream << uri.path;

  // The 'query' part.
  if (uri.query.isSome()) {
    stream << "?" << uri.query.get();
  }

  // The 'fragment' part.
  if (uri.fragment.isSome()) {
    stream << "#" << uri.fragment.get();
  }

  return stream;
}

} // namespace uri {

#endif // __STOUT_URI_HPP__
