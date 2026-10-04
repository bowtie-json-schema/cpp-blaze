#include <sourcemeta/core/json.h>
#include <sourcemeta/core/jsonschema.h>

#include <sourcemeta/blaze/compiler.h>
#include <sourcemeta/blaze/evaluator.h>
#include <sourcemeta/blaze/output_simple.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

enum class Output { Flag, Annotations };

namespace {

constexpr std::array<std::string_view, 3> ANNOTATION_COLLECTIONS{
    "properties", "patternProperties", "additionalProperties"};

auto is_annotation_collection(const std::string_view keyword) -> bool {
  return std::ranges::find(ANNOTATION_COLLECTIONS, keyword) !=
         ANNOTATION_COLLECTIONS.cend();
}

auto keyword_location(const std::string &location) -> std::string {
  const auto fragment{location.find('#')};
  return fragment == std::string::npos ? "#" + location
                                       : location.substr(fragment);
}

auto keyword_name(const std::string &location) -> std::string {
  const auto segment{location.rfind('/')};
  return segment == std::string::npos ? std::string{}
                                      : location.substr(segment + 1);
}

}

int main() {
  using namespace sourcemeta::core;
  using namespace sourcemeta::blaze;
  bool started{false};
  std::string default_dialect{};
  sourcemeta::blaze::Evaluator evaluator;

  std::string line;
  while (std::getline(std::cin, line)) {
    const JSON message{parse_json(line)};

    assert(message.defines("cmd"));
    assert(message.at("cmd").is_string());

    const std::string command{message.at("cmd").to_string()};

    if (command == "start") {
      started = true;
      assert(message.defines("version") && message.at("version").is_integer());
      assert(message.at("version").to_integer() == 1);

      auto response{JSON::make_object()};
      response.assign("version", JSON{1});
      auto implementation{JSON::make_object()};
      implementation.assign("language", JSON{"c++"});
      implementation.assign("version", JSON{BLAZE_VERSION});
      implementation.assign("name", JSON{"blaze"});
      implementation.assign("homepage",
                            JSON{"https://github.com/sourcemeta/blaze"});
      implementation.assign("issues",
                            JSON{"https://github.com/sourcemeta/blaze/issues"});
      implementation.assign("source",
                            JSON{"https://github.com/sourcemeta/blaze"});
      implementation.assign(
          "dialects", JSON{JSON{"https://json-schema.org/draft/2020-12/schema"},
                           JSON{"https://json-schema.org/draft/2019-09/schema"},
                           JSON{"http://json-schema.org/draft-07/schema#"},
                           JSON{"http://json-schema.org/draft-06/schema#"},
                           JSON{"http://json-schema.org/draft-04/schema#"}});

      response.assign("implementation", std::move(implementation));
      stringify(response, std::cout);
      std::cout << std::endl;
    } else if (command == "dialect") {
      assert(started);
      assert(message.defines("dialect") && message.at("dialect").is_string());
      default_dialect = message.at("dialect").to_string();
      auto response{JSON::make_object()};
      response.assign("ok", JSON{true});
      stringify(response, std::cout);
      std::cout << std::endl;
    } else if (command == "run") {
      assert(started);
      assert(message.defines("seq"));
      assert(message.defines("case") && message.at("case").is_object());
      assert(message.at("case").defines("schema") &&
             is_schema(message.at("case").at("schema")));
      assert(message.at("case").defines("tests") &&
             message.at("case").at("tests").is_array());

      auto output{Output::Flag};
      if (message.defines("output")) {
        assert(message.at("output").is_string());
        const auto requested{message.at("output").to_string()};
        assert(requested == "flag" || requested == "annotations");
        if (requested == "annotations") {
          output = Output::Annotations;
        }
      }

      std::unordered_map<std::string, sourcemeta::core::JSON> registry;
      if (message.at("case").defines("registry")) {
        assert(message.at("case").at("registry").is_object());
        for (const auto &pair : message.at("case").at("registry").as_object()) {
          registry.emplace(pair.first, pair.second);
        }
      }

      const sourcemeta::core::SchemaResolver resolver{
          [&registry](const std::string_view identifier)
              -> std::optional<sourcemeta::core::JSON> {
            const auto match{registry.find(std::string{identifier})};
            if (match != registry.cend()) {
              return match->second;
            }
            return sourcemeta::core::schema_resolver(identifier);
          }};

      try {
        const auto mode{output == Output::Annotations ? Mode::Exhaustive
                                                      : Mode::FastValidation};
        const auto schema_template{compile(
            message.at("case").at("schema"), schema_walker, resolver,
            default_schema_compiler, mode, default_dialect)};

        auto response{JSON::make_object()};
        response.assign("seq", message.at("seq"));
        response.assign("results", JSON::make_array());

        for (const auto &test : message.at("case").at("tests").as_array()) {
          assert(test.defines("instance"));
          auto test_result{JSON::make_object()};

          if (output == Output::Flag) {
            const bool valid{
                evaluator.validate(schema_template, test.at("instance"))};
            test_result.assign("valid", JSON{valid});
            response.at("results").push_back(std::move(test_result));
            continue;
          }

          SimpleOutput collected{test.at("instance")};
          const bool valid{evaluator.validate(schema_template,
                                              test.at("instance"),
                                              std::ref(collected))};
          test_result.assign("valid", JSON{valid});

          auto annotations{JSON::make_array()};
          if (valid) {
            for (const auto &entry : collected.annotations()) {
              const auto location{
                  keyword_location(entry.first.schema_location.get())};
              const auto keyword{keyword_name(location)};
              const auto &values{entry.second};

              auto annotation{JSON::make_object()};
              annotation.assign("keyword", JSON{keyword});
              annotation.assign("instanceLocation",
                                JSON{sourcemeta::core::to_string(
                                    entry.first.instance_location)});
              annotation.assign("keywordLocation", JSON{location});
              annotation.assign(
                  "annotation",
                  (is_annotation_collection(keyword) && !values.empty())
                      ? sourcemeta::core::to_json(values)
                      : JSON{values.back()});
              annotations.push_back(std::move(annotation));
            }
          }

          test_result.assign("annotations", std::move(annotations));
          response.at("results").push_back(std::move(test_result));
        }

        stringify(response, std::cout);
        std::cout << std::endl;
      } catch (const std::exception &error) {
        auto response{JSON::make_object()};
        response.assign("errored", JSON{true});
        response.assign("context", JSON::make_object());
        response.at("context").assign("message", JSON{error.what()});
        stringify(response, std::cout);
        std::cout << std::endl;
      }
    } else if (command == "stop") {
      assert(started);
      return EXIT_SUCCESS;
    } else {
      std::cerr << "Unknown command: " << command << "\n";
      return EXIT_FAILURE;
    }
  }

  return EXIT_FAILURE;
}
